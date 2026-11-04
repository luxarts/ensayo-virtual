#include "HighResClock.h"

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <avrt.h>
#include <timeapi.h>

#include <algorithm>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
 #define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

namespace ensayo
{
namespace
{
    int64_t preciseNow100ns() noexcept
    {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime (&ft);
        ULARGE_INTEGER value;
        value.LowPart = ft.dwLowDateTime;
        value.HighPart = ft.dwHighDateTime;
        return (int64_t) value.QuadPart;
    }
}

HighResClock::HighResClock (double periodMicroseconds, std::function<void()> fn)
    : periodUs (periodMicroseconds > 0.0 ? periodMicroseconds : 1000.0), callback (std::move (fn))
{
    stopEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
}

HighResClock::~HighResClock()
{
    stop();

    if (stopEvent != nullptr)
        CloseHandle ((HANDLE) stopEvent);
}

void HighResClock::start()
{
    if (running.exchange (true))
        return;

    ResetEvent ((HANDLE) stopEvent);
    maxLateUs.store (0.0);
    totalLateUs.store (0.0);
    ticks.store (0);
    thread = std::thread ([this] { run(); });
}

void HighResClock::stop()
{
    if (! running.exchange (false))
        return;

    SetEvent ((HANDLE) stopEvent);

    if (thread.joinable())
        thread.join();
}

HighResClock::Stats HighResClock::getStats() const noexcept
{
    Stats stats;
    stats.ticks = ticks.load();
    stats.maxLateUs = maxLateUs.load();
    stats.meanLateUs = stats.ticks > 0 ? totalLateUs.load() / (double) stats.ticks : 0.0;
    return stats;
}

void HighResClock::run()
{
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);   // failure is harmless

    bool usedTimePeriod = false;
    HANDLE timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

    if (timer == nullptr)
    {
        timer = CreateWaitableTimerW (nullptr, TRUE, nullptr);
        timeBeginPeriod (1);
        usedTimePeriod = true;
    }

    const double period100ns = periodUs * 10.0;
    int64_t base = preciseNow100ns();
    uint64_t index = 0;
    HANDLE handles[2] = { (HANDLE) stopEvent, timer };

    while (running.load (std::memory_order_acquire))
    {
        ++index;
        const int64_t due = base + (int64_t) std::llround ((double) index * period100ns);
        LARGE_INTEGER dueTime;
        dueTime.QuadPart = due;

        if (timer != nullptr && SetWaitableTimer (timer, &dueTime, 0, nullptr, nullptr, FALSE))
        {
            const DWORD waited = WaitForMultipleObjects (2, handles, FALSE, 1000);

            if (waited == WAIT_OBJECT_0)
                break;
        }
        else
        {
            // No timer: fall back to a plain wait that still honours stop().
            const auto remaining = std::max<int64_t> (0, due - preciseNow100ns());

            if (WaitForSingleObject ((HANDLE) stopEvent, (DWORD) (remaining / 10000)) == WAIT_OBJECT_0)
                break;
        }

        if (! running.load (std::memory_order_acquire))
            break;

        const int64_t now = preciseNow100ns();
        const double lateUs = (double) (now - due) / 10.0;

        if (lateUs > 0.0)
        {
            double currentMax = maxLateUs.load (std::memory_order_relaxed);

            while (lateUs > currentMax && ! maxLateUs.compare_exchange_weak (currentMax, lateUs))
            {
            }

            totalLateUs.store (totalLateUs.load (std::memory_order_relaxed) + lateUs, std::memory_order_relaxed);
        }

        ticks.fetch_add (1, std::memory_order_relaxed);

        if (callback)
            callback();

        // After a long stall (debugger, sleep) restart the schedule instead of firing a burst of catch-up ticks.
        if (lateUs > 200000.0)
        {
            base = preciseNow100ns();
            index = 0;
        }
    }

    if (timer != nullptr)
    {
        CancelWaitableTimer (timer);
        CloseHandle (timer);
    }

    if (usedTimePeriod)
        timeEndPeriod (1);

    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics (mmcss);
}
}
