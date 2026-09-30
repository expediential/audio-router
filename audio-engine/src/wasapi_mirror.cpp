#include "syncaudio/core/wasapi_mirror.h"

#include <windows.h>
#include <audioclient.h>
#include <ks.h>
#include <ksmedia.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string_view>
#include <vector>

namespace syncaudio {
namespace {

using Microsoft::WRL::ComPtr;

[[noreturn]] void throw_hresult(std::wstring_view operation, HRESULT result) {
    const int byte_count = WideCharToMultiByte(CP_UTF8, 0, operation.data(),
                                               static_cast<int>(operation.size()),
                                               nullptr, 0, nullptr, nullptr);
    std::string operation_utf8(static_cast<std::size_t>(byte_count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, operation.data(), static_cast<int>(operation.size()),
                        operation_utf8.data(), byte_count, nullptr, nullptr);
    throw std::runtime_error(std::format("{} failed (HRESULT 0x{:08X})", operation_utf8,
                                         static_cast<unsigned long>(result)));
}

void check(HRESULT result, std::wstring_view operation) {
    if (FAILED(result)) throw_hresult(operation, result);
}

FloatStreamFormat inspect_format(const WAVEFORMATEX& format) {
    bool is_float = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto& extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE&>(format);
        is_float = extensible.SubFormat == KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    }
    if (!is_float) return {};
    return {format.nSamplesPerSec, format.nChannels, format.wBitsPerSample};
}

bool compatible(const FloatStreamFormat& source, const FloatStreamFormat& destination) noexcept {
    return source.is_float32() && destination.is_float32() &&
           source.sample_rate == destination.sample_rate && source.channels == destination.channels;
}

struct EventHandle {
    HANDLE value{nullptr};
    ~EventHandle() { if (value != nullptr) CloseHandle(value); }
    EventHandle() = default;
    EventHandle(const EventHandle&) = delete;
    EventHandle& operator=(const EventHandle&) = delete;
};

} // namespace

class WasapiMirror::Implementation {
public:
    explicit Implementation(std::wstring target) : destination_id(std::move(target)) {}

