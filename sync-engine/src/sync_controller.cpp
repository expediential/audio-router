#include "syncaudio/core/sync_controller.h"

#include <algorithm>
#include <cmath>

namespace syncaudio {

DriftController::DriftController(DriftControllerConfig config) : config_(config) {}

double DriftController::update(double buffered_frames, double target_frames) noexcept {
    // Occupancy is the observable safe proxy for independent endpoint clocks.
    // The correction is intentionally bounded to a few hundred ppm so it is
    // inaudible and cannot turn a transient scheduling delay into a pitch jump.
    if (target_frames <= 0.0) return ratio_;
    const double normalized_error = (buffered_frames - target_frames) / target_frames;
    integral_ = std::clamp(integral_ + normalized_error, -config_.integral_limit, config_.integral_limit);
    const double requested_ppm = (config_.proportional * normalized_error +
                                  config_.integral * integral_) * 1'000'000.0;
    const double bounded_ppm = std::clamp(requested_ppm, -config_.maximum_ppm, config_.maximum_ppm);
    ratio_ = 1.0 + bounded_ppm / 1'000'000.0;
    return ratio_;
}

void DriftController::reset() noexcept { integral_ = 0.0; ratio_ = 1.0; }

} // namespace syncaudio

