#pragma once

#include <string>

namespace syncaudio {

struct EndpointVolumeState {
    float scalar{1.0F};
    bool muted{false};
};

// These controls operate on the real Windows render endpoint. They are used by
// the dashboard before and during routing; no values are simulated in the UI.
[[nodiscard]] EndpointVolumeState endpoint_volume_state(const std::wstring& endpoint_id);
void set_endpoint_volume(const std::wstring& endpoint_id, float scalar);
void set_endpoint_mute(const std::wstring& endpoint_id, bool muted);

} // namespace syncaudio

