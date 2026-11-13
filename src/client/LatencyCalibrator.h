#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <vector>
namespace ensayo
{
class LatencyCalibrator
{
public:
    static constexpr int clicks = 8, periodSamples = 24000;   // a click every 0.5 s
    void start() noexcept;                                     // message thread
    // Audio thread: writes the click train to out (both channels), scans `in` for each click (threshold 0.1 after
    // 20 ms of quiet), records the round trip in samples. Done after `clicks` clicks or 6 s.
    void process (const float* in, float* outL, float* outR, int n) noexcept;
    bool isRunning() const noexcept;
    std::optional<int> roundTrip() const;                     // median; nullopt if fewer than 5 clicks detected
    static int calibrationFor (int roundTrip, int inputLatency, int outputLatency) noexcept
        { return roundTrip - inputLatency - outputLatency; }

private:
    static constexpr int firstClickSamples = 12000;           // 0.25 s
    static constexpr int quietSamples = 960;                  // 20 ms
    static constexpr int maxSamples = 6 * 48000;
    static constexpr float clickLevel = 0.9f, threshold = 0.1f;

    std::atomic<bool> running { false };
    std::atomic<int> detected { 0 };
    std::array<std::atomic<int>, clicks> trips {};
    // Audio-thread state (reset by start() before `running` is published).
    int64_t position = 0, lastClickAt = -1;
    int emitted = 0, quietRun = 0;
    bool lastClickDetected = true;
};
}
