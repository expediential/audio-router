#include "syncaudio/core/sync_controller.h"
#include <cassert>

int main() {
    syncaudio::DriftController controller;
    const auto neutral = controller.update(480.0, 480.0);
    assert(neutral == 1.0);
    const auto bounded = controller.update(480'000.0, 480.0);
    assert(bounded <= 1.0003);
    assert(bounded >= 0.9997);
}

