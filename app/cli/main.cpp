#include "syncaudio/core/device_descriptor.h"
#include "syncaudio/core/wasapi_mirror.h"

#include <windows.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <limits>

namespace {

std::atomic_bool keep_running{true};

BOOL WINAPI on_console_control(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        keep_running.store(false, std::memory_order_relaxed);
        return TRUE;
    }
    return FALSE;
}

void print_usage() {
    std::wcout << L"Usage:\n"
               << L"  syncaudio-cli                         List actual render endpoints\n"
               << L"  syncaudio-cli --mirror <endpoint-id> [seconds]\n\n"
               << L"--mirror captures the default multimedia output using WASAPI loopback and\n"
               << L"mirrors it to one explicitly selected different endpoint. The current\n"
               << L"real path requires compatible Float32 shared-mode formats.\n";
}

[[nodiscard]] bool launched_directly_from_explorer() noexcept {
    // A process launched by Explorer is the sole process attached to its new
    // console. A normal PowerShell/cmd invocation shares that console with its
    // parent, so commands and automated checks remain non-interactive.
    DWORD process_ids[2]{};
    return GetConsoleProcessList(process_ids, static_cast<DWORD>(std::size(process_ids))) == 1;
}

void wait_before_explorer_exit(bool should_wait) {
    if (!should_wait) return;
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD console_mode = 0;
    if (input == INVALID_HANDLE_VALUE || input == nullptr || !GetConsoleMode(input, &console_mode)) return;
    std::wcout << L"\nPress Enter to close SyncAudio.\n";
    std::wcin.ignore((std::numeric_limits<std::streamsize>::max)(), L'\n');
    std::wcin.get();
}

int run_mirror(const std::wstring& endpoint_id, std::uint32_t seconds) {
    syncaudio::WasapiMirror mirror(endpoint_id);
    mirror.prepare();
    const auto source = mirror.source_format();
    const auto destination = mirror.destination_format();
    std::wcout << L"Ready: " << source.sample_rate << L" Hz, " << source.channels
               << L" channel Float32 loopback -> compatible destination.\n";
    std::wcout << L"Mirroring for " << seconds << L" second(s). Press Ctrl+C to stop safely.\n";
    mirror.start();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (keep_running.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < deadline) {
        mirror.service_once(1'000);
    }
    mirror.stop();
    const auto statistics = mirror.statistics();
    std::wcout << L"Stopped. Captured: " << statistics.captured_frames
               << L", rendered: " << statistics.rendered_frames
               << L", source overflow: " << statistics.capture_overflow_frames
               << L", destination underrun: " << statistics.render_underrun_frames << L".\n";
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[]) {
    const HRESULT initialization = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialization)) {
        std::wcerr << L"Unable to initialize COM (0x" << std::hex << static_cast<unsigned long>(initialization) << L").\n";
        return 1;
    }
    SetConsoleCtrlHandler(on_console_control, TRUE);

    const bool launched_directly = launched_directly_from_explorer();
    int result = 0;
    try {
        if (argc >= 3 && std::wstring_view(argv[1]) == L"--mirror") {
            const auto seconds = argc >= 4 ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 60U;
            result = run_mirror(argv[2], seconds);
        } else if (argc > 1 && std::wstring_view(argv[1]) != L"--list") {
            print_usage();
            result = 2;
        } else {
            const auto devices = syncaudio::enumerate_render_endpoints();
            std::wcout << L"SyncAudio - detected Windows render endpoints\n\n";
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
            if (launched_directly) {
                std::wcout << L"\nTo mirror system audio, open PowerShell in this folder and run:\n"
                           << L"  .\\SyncAudio.exe --mirror '<non-default endpoint ID>' 60\n";
            }
        }
    } catch (const std::exception& error) {
        std::wcerr << L"SyncAudio error: " << error.what() << L"\n";
        result = 1;
    }
    wait_before_explorer_exit(launched_directly);
    CoUninitialize();
    return result;
}
