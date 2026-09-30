#include "syncaudio/core/sync_controller.h"
#include <cmath>

int main() {
    syncaudio::DriftController controller;
    const auto neutral = controller.update(480.0, 480.0);
    if (neutral != 1.0) return 1;
    const auto bounded = controller.update(480'000.0, 480.0);
    if (bounded > 1.0003 || bounded < 0.9997) return 2;
    controller.reset();
    if (controller.ratio() != 1.0) return 3;
    return 0;
}
