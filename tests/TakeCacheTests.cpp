#include "client/TakeCache.h"

#include <juce_core/juce_core.h>
#include <atomic>
#include <random>
#include <thread>
#include <vector>

namespace ensayo
{
class TakeCacheTests : public juce::UnitTest
{
public:
    TakeCacheTests() : juce::UnitTest ("TakeCache", "EnsayoVirtual") {}

    static juce::File testDir()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
            .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("takecache");
    }

    static juce::File freshDir()
    {
        auto dir = testDir();
        dir.deleteRecursively();
        return dir;
    }

    // A ramp from -0.8 to 0.8 repeating every 100000 samples; channel 1 is the negated ramp.
    static float ramp (int64_t i, int channel = 0)
    {
        const auto v = -0.8f + 1.6f * (float) (i % 100000) / 100000.0f;
        return channel == 0 ? v : -v;
    }

    static juce::MemoryBlock rampSegment (int64_t start, int n, int channels = 1)
    {
        juce::AudioBuffer<float> buffer (channels, n);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < n; ++i)
                buffer.setSample (ch, i, ramp (start + i, ch));

        return encodeSegment (buffer, n);
    }

    bool appendRamp (TakeCache& cache, const Id& take, int index, int n = segmentSamples, int channels = 1)
    {
        const auto flac = rampSegment ((int64_t) index * segmentSamples, n, channels);
        return cache.appendSegment (take, index, flac.getData(), flac.getSize());
    }

    void runTest() override
    {
        beginTest ("three segments read across chunk boundaries, zeros past the end");
        {
            TakeCache cache (freshDir());
            const auto take = newId();
            int changes = 0;
            cache.onTakeChanged = [&] (const Id& t) { if (t == take) ++changes; };

            expectEquals (cache.channels (take), 0);
            expect (appendRamp (cache, take, 0));
            expect (appendRamp (cache, take, 1));
            expect (appendRamp (cache, take, 2));
            expectEquals (changes, 3);
            expectEquals (cache.segmentCount (take), 3);
            expectEquals (cache.sampleCount (take), (int64_t) 144000);
            expectEquals (cache.channels (take), 1);
            expect (cache.takes() == std::vector<Id> { take });
            expectEquals ((int) cache.peaks (take).size(), (144000 + peakBlock - 1) / peakBlock);

            // Peak windows follow the whole take, not each segment (48000 is not a multiple of peakBlock).
            {
                juce::AudioBuffer<float> whole (1, 144000);
                for (int i = 0; i < 144000; ++i)
                    whole.setSample (0, i, quantize24 (ramp (i)));

                std::vector<Peak> expectedPeaks;
                appendPeaks (whole, 144000, expectedPeaks);
                const auto got = cache.peaks (take);
                bool same = got.size() == expectedPeaks.size();

                for (size_t i = 0; same && i < got.size(); ++i)
                    same = got[i].min == expectedPeaks[i].min && got[i].max == expectedPeaks[i].max;

                expect (same, "peaks do not match the continuous take");
            }

            float dest[20];
            cache.read (take, 0, 47990, 20, dest);

            for (int i = 0; i < 20; ++i)
                expectEquals (dest[i], quantize24 (ramp (47990 + i)));

            float tail[20];
            cache.read (take, 0, 143990, 20, tail);

            for (int i = 0; i < 10; ++i)
                expectEquals (tail[i], quantize24 (ramp (143990 + i)));
            for (int i = 10; i < 20; ++i)
                expectEquals (tail[i], 0.0f);

            float before[10];
            cache.read (take, 0, -5, 10, before);

            for (int i = 0; i < 5; ++i)
                expectEquals (before[i], 0.0f);
            for (int i = 5; i < 10; ++i)
                expectEquals (before[i], quantize24 (ramp (i - 5)));

            float unknown[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
            cache.read (newId(), 0, 0, 4, unknown);

            for (auto v : unknown)
                expectEquals (v, 0.0f);

            const auto bytes = cache.segmentBytes (take, 1);
            expect (bytes == rampSegment (segmentSamples, segmentSamples));
            expect (cache.segmentBytes (take, 3).isEmpty());
            expect (testDir().getChildFile (take).getChildFile ("2.flac").existsAsFile());
        }

        beginTest ("out-of-order append is ignored");
        {
            TakeCache cache (freshDir());
            const auto take = newId();
            expect (appendRamp (cache, take, 0));
            expect (! appendRamp (cache, take, 2));
            expect (! appendRamp (cache, take, 0));
            expectEquals (cache.segmentCount (take), 1);
            expectEquals (cache.sampleCount (take), (int64_t) segmentSamples);
            expect (! testDir().getChildFile (take).getChildFile ("2.flac").exists());

            const auto other = newId();
            expect (! appendRamp (cache, other, 1));
            expectEquals (cache.segmentCount (other), 0);
            expect (cache.takes().size() == 1);

            juce::MemoryBlock garbage (100, true);
            expect (! cache.appendSegment (take, 1, garbage.getData(), garbage.getSize()));
            expect (! appendRamp (cache, take, 1, segmentSamples, 2));   // channel count must not change
            expectEquals (cache.segmentCount (take), 1);
        }

        beginTest ("nothing can follow a short segment");
        {
            TakeCache cache (freshDir());
            const auto take = newId();
            expect (appendRamp (cache, take, 0));
            expect (appendRamp (cache, take, 1, 1000));
            expect (! appendRamp (cache, take, 2));
            expectEquals (cache.segmentCount (take), 2);
            expectEquals (cache.sampleCount (take), (int64_t) segmentSamples + 1000);
        }

        beginTest ("loadFromDisk restores takes");
        {
            const auto take = newId();
            std::vector<Peak> peaks;
            {
                TakeCache cache (freshDir());
                expect (appendRamp (cache, take, 0, segmentSamples, 2));
                expect (appendRamp (cache, take, 1, segmentSamples, 2));
                expect (appendRamp (cache, take, 2, 12345, 2));
                peaks = cache.peaks (take);
            }

            // A segment after a gap is not loaded.
            const auto stray = rampSegment (0, segmentSamples);
            expect (testDir().getChildFile (take).getChildFile ("5.flac").replaceWithData (stray.getData(), stray.getSize()));

            TakeCache restored (testDir());
            restored.loadFromDisk();
            expect (restored.takes() == std::vector<Id> { take });
            expectEquals (restored.segmentCount (take), 3);
            expectEquals (restored.sampleCount (take), (int64_t) 2 * segmentSamples + 12345);
            expectEquals (restored.channels (take), 2);

            const auto restoredPeaks = restored.peaks (take);
            expectEquals (restoredPeaks.size(), peaks.size());
            bool same = restoredPeaks.size() == peaks.size();

            for (size_t i = 0; same && i < peaks.size(); ++i)
                same = restoredPeaks[i].min == peaks[i].min && restoredPeaks[i].max == peaks[i].max;

            expect (same, "peaks differ after loadFromDisk");

            float dest[4];
            restored.read (take, 1, 50000, 4, dest);

            for (int i = 0; i < 4; ++i)
                expectEquals (dest[i], quantize24 (ramp (50000 + i, 1)));
        }

        beginTest ("concurrent append and read");
        {
            TakeCache cache (freshDir());
            const auto take = newId();
            constexpr int segments = 60;

            std::vector<juce::MemoryBlock> flac;
            for (int s = 0; s < segments; ++s)
                flac.push_back (rampSegment ((int64_t) s * segmentSamples, segmentSamples));

            std::vector<float> expected ((size_t) segments * segmentSamples);
            for (size_t i = 0; i < expected.size(); ++i)
                expected[i] = quantize24 (ramp ((int64_t) i));

            std::atomic<int> appendFailures { 0 };
            std::atomic<bool> appending { true };

            std::thread writer ([&]
            {
                for (int s = 0; s < segments; ++s)
                    if (! cache.appendSegment (take, s, flac[(size_t) s].getData(), flac[(size_t) s].getSize()))
                        ++appendFailures;

                appending = false;
            });

            int badValues = 0;
            int nonEmptyReads = 0;
            std::mt19937 rng (42);
            float dest[512];

            for (int r = 0; r < 100000; ++r)
            {
                const auto observed = cache.sampleCount (take);
                const auto offset = (int64_t) (rng() % (uint32_t) (observed + 1));
                const auto n = 1 + (int) (rng() % 512);
                cache.read (take, 0, offset, n, dest);

                if (observed > 0 && observed < (int64_t) expected.size())
                    ++nonEmptyReads;

                for (int i = 0; i < n; ++i)
                {
                    const auto pos = offset + i;
                    const bool inRange = pos < (int64_t) expected.size();

                    if (pos < observed)
                    {
                        if (dest[i] != expected[(size_t) pos])
                            ++badValues;
                    }
                    else if (dest[i] != 0.0f && ! (inRange && dest[i] == expected[(size_t) pos]))
                    {
                        ++badValues;
                    }
                }
            }

            writer.join();
            expectEquals (appendFailures.load(), 0);
            expectEquals (badValues, 0);
            expect (! appending.load());
            expectEquals (cache.sampleCount (take), (int64_t) segments * segmentSamples);
            logMessage ("reads during the append: " + juce::String (nonEmptyReads));
        }

        beginTest ("channel index clamps to the last channel");
        {
            TakeCache cache (freshDir());
            const auto take = newId();
            expect (appendRamp (cache, take, 0, segmentSamples, 2));
            expectEquals (cache.channels (take), 2);

            float dest[8], right[8];
            cache.read (take, 5, 100, 8, dest);
            cache.read (take, 1, 100, 8, right);

            for (int i = 0; i < 8; ++i)
            {
                expectEquals (dest[i], right[i]);
                expectEquals (dest[i], quantize24 (ramp (100 + i, 1)));
            }

            cache.read (take, -3, 100, 8, dest);
            expectEquals (dest[0], quantize24 (ramp (100, 0)));
        }

        testDir().deleteRecursively();
    }
};

static TakeCacheTests takeCacheTests;
}
