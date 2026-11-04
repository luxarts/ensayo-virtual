#pragma once
#include <functional>
#include <thread>
#include <atomic>
#include <cstdint>
namespace ensayo
{
// Thread (MMCSS "Pro Audio", CreateWaitableTimerExW high resolution, absolute deadlines) calling fn every periodUs.
class HighResClock
{
public:
    HighResClock (double periodMicroseconds, std::function<void()> fn);
    ~HighResClock();
    void start(); void stop();
    struct Stats { double maxLateUs = 0, meanLateUs = 0; uint64_t ticks = 0; };
    Stats getStats() const noexcept;
private:
    void run();

    const double periodUs;
    const std::function<void()> callback;
    std::thread thread;
    std::atomic<bool> running { false };
    void* stopEvent = nullptr;                       // HANDLE, signalled by stop()
    std::atomic<double> maxLateUs { 0.0 }, totalLateUs { 0.0 };
    std::atomic<uint64_t> ticks { 0 };

    HighResClock (const HighResClock&) = delete;
    HighResClock& operator= (const HighResClock&) = delete;
};
}
