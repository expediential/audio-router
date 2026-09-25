#include "syncaudio/core/device_descriptor.h"

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <memory>

namespace syncaudio {
namespace {

using Microsoft::WRL::ComPtr;

EndpointState map_state(DWORD state) noexcept {
    if (state & DEVICE_STATE_ACTIVE) return EndpointState::active;
    if (state & DEVICE_STATE_DISABLED) return EndpointState::disabled;
    if (state & DEVICE_STATE_UNPLUGGED) return EndpointState::unplugged;
    if (state & DEVICE_STATE_NOTPRESENT) return EndpointState::not_present;
    return EndpointState::unknown;
}

std::wstring property_string(IPropertyStore* store, const PROPERTYKEY& key) {
    PROPVARIANT value{};
    PropVariantInit(&value);
    const HRESULT result = store->GetValue(key, &value);
    std::wstring text;
    if (SUCCEEDED(result) && value.vt == VT_LPWSTR && value.pwszVal != nullptr) text = value.pwszVal;
    PropVariantClear(&value);
    return text;
}

} // namespace

const wchar_t* to_string(EndpointState state) noexcept {
    switch (state) {
    case EndpointState::active: return L"Active";
    case EndpointState::disabled: return L"Disabled";
    case EndpointState::unplugged: return L"Unplugged";
    case EndpointState::not_present: return L"Not present";
    default: return L"Unknown";
    }
}

std::vector<DeviceDescriptor> enumerate_render_endpoints() {
    std::vector<DeviceDescriptor> devices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) return devices;

    std::wstring default_id;
    ComPtr<IMMDevice> default_device;
    if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, &default_device))) {
        LPWSTR id = nullptr;
        if (SUCCEEDED(default_device->GetId(&id))) { default_id = id; CoTaskMemFree(id); }
    }

    ComPtr<IMMDeviceCollection> collection;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATEMASK_ALL, &collection))) return devices;
    UINT count = 0;
    collection->GetCount(&count);
    devices.reserve(count);
    for (UINT index = 0; index < count; ++index) {
        ComPtr<IMMDevice> device;
        if (FAILED(collection->Item(index, &device))) continue;
        DWORD state = 0;
        device->GetState(&state);
        LPWSTR raw_id = nullptr;
        if (FAILED(device->GetId(&raw_id))) continue;
        DeviceDescriptor descriptor;
        descriptor.id = raw_id;
        CoTaskMemFree(raw_id);
        descriptor.state = map_state(state);
        descriptor.is_default_multimedia = descriptor.id == default_id;
        ComPtr<IPropertyStore> store;
        if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store))) {
            descriptor.name = property_string(store.Get(), PKEY_Device_FriendlyName);
            descriptor.interface_name = property_string(store.Get(), PKEY_DeviceInterface_FriendlyName);
        }
        // GetMixFormat reports the actual shared-mode mix format that this
        // endpoint exposes today. A device can be enumerated while unavailable,
        // so activation failure is represented as an unknown (zero) format.
        ComPtr<IAudioClient> audio_client;
        if (SUCCEEDED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                       reinterpret_cast<void**>(audio_client.GetAddressOf())))) {
            WAVEFORMATEX* format = nullptr;
            if (SUCCEEDED(audio_client->GetMixFormat(&format)) && format != nullptr) {
                descriptor.channels = format->nChannels;
                descriptor.sample_rate = format->nSamplesPerSec;
                CoTaskMemFree(format);
            }
        }
        devices.push_back(std::move(descriptor));
    }
    return devices;
}

} // namespace syncaudio
