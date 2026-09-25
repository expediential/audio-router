#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace syncaudio {

enum class EndpointState { active, disabled, unplugged, not_present, unknown };

struct DeviceDescriptor {
    std::wstring id;
    std::wstring name;
    std::wstring interface_name;
    EndpointState state{EndpointState::unknown};
    bool is_default_multimedia{false};
    std::uint32_t channels{0};
    std::uint32_t sample_rate{0};
};

// Enumerates real MMDevice render endpoints. It deliberately does not infer a
// Bluetooth codec/profile from the friendly name; that is not reliable API data.
[[nodiscard]] std::vector<DeviceDescriptor> enumerate_render_endpoints();

[[nodiscard]] const wchar_t* to_string(EndpointState state) noexcept;

} // namespace syncaudio

