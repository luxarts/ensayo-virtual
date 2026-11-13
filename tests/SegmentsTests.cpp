#include "core/Segments.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>

namespace ensayo
{
class SegmentsTests : public juce::UnitTest
{
public:
    SegmentsTests() : juce::UnitTest ("Segments", "EnsayoVirtual") {}

    static float sine (int64_t i, double hz, float amplitude)
    {
        return amplitude * (float) std::sin (juce::MathConstants<double>::twoPi * hz * (double) i / 48000.0);
    }

    static juce::AudioBuffer<float> sineBuffer (int channels, int samples, int64_t start = 0)
    {
        juce::AudioBuffer<float> buffer (channels, samples);

        for (int ch = 0; ch < channels; ++ch)
            for (int i = 0; i < samples; ++i)
                buffer.setSample (ch, i, sine (start + i, ch == 0 ? 440.0 : 1234.5, ch == 0 ? 0.8f : 0.6f));

        return buffer;
    }

    int countMismatches (const juce::AudioBuffer<float>& original, const juce::AudioBuffer<float>& decoded)
    {
        int mismatches = 0;

        for (int ch = 0; ch < original.getNumChannels(); ++ch)
            for (int i = 0; i < original.getNumSamples(); ++i)
                if (decoded.getSample (ch, i) != quantize24 (original.getSample (ch, i)))
                    ++mismatches;

        return mismatches;
    }

    void runTest() override
    {
        beginTest ("mono 48000-sample sine round trips exactly through 24-bit FLAC");
        {
            const auto x = sineBuffer (1, segmentSamples);
            const auto flac = encodeSegment (x, segmentSamples);
            expect (flac.getSize() > 0);

            juce::AudioBuffer<float> out;
            expect (decodeSegment (flac.getData(), flac.getSize(), out));
            expectEquals (out.getNumChannels(), 1);
            expectEquals (out.getNumSamples(), segmentSamples);
            expectEquals (countMismatches (x, out), 0);
            expect (std::abs (quantize24 (0.5f) - 0.5f) < 1.0e-6f);
            expect (quantize24 (2.0f) < 1.0f);
            expectEquals (quantize24 (-2.0f), -1.0f);
        }

        beginTest ("stereo short segment round trips exactly");
        {
            const auto x = sineBuffer (2, 30000);
            const auto flac = encodeSegment (x, 30000);

            juce::AudioBuffer<float> out;
            expect (decodeSegment (flac.getData(), flac.getSize(), out));
            expectEquals (out.getNumChannels(), 2);
            expectEquals (out.getNumSamples(), 30000);
            expectEquals (countMismatches (x, out), 0);
        }

        beginTest ("consecutive segments join without a seam");
        {
            std::vector<float> joined;

            for (int s = 0; s < 3; ++s)
            {
                const auto x = sineBuffer (1, segmentSamples, (int64_t) s * segmentSamples);
                const auto flac = encodeSegment (x, segmentSamples);
                juce::AudioBuffer<float> out;
                expect (decodeSegment (flac.getData(), flac.getSize(), out));
                joined.insert (joined.end(), out.getReadPointer (0), out.getReadPointer (0) + out.getNumSamples());
            }

            expectEquals ((int) joined.size(), 3 * segmentSamples);
            int mismatches = 0;

            for (size_t i = 0; i < joined.size(); ++i)
                if (joined[i] != quantize24 (sine ((int64_t) i, 440.0, 0.8f)))
                    ++mismatches;

            expectEquals (mismatches, 0);

            for (int join = 1; join < 3; ++join)
            {
                const auto at = (size_t) join * segmentSamples;
                expectEquals (joined[at - 1], quantize24 (sine ((int64_t) at - 1, 440.0, 0.8f)));
                expectEquals (joined[at], quantize24 (sine ((int64_t) at, 440.0, 0.8f)));
            }
        }

        beginTest ("garbage bytes and non-48 kHz FLAC are rejected");
        {
            juce::Random random (1234);
            juce::MemoryBlock garbage (4096);

            for (size_t i = 0; i < garbage.getSize(); ++i)
                garbage[i] = (char) random.nextInt (256);

            juce::AudioBuffer<float> out;
            expect (! decodeSegment (garbage.getData(), garbage.getSize(), out));
            expect (! decodeSegment (nullptr, 0, out));

            juce::MemoryBlock flac44;
            {
                std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (flac44, false);
                auto writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                                   .withSampleRate (44100)
                                                                                   .withNumChannels (1)
                                                                                   .withBitsPerSample (24));
                expect (writer != nullptr);
                const auto x = sineBuffer (1, 4410);
                expect (writer->writeFromAudioSampleBuffer (x, 0, x.getNumSamples()));
            }

            expect (flac44.getSize() > 0);
            expect (! decodeSegment (flac44.getData(), flac44.getSize(), out));
        }

        beginTest ("appendPeaks gives one min/max pair per 256-sample block");
        {
            juce::AudioBuffer<float> x (2, 1000);
            juce::Random random (99);

            for (int ch = 0; ch < 2; ++ch)
                for (int i = 0; i < 1000; ++i)
                    x.setSample (ch, i, random.nextFloat() * 2.0f - 1.0f);

            std::vector<Peak> peaks;
            appendPeaks (x, 1000, peaks);
            expectEquals ((int) peaks.size(), 4);

            for (int b = 0; b < 4; ++b)
            {
                const int start = b * peakBlock;
                const int end = std::min (1000, start + peakBlock);
                float lo = 1.0f, hi = -1.0f;

                for (int ch = 0; ch < 2; ++ch)
                    for (int i = start; i < end; ++i)
                    {
                        lo = std::min (lo, x.getSample (ch, i));
                        hi = std::max (hi, x.getSample (ch, i));
                    }

                expectEquals (peaks[(size_t) b].min, lo);
                expectEquals (peaks[(size_t) b].max, hi);
            }
        }
    }
};

static SegmentsTests segmentsTests;
}
