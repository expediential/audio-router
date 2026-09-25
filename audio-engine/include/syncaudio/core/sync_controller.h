#pragma once

namespace syncaudio {

struct DriftControllerConfig {
    double proportional{0.0008};
    double integral{0.00004};
    double integral_limit{2.0};
    double maximum_ppm{300.0};
};

class DriftController {
public:
    explicit DriftController(DriftControllerConfig config = {});
    [[nodiscard]] double update(double buffered_frames, double target_frames) noexcept;
    void reset() noexcept;
    [[nodiscard]] double ratio() const noexcept { return ratio_; }

private:
    DriftControllerConfig config_;
    double integral_{0.0};
    double ratio_{1.0};
};

} // namespace syncaudio

