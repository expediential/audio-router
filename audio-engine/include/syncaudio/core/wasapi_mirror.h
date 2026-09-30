#pragma once

#include "syncaudio/core/device_descriptor.h"
#include "syncaudio/core/ring_buffer.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace syncaudio {

// The observable portion of a shared-mode mix format required by the first
// zero-conversion mirror path. Format adapters/SRC will sit in front of this
// renderer in the multi-device milestone.
struct FloatStreamFormat {
    std::uint32_t sample_rate{0};
    std::uint16_t channels{0};
    std::uint16_t bits_per_sample{0};

    [[nodiscard]] bool is_float32() const noexcept {
        return sample_rate != 0 && channels != 0 && bits_per_sample == 32;
    }
};

struct MirrorStatistics {
    std::uint64_t captured_frames{0};
    std::uint64_t rendered_frames{0};
    std::uint64_t capture_overflow_frames{0};
    std::uint64_t render_underrun_frames{0};
};

// A real, one-destination WASAPI mirror. It captures the default multimedia
// render endpoint via shared-mode loopback and renders that shared mix to an
// explicitly selected *different* endpoint. Both endpoints must currently use
// compatible Float32 formats. This constraint is enforced, not hidden: a later
// FormatAdapter will handle channel mapping and high-quality async SRC.
class WasapiMirror final {
public:
    explicit WasapiMirror(std::wstring destination_endpoint_id);
    ~WasapiMirror();
    WasapiMirror(const WasapiMirror&) = delete;
    WasapiMirror& operator=(const WasapiMirror&) = delete;

    // Initializes endpoints and checks compatibility. It does not begin audio.
    void prepare();
    void start();
    // Services whichever Core Audio event is signaled. Returns false after a
    // timeout; callers can use that to update UI/diagnostics without polling.
    bool service_once(std::uint32_t timeout_ms);
    void stop() noexcept;

    [[nodiscard]] FloatStreamFormat source_format() const noexcept;
    [[nodiscard]] FloatStreamFormat destination_format() const noexcept;
    [[nodiscard]] MirrorStatistics statistics() const noexcept;

private:
    class Implementation;
    std::unique_ptr<Implementation> implementation_;
};

} // namespace syncaudio

