#include "syncaudio/core/device_descriptor.h"

#include <windows.h>
#include <iostream>

int wmain() {
    const HRESULT initialization = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialization)) {
        std::wcerr << L"Unable to initialize COM (0x" << std::hex << static_cast<unsigned long>(initialization) << L").\n";
        return 1;
    }

    const auto devices = syncaudio::enumerate_render_endpoints();
    std::wcout << L"SyncAudio — detected Windows render endpoints\n\n";
    if (devices.empty()) {
        std::wcout << L"No render endpoints were returned by MMDevice. Check Windows Sound settings.\n";
    }
    for (const auto& device : devices) {
        std::wcout << (device.is_default_multimedia ? L"* " : L"  ")
                   << (device.name.empty() ? L"(unnamed endpoint)" : device.name)
                   << L"\n    State: " << syncaudio::to_string(device.state)
                   << L"\n    Shared mix format: " << device.sample_rate << L" Hz, "
                   << device.channels << L" channel(s)"
                   << L"\n    ID: " << device.id << L"\n";
    }
    std::wcout << L"\n* Default multimedia render endpoint\n";
    CoUninitialize();
    return 0;
}
