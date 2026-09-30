cmake_minimum_required(VERSION 3.25)
project(SyncAudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# Keep the distributable CLI independent of a separately installed Visual C++
# runtime. Windows Core Audio itself remains provided by Windows.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(syncaudio-core
    audio-engine/include/syncaudio/core/device_descriptor.h
    audio-engine/include/syncaudio/core/endpoint_volume.h
    audio-engine/include/syncaudio/core/ring_buffer.h
    audio-engine/include/syncaudio/core/sync_controller.h
    audio-engine/include/syncaudio/core/wasapi_mirror.h
    audio-engine/src/device_enumerator.cpp
    audio-engine/src/endpoint_volume.cpp
    audio-engine/src/wasapi_mirror.cpp
    sync-engine/src/sync_controller.cpp)

target_include_directories(syncaudio-core PUBLIC audio-engine/include)
target_compile_definitions(syncaudio-core PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(syncaudio-core PUBLIC ole32 uuid)
target_compile_options(syncaudio-core PRIVATE /W4 /permissive- /EHsc)

add_executable(syncaudio-cli app/cli/main.cpp)
target_link_libraries(syncaudio-cli PRIVATE syncaudio-core)
target_compile_options(syncaudio-cli PRIVATE /W4 /permissive- /EHsc)

add_executable(syncaudio-app WIN32 app/win32/main.cpp)
target_link_libraries(syncaudio-app PRIVATE syncaudio-core comctl32 dwmapi uxtheme msimg32)
target_compile_definitions(syncaudio-app PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_compile_options(syncaudio-app PRIVATE /W4 /permissive- /EHsc)

include(CTest)
if(BUILD_TESTING)
    add_executable(syncaudio-sync-tests tests/sync_controller_test.cpp)
    target_link_libraries(syncaudio-sync-tests PRIVATE syncaudio-core)
    target_compile_options(syncaudio-sync-tests PRIVATE /W4 /permissive- /EHsc)
    add_test(NAME syncaudio.sync-controller COMMAND syncaudio-sync-tests)
endif()
