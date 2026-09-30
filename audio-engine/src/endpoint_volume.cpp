#include "syncaudio/core/endpoint_volume.h"

#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <stdexcept>

namespace syncaudio {
namespace {

using Microsoft::WRL::ComPtr;

void check(HRESULT result, const char* action) {
    if (FAILED(result)) throw std::runtime_error(action);
}

ComPtr<IAudioEndpointVolume> endpoint_volume(const std::wstring& endpoint_id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(&enumerator)), "Could not create Windows audio device enumerator.");
    ComPtr<IMMDevice> device;
    check(enumerator->GetDevice(endpoint_id.c_str(), &device), "The selected audio endpoint is no longer available.");
    ComPtr<IAudioEndpointVolume> volume;
    check(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                           reinterpret_cast<void**>(volume.GetAddressOf())),
          "Could not open the selected endpoint volume control.");
    return volume;
}

} // namespace

EndpointVolumeState endpoint_volume_state(const std::wstring& endpoint_id) {
    auto volume = endpoint_volume(endpoint_id);
    EndpointVolumeState state;
    check(volume->GetMasterVolumeLevelScalar(&state.scalar), "Could not read endpoint volume.");
    BOOL muted = FALSE;
    check(volume->GetMute(&muted), "Could not read endpoint mute state.");
    state.muted = muted != FALSE;
    return state;
}

void set_endpoint_volume(const std::wstring& endpoint_id, float scalar) {
    auto volume = endpoint_volume(endpoint_id);
    check(volume->SetMasterVolumeLevelScalar(std::clamp(scalar, 0.0F, 1.0F), nullptr),
          "Could not set endpoint volume.");
}

void set_endpoint_mute(const std::wstring& endpoint_id, bool muted) {
    auto volume = endpoint_volume(endpoint_id);
    check(volume->SetMute(muted ? TRUE : FALSE, nullptr), "Could not set endpoint mute state.");
}

} // namespace syncaudio

