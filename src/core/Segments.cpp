#include "core/Segments.h"

#include <algorithm>
#include <cmath>

namespace ensayo
{
namespace
{
constexpr double int24Scale = 8388608.0;   // 2^23
constexpr int int24Max = 8388607;
constexpr int int24Min = -8388608;

// The 24-bit integer a float sample is stored as: rounded, clamped to [-1, 1), NaN as silence.
int toInt24 (float sample) noexcept
{
    if (std::isnan (sample))
        return 0;

    const auto scaled = std::round ((double) sample * int24Scale);
    return (int) std::clamp (scaled, (double) int24Min, (double) int24Max);
}
}

float quantize24 (float sample) noexcept
{
    return (float) ((double) toInt24 (sample) / int24Scale);
}

// Segments are at most segmentSamples long; anything longer is refused so that encode and decode agree.
juce::MemoryBlock encodeSegment (const juce::AudioBuffer<float>& audio, int numSamples)
{
    const int channels = audio.getNumChannels();

    if (channels < 1 || channels > 2 || numSamples < 0 || numSamples > audio.getNumSamples()
        || numSamples > segmentSamples)
        return {};

    juce::MemoryBlock block;
    {
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::MemoryOutputStream> (block, false);
        auto writer = juce::FlacAudioFormat().createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                                           .withSampleRate (48000)
                                                                           .withNumChannels (channels)
                                                                           .withBitsPerSample (24));
        if (writer == nullptr)
            return {};

        // The FLAC writer takes left-aligned 32-bit integers and shifts them down to 24 bits itself.
        std::vector<int> ints ((size_t) channels * (size_t) numSamples);
        const int* pointers[3] = { nullptr, nullptr, nullptr };

        for (int ch = 0; ch < channels; ++ch)
        {
            auto* dest = ints.data() + (size_t) ch * (size_t) numSamples;
            const auto* src = audio.getReadPointer (ch);

            for (int i = 0; i < numSamples; ++i)
                dest[i] = toInt24 (src[i]) * 256;

            pointers[ch] = dest;
        }

        if (numSamples > 0 && ! writer->write (pointers, numSamples))
            return {};
    }   // destroying the writer finishes the stream and rewrites STREAMINFO

    return block;
}

bool decodeSegment (const void* data, size_t size, juce::AudioBuffer<float>& out)
{
    if (data == nullptr || size == 0)
        return false;

    std::unique_ptr<juce::AudioFormatReader> reader (
        juce::FlacAudioFormat().createReaderFor (new juce::MemoryInputStream (data, size, false), true));

    if (reader == nullptr || reader->sampleRate != 48000.0
        || reader->numChannels < 1 || reader->numChannels > 2
        || reader->lengthInSamples < 0 || reader->lengthInSamples > segmentSamples)
        return false;

    const int channels = (int) reader->numChannels;
    const int length = (int) reader->lengthInSamples;
    out.setSize (channels, length, false, false, false);

    if (length == 0)
        return true;

    // The reader yields left-aligned 32-bit integers; read them as such so the conversion is exact.
    std::vector<int> ints ((size_t) channels * (size_t) length);
    int* pointers[2] = { ints.data(), channels > 1 ? ints.data() + length : nullptr };

    if (! reader->read (pointers, channels, 0, length, false))
        return false;

    for (int ch = 0; ch < channels; ++ch)
    {
        auto* samples = out.getWritePointer (ch);
        const auto* src = pointers[ch];

        for (int i = 0; i < length; ++i)
            samples[i] = (float) ((double) (src[i] >> 8) / int24Scale);
    }

    return true;
}

void appendPeaks (const juce::AudioBuffer<float>& audio, int numSamples, std::vector<Peak>& out)
{
    numSamples = std::clamp (numSamples, 0, audio.getNumSamples());
    const int channels = audio.getNumChannels();

    for (int start = 0; start < numSamples; start += peakBlock)
    {
        const int end = std::min (numSamples, start + peakBlock);
        Peak peak;

        if (channels > 0)
        {
            peak.min = peak.max = audio.getSample (0, start);

            for (int ch = 0; ch < channels; ++ch)
            {
                const auto* src = audio.getReadPointer (ch);

                for (int i = start; i < end; ++i)
                {
                    peak.min = std::min (peak.min, src[i]);
                    peak.max = std::max (peak.max, src[i]);
                }
            }
        }

        out.push_back (peak);
    }
}
}
