#include "client/LatencyCalibrator.h"

#include <algorithm>
#include <cmath>

namespace ensayo
{
void LatencyCalibrator::start() noexcept
{
    running.store (false, std::memory_order_release);
    detected.store (0);
    position = 0;
    lastClickAt = -1;
    emitted = 0;
    quietRun = 0;
    lastClickDetected = true;
    running.store (true, std::memory_order_release);
}

bool LatencyCalibrator::isRunning() const noexcept { return running.load (std::memory_order_acquire); }

void LatencyCalibrator::process (const float* in, float* outL, float* outR, int n) noexcept
{
    if (! running.load (std::memory_order_acquire))
    {
        std::fill (outL, outL + n, 0.0f);
        std::fill (outR, outR + n, 0.0f);
        return;
    }

    for (int i = 0; i < n; ++i)
    {
        float out = 0.0f;
        const bool clickDue = emitted < clicks && position == (int64_t) firstClickSamples + (int64_t) emitted * periodSamples;

        if (clickDue)
        {
            out = clickLevel;
            lastClickAt = position;
            lastClickDetected = false;
            ++emitted;
        }

        outL[i] = out;
        outR[i] = out;

        // A click is detected at the first loud input sample that follows 20 ms of quiet.
        if (std::abs (in[i]) > threshold)
        {
            if (quietRun >= quietSamples && ! lastClickDetected && lastClickAt >= 0)
            {
                const int d = detected.load (std::memory_order_relaxed);

                if (d < clicks)
                {
                    trips[(size_t) d].store ((int) (position - lastClickAt), std::memory_order_relaxed);
                    detected.store (d + 1, std::memory_order_release);
                }

                lastClickDetected = true;
            }

            quietRun = 0;
        }
        else
        {
            quietRun = std::min (quietRun + 1, quietSamples);
        }

        ++position;
    }

    const bool trainOver = emitted >= clicks && position >= lastClickAt + periodSamples;

    if (trainOver || position >= maxSamples)
        running.store (false, std::memory_order_release);
}

std::optional<int> LatencyCalibrator::roundTrip() const
{
    if (isRunning())
        return std::nullopt;

    const int count = detected.load (std::memory_order_acquire);

    if (count < 5)
        return std::nullopt;

    std::vector<int> values;

    for (int i = 0; i < count; ++i)
        values.push_back (trips[(size_t) i].load (std::memory_order_relaxed));

    std::sort (values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 == 1 ? values[mid] : (values[mid - 1] + values[mid]) / 2;
}
}