    void prepare() {
        if (prepared) return;
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(&enumerator)), L"Create MMDevice enumerator");
        check(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &source_device),
              L"Get default multimedia source");
        LPWSTR source_raw_id = nullptr;
        check(source_device->GetId(&source_raw_id), L"Read source endpoint ID");
        source_id = source_raw_id;
        CoTaskMemFree(source_raw_id);
        if (source_id == destination_id) {
            throw std::runtime_error("The destination must differ from the loopback source to prevent feedback.");
        }
        check(enumerator->GetDevice(destination_id.c_str(), &destination_device), L"Open destination endpoint");

        check(source_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                      reinterpret_cast<void**>(source_client.GetAddressOf())),
              L"Activate source IAudioClient");
        check(destination_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                           reinterpret_cast<void**>(destination_client.GetAddressOf())),
              L"Activate destination IAudioClient");

        WAVEFORMATEX* source_mix = nullptr;
        WAVEFORMATEX* destination_mix = nullptr;
        check(source_client->GetMixFormat(&source_mix), L"Read source mix format");
        check(destination_client->GetMixFormat(&destination_mix), L"Read destination mix format");
        source_stream_format = inspect_format(*source_mix);
        destination_stream_format = inspect_format(*destination_mix);
        CoTaskMemFree(source_mix);
        CoTaskMemFree(destination_mix);
        if (!compatible(source_stream_format, destination_stream_format)) {
            throw std::runtime_error("This first mirror path requires both endpoints to use the same Float32 shared-mode sample rate and channel count. Choose a compatible endpoint or wait for the format-adapter milestone.");
        }

        source_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        destination_event.value = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (source_event.value == nullptr || destination_event.value == nullptr) {
            throw std::runtime_error("Unable to create WASAPI event handles.");
        }

        // Loopback is available only in shared mode. On modern Windows it can
        // be event-driven, avoiding timer/Sleep based servicing.
        WAVEFORMATEX source_format{};
        source_format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        source_format.nChannels = source_stream_format.channels;
        source_format.nSamplesPerSec = source_stream_format.sample_rate;
        source_format.wBitsPerSample = source_stream_format.bits_per_sample;
        source_format.nBlockAlign = static_cast<WORD>(source_format.nChannels * sizeof(float));
        source_format.nAvgBytesPerSec = source_format.nSamplesPerSec * source_format.nBlockAlign;
        check(source_client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                        0, 0, &source_format, nullptr),
              L"Initialize loopback capture");
        check(source_client->SetEventHandle(source_event.value), L"Set loopback event handle");
        check(source_client->GetService(IID_PPV_ARGS(&capture_client)), L"Get IAudioCaptureClient");

        WAVEFORMATEX destination_format = source_format;
        check(destination_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
                                             0, 0, &destination_format, nullptr),
              L"Initialize destination render");
        check(destination_client->SetEventHandle(destination_event.value), L"Set render event handle");
        check(destination_client->GetService(IID_PPV_ARGS(&render_client)), L"Get IAudioRenderClient");

        UINT32 source_buffer_frames = 0;
        UINT32 destination_buffer_frames = 0;
        check(source_client->GetBufferSize(&source_buffer_frames), L"Read source buffer size");
        check(destination_client->GetBufferSize(&destination_buffer_frames), L"Read destination buffer size");
        silent_frames.assign(source_buffer_frames * source_stream_format.channels, 0.0F);
        // A bounded 250 ms reservoir gives an independently serviced output
        // room to absorb normal scheduling variability without hiding a fault.
        const auto reservoir_frames = (std::max)(destination_buffer_frames * 4U,
            static_cast<UINT32>(source_stream_format.sample_rate / 4));
        ring = std::make_unique<FloatFrameRingBuffer>(reservoir_frames, source_stream_format.channels);
        prepared = true;
    }

    void start() {
        if (!prepared) prepare();
        if (started) return;
        // Prefill the device buffer with silence before capture begins. The
        // first captured frame thus enters a known, bounded queue state.
        fill_render_buffer(true);
        check(destination_client->Start(), L"Start destination render");
        check(source_client->Start(), L"Start loopback capture");
        started = true;
    }

    bool service_once(std::uint32_t timeout_ms) {
        if (!started) throw std::logic_error("Mirror must be started before servicing.");
        HANDLE handles[] = {source_event.value, destination_event.value};
        const DWORD wait = WaitForMultipleObjects(static_cast<DWORD>(std::size(handles)), handles, FALSE, timeout_ms);
        if (wait == WAIT_TIMEOUT) return false;
        if (wait == WAIT_FAILED) throw std::runtime_error("Waiting for WASAPI event failed.");
        if (wait == WAIT_OBJECT_0) drain_capture();
        else if (wait == WAIT_OBJECT_0 + 1) fill_render_buffer(false);
        else throw std::runtime_error("Unexpected WASAPI wait result.");
        return true;
    }

    void stop() noexcept {
        if (!started) return;
        source_client->Stop();
        destination_client->Stop();
        source_client->Reset();
        destination_client->Reset();
        started = false;
    }

    void drain_capture() {
        UINT32 packets = 0;
        check(capture_client->GetNextPacketSize(&packets), L"Read loopback packet size");
        while (packets != 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            check(capture_client->GetBuffer(&data, &frames, &flags, nullptr, nullptr), L"Read loopback packet");
            const auto sample_count = static_cast<std::size_t>(frames) * source_stream_format.channels;
            std::size_t accepted = 0;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0) {
                accepted = ring->write(std::span<const float>(silent_frames.data(), sample_count));
            } else {
                accepted = ring->write(std::span<const float>(reinterpret_cast<const float*>(data), sample_count));
            }
            statistics.captured_frames.fetch_add(frames, std::memory_order_relaxed);
            statistics.capture_overflow_frames.fetch_add(frames - accepted, std::memory_order_relaxed);
            check(capture_client->ReleaseBuffer(frames), L"Release loopback packet");
            check(capture_client->GetNextPacketSize(&packets), L"Read next loopback packet size");
        }
    }

    void fill_render_buffer(bool full_prefill) {
        UINT32 padding = 0;
        check(destination_client->GetCurrentPadding(&padding), L"Read destination padding");
        UINT32 total = 0;
        check(destination_client->GetBufferSize(&total), L"Read destination buffer size");
        const UINT32 writable = full_prefill ? total : total - padding;
        if (writable == 0) return;
        BYTE* data = nullptr;
        check(render_client->GetBuffer(writable, &data), L"Acquire destination buffer");
        const auto samples = static_cast<std::size_t>(writable) * destination_stream_format.channels;
        std::span<float> output(reinterpret_cast<float*>(data), samples);
        const auto copied = full_prefill ? 0U : static_cast<UINT32>(ring->read(output));
        if (copied < writable) {
            std::fill(output.begin() + static_cast<std::size_t>(copied) * destination_stream_format.channels,
                      output.end(), 0.0F);
            if (!full_prefill) statistics.render_underrun_frames.fetch_add(writable - copied, std::memory_order_relaxed);
        }
        statistics.rendered_frames.fetch_add(writable, std::memory_order_relaxed);
        check(render_client->ReleaseBuffer(writable, 0), L"Release destination buffer");
    }

    struct AtomicStatistics {
        std::atomic<std::uint64_t> captured_frames{0};
        std::atomic<std::uint64_t> rendered_frames{0};
        std::atomic<std::uint64_t> capture_overflow_frames{0};
        std::atomic<std::uint64_t> render_underrun_frames{0};
    } statistics;
    std::wstring destination_id;
    std::wstring source_id;
    ComPtr<IMMDevice> source_device;
    ComPtr<IMMDevice> destination_device;
    ComPtr<IAudioClient> source_client;
    ComPtr<IAudioClient> destination_client;
    ComPtr<IAudioCaptureClient> capture_client;
    ComPtr<IAudioRenderClient> render_client;
    EventHandle source_event;
    EventHandle destination_event;
    std::unique_ptr<FloatFrameRingBuffer> ring;
    std::vector<float> silent_frames;
    FloatStreamFormat source_stream_format{};
    FloatStreamFormat destination_stream_format{};
    bool prepared{false};
    bool started{false};
};

WasapiMirror::WasapiMirror(std::wstring destination_endpoint_id)
    : implementation_(std::make_unique<Implementation>(std::move(destination_endpoint_id))) {}
WasapiMirror::~WasapiMirror() { stop(); }
void WasapiMirror::prepare() { implementation_->prepare(); }
void WasapiMirror::start() { implementation_->start(); }
bool WasapiMirror::service_once(std::uint32_t timeout_ms) { return implementation_->service_once(timeout_ms); }
void WasapiMirror::stop() noexcept { if (implementation_) implementation_->stop(); }
FloatStreamFormat WasapiMirror::source_format() const noexcept { return implementation_->source_stream_format; }
FloatStreamFormat WasapiMirror::destination_format() const noexcept { return implementation_->destination_stream_format; }
MirrorStatistics WasapiMirror::statistics() const noexcept {
    const auto& stats = implementation_->statistics;
    return {stats.captured_frames.load(std::memory_order_relaxed), stats.rendered_frames.load(std::memory_order_relaxed),
            stats.capture_overflow_frames.load(std::memory_order_relaxed), stats.render_underrun_frames.load(std::memory_order_relaxed)};
}

} // namespace syncaudio
