#include "core/MixMath.h"
#include "core/Protocol.h"

#include <array>
#include <cmath>

namespace ensayo
{
class MixMathTests : public juce::UnitTest
{
public:
    MixMathTests() : juce::UnitTest ("MixMath", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("dB to gain");
        {
            expectWithinAbsoluteError (dbToGain (0.0f), 1.0f, 1.0e-6f);
            expectEquals (dbToGain (-60.0f), 0.0f);
            expectEquals (dbToGain (-100.0f), 0.0f);
            expectWithinAbsoluteError (dbToGain (-6.0206f), 0.5f, 1.0e-4f);
            expectWithinAbsoluteError (dbToGain (12.0f), 3.98107f, 1.0e-4f);
            expectWithinAbsoluteError (dbToGain (20.0f), dbToGain (maxFaderDb), 1.0e-6f);
        }

        beginTest ("channel gain");
        {
            ChannelGain gain;
            expectWithinAbsoluteError (gain.voiceGain(), 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (gain.instrumentGain(), 1.0f, 1.0e-6f);

            gain.voiceDb = -6.0206f;
            gain.instrumentDb = -60.0f;
            expectWithinAbsoluteError (gain.voiceGain(), 0.5f, 1.0e-4f);
            expectEquals (gain.instrumentGain(), 0.0f);

            gain.muted = true;
            gain.instrumentDb = 0.0f;
            expectEquals (gain.voiceGain(), 0.0f);
            expectEquals (gain.instrumentGain(), 0.0f);
        }

        beginTest ("addMonoCentred accumulates");
        {
            std::array<float, 4> L { 1.0f, 1.0f, 1.0f, 1.0f };
            std::array<float, 4> R { -1.0f, -1.0f, -1.0f, -1.0f };
            const std::array<float, 4> mono { 0.5f, 0.25f, 0.0f, -0.5f };

            addMonoCentred (L.data(), R.data(), mono.data(), 2.0f, 4);

            expectWithinAbsoluteError (L[0], 2.0f, 1.0e-6f);
            expectWithinAbsoluteError (L[1], 1.5f, 1.0e-6f);
            expectWithinAbsoluteError (L[2], 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (L[3], 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (R[0], 0.0f, 1.0e-6f);
            expectWithinAbsoluteError (R[3], -2.0f, 1.0e-6f);

            addMonoCentred (L.data(), R.data(), mono.data(), 0.0f, 4);
            expectWithinAbsoluteError (L[0], 2.0f, 1.0e-6f);
        }

        beginTest ("addStereo accumulates");
        {
            std::array<float, 3> L { 0.0f, 0.0f, 0.0f };
            std::array<float, 3> R { 1.0f, 1.0f, 1.0f };
            const std::array<float, 3> inL { 1.0f, 2.0f, 3.0f };
            const std::array<float, 3> inR { -1.0f, -2.0f, -3.0f };

            addStereo (L.data(), R.data(), inL.data(), inR.data(), 0.5f, 3);

            expectWithinAbsoluteError (L[2], 1.5f, 1.0e-6f);
            expectWithinAbsoluteError (R[2], -0.5f, 1.0e-6f);
        }

        beginTest ("peakOf");
        {
            const std::array<float, 5> data { 0.1f, -0.9f, 0.3f, 0.5f, -0.2f };
            expectWithinAbsoluteError (peakOf (data.data(), 5), 0.9f, 1.0e-6f);
            expectEquals (peakOf (data.data(), 0), 0.0f);
            expectEquals (peakOf (nullptr, 5), 0.0f);
        }

        beginTest ("interleave and deinterleave");
        {
            const std::array<float, 3> L { 1.0f, 2.0f, 3.0f };
            const std::array<float, 3> R { 4.0f, 5.0f, 6.0f };
            std::array<float, 6> out {};

            interleave (L.data(), R.data(), out.data(), 3);
            const std::array<float, 6> expected { 1.0f, 4.0f, 2.0f, 5.0f, 3.0f, 6.0f };

            for (size_t i = 0; i < out.size(); ++i)
                expectEquals (out[i], expected[i]);

            std::array<float, 3> backL {}, backR {};
            deinterleave (out.data(), backL.data(), backR.data(), 3);

            for (size_t i = 0; i < 3; ++i)
            {
                expectEquals (backL[i], L[i]);
                expectEquals (backR[i], R[i]);
            }
        }
    }
};

static MixMathTests mixMathTests;
}
