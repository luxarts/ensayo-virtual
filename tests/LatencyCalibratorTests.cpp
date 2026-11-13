#include "client/LatencyCalibrator.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>
#include <vector>

namespace ensayo
{
class LatencyCalibratorTests : public juce::UnitTest
{
public:
    LatencyCalibratorTests() : juce::UnitTest ("LatencyCalibrator", "EnsayoVirtual") {}

    // Runs the calibrator against a simulated interface: output delayed by `delay` samples, noise at -60 dB.
    // Returns the number of samples processed.
    static int64_t simulate (LatencyCalibrator& cal, int delay, bool loopback, float noise, int64_t maxSamples)
    {
        juce::Random random (42);
        std::vector<float> history;                 // every output sample so far; the loopback reads `delay` back
        constexpr int block = 128;
        std::vector<float> in (block), l (block), r (block);
        int64_t processed = 0;
        cal.start();

        while (cal.isRunning() && processed < maxSamples)
        {
            for (int i = 0; i < block; ++i)
            {
                in[(size_t) i] = noise * (random.nextFloat() * 2.0f - 1.0f);
                const auto from = processed + i - delay;

                if (loopback && from >= 0 && from < (int64_t) history.size())
                    in[(size_t) i] += history[(size_t) from];
            }

            cal.process (in.data(), l.data(), r.data(), block);
            history.insert (history.end(), l.begin(), l.end());
            processed += block;
        }

        return processed;
    }

    void runTest() override
    {
        beginTest ("measures a 517-sample loopback under -60 dB noise");
        {
            LatencyCalibrator cal;
            const auto processed = simulate (cal, 517, true, 0.001f, 7 * 48000);
            juce::ignoreUnused (processed);
            const auto rt = cal.roundTrip();
            expect (rt.has_value());

            if (rt.has_value())
                expect (std::abs (*rt - 517) <= 1, "round trip " + juce::String (*rt));

            expect (! cal.isRunning());
        }

        beginTest ("without a loopback it reports nothing after the 6 s window");
        {
            LatencyCalibrator cal;
            const auto processed = simulate (cal, 0, false, 0.001f, 8 * 48000);
            expect (processed >= 4 * 48000);
            expect (processed <= 6 * 48000 + 128);
            expect (! cal.isRunning());
            expect (! cal.roundTrip().has_value());
        }

        beginTest ("writes eight single-sample 0.9 clicks on both outputs, the first after 0.25 s");
        {
            LatencyCalibrator cal;
            cal.start();
            expect (cal.isRunning());
            std::vector<float> in (128, 0.0f), l (128), r (128);
            std::vector<int64_t> at;
            int64_t pos = 0;
            bool bothEqual = true;

            while (cal.isRunning() && pos < 7 * 48000)
            {
                cal.process (in.data(), l.data(), r.data(), 128);

                for (int i = 0; i < 128; ++i)
                {
                    bothEqual = bothEqual && l[(size_t) i] == r[(size_t) i];

                    if (l[(size_t) i] != 0.0f)
                    {
                        expectEquals (l[(size_t) i], 0.9f);
                        at.push_back (pos + i);
                    }
                }

                pos += 128;
            }

            expect (bothEqual);
            expectEquals ((int) at.size(), 8);

            if (at.size() == 8)
            {
                expectEquals (at[0], (int64_t) 12000);

                for (size_t k = 1; k < 8; ++k)
                    expectEquals (at[k] - at[k - 1], (int64_t) LatencyCalibrator::periodSamples);
            }

            // Once done it is silent.
            cal.process (in.data(), l.data(), r.data(), 128);
            expect (std::all_of (l.begin(), l.end(), [] (float v) { return v == 0.0f; }));
        }

        beginTest ("a hot input is not mistaken for clicks and calibrationFor subtracts the known latencies");
        {
            LatencyCalibrator cal;
            std::vector<float> in (128, 0.5f), l (128), r (128);
            cal.start();

            while (cal.isRunning())
                cal.process (in.data(), l.data(), r.data(), 128);

            expect (! cal.roundTrip().has_value());
            expectEquals (LatencyCalibrator::calibrationFor (517, 200, 264), 53);
        }
    }
};

static LatencyCalibratorTests latencyCalibratorTests;
}
