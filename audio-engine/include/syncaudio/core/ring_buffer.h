#pragma once

#include <atomic>
#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace syncaudio {

// Bounded SPSC buffer for interleaved float frames. One capture fan-out writer
// and one endpoint render reader own each instance. It never allocates after
// construction and reports saturation instead of blocking a real-time thread.
class FloatFrameRingBuffer {
public:
    FloatFrameRingBuffer(std::size_t capacity_frames, std::size_t channels)
        : capacity_frames_(capacity_frames), channels_(channels),
          samples_(capacity_frames * channels) {}

    [[nodiscard]] std::size_t channels() const noexcept { return channels_; }
    [[nodiscard]] std::size_t capacity_frames() const noexcept { return capacity_frames_; }

    [[nodiscard]] std::size_t available_to_read() const noexcept {
        const auto write = write_frame_.load(std::memory_order_acquire);
        const auto read = read_frame_.load(std::memory_order_relaxed);
        return write - read;
    }

    [[nodiscard]] std::size_t available_to_write() const noexcept {
        return capacity_frames_ - available_to_read();
    }

    // Copies whole interleaved frames. Returns the number written.
    std::size_t write(std::span<const float> frames) noexcept {
        if (channels_ == 0 || frames.size() % channels_ != 0) return 0;
        const std::size_t requested = frames.size() / channels_;
        const std::size_t count = (std::min)(requested, available_to_write());
        auto write = write_frame_.load(std::memory_order_relaxed);
        for (std::size_t f = 0; f < count; ++f) {
            const auto slot = ((write + f) % capacity_frames_) * channels_;
            for (std::size_t c = 0; c < channels_; ++c) samples_[slot + c] = frames[f * channels_ + c];
        }
        write_frame_.store(write + count, std::memory_order_release);
        return count;
    }

    // Copies up to requested frames. Silence fill belongs to the renderer so
    // starvation remains visible in diagnostics instead of being hidden here.
    std::size_t read(std::span<float> frames) noexcept {
        if (channels_ == 0 || frames.size() % channels_ != 0) return 0;
        const std::size_t requested = frames.size() / channels_;
        const std::size_t count = (std::min)(requested, available_to_read());
        auto read = read_frame_.load(std::memory_order_relaxed);
        for (std::size_t f = 0; f < count; ++f) {
            const auto slot = ((read + f) % capacity_frames_) * channels_;
            for (std::size_t c = 0; c < channels_; ++c) frames[f * channels_ + c] = samples_[slot + c];
        }
        read_frame_.store(read + count, std::memory_order_release);
        return count;
    }

private:
    std::size_t capacity_frames_;
    std::size_t channels_;
    std::vector<float> samples_;
    std::atomic<std::size_t> write_frame_{0};
    std::atomic<std::size_t> read_frame_{0};
};

} // namespace syncaudio
