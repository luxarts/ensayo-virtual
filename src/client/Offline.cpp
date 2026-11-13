#include "client/Offline.h"
#include "server/MediaFoundationAudioFormat.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <new>

namespace ensayo
{
namespace
{
constexpr double targetRate = 48000.0;
constexpr double maxSeconds = 4.0 * 3600.0;
constexpr int exportBlock = 4096;
constexpr float lsb24 = 1.0f / 8388608.0f;

// Everything that is not a plain JUCE format goes first so that m4a/mp4/aac reach Media Foundation.
std::unique_ptr<juce::AudioFormatReader> openReader (const juce::File& file)
{
    juce::AudioFormatManager formats;
    formats.registerFormat (new MediaFoundationAudioFormat(), false);
    formats.registerBasicFormats();
    return std::unique_ptr<juce::AudioFormatReader> (formats.createReaderFor (file));
}

// Offline sinc resampling of the whole buffer. The interpolator delays its output by its algorithmic latency
// (getBaseLatency() input samples), which is skipped, and it is fed trailing zeros so the tail is flushed, not cut.
juce::AudioBuffer<float> resample (const juce::AudioBuffer<float>& in, double sourceRate)
{
    const int channels = in.getNumChannels();
    const int inLength = in.getNumSamples();
    const double ratio = sourceRate / targetRate;
    const int outLength = (int) std::llround ((double) inLength * targetRate / sourceRate);
    const int skip = (int) std::llround ((double) juce::WindowedSincInterpolator::getBaseLatency() / ratio);
    const int padded = inLength + 512 + (int) std::ceil (ratio * 2.0);

    juce::AudioBuffer<float> out (channels, outLength);
    juce::AudioBuffer<float> work (1, padded);
    std::vector<float> produced ((size_t) (outLength + skip));

    for (int c = 0; c < channels; ++c)
    {
        work.clear();
        work.copyFrom (0, 0, in, c, 0, inLength);

        juce::WindowedSincInterpolator interpolator;
        std::fill (produced.begin(), produced.end(), 0.0f);
        interpolator.process (ratio, work.getReadPointer (0), produced.data(), outLength + skip, padded, 0);
        out.copyFrom (c, 0, produced.data() + skip, outLength);
    }

    return out;
}
}

juce::Result loadAndResample (const juce::File& file, juce::AudioBuffer<float>& out)
{
    if (! file.existsAsFile())
        return juce::Result::fail ("File not found: " + file.getFullPathName());

    auto reader = openReader (file);

    if (reader == nullptr)
        return juce::Result::fail ("Unsupported or unreadable audio file: " + file.getFileName());

    if (reader->sampleRate <= 0.0 || reader->numChannels == 0 || reader->lengthInSamples <= 0)
        return juce::Result::fail ("The file contains no audio");

    if ((double) reader->lengthInSamples / reader->sampleRate > maxSeconds)
        return juce::Result::fail ("The file is longer than 4 hours");

    // Leaves room for the interpolator's padding and keeps every sample count inside an int.
    if (reader->lengthInSamples > (int64_t) (std::numeric_limits<int>::max() - 4096))
        return juce::Result::fail ("The file has too many samples to import");

    const int channels = std::min (2, (int) reader->numChannels);
    const int length = (int) reader->lengthInSamples;
    const double sourceRate = reader->sampleRate;

    try
    {
        juce::AudioBuffer<float> decoded (channels, length);

        if (! reader->read (&decoded, 0, length, 0, true, channels > 1))
            return juce::Result::fail ("The file could not be decoded");

        reader.reset();   // the decoder may hold its own copy of the whole file

        if (std::abs (sourceRate - targetRate) < 0.5)
        {
            out = std::move (decoded);
        }
        else
        {
            auto resampled = resample (decoded, sourceRate);
            decoded.setSize (0, 0);
            out = std::move (resampled);
        }
    }
    catch (const std::bad_alloc&)
    {
        return juce::Result::fail ("Not enough memory to import this file");
    }

    return juce::Result::ok();
}

std::vector<juce::MemoryBlock> makeSegments (const juce::AudioBuffer<float>& audio)
{
    std::vector<juce::MemoryBlock> segments;
    const int channels = std::min (2, audio.getNumChannels());

    if (channels == 0)
        return segments;

    for (int start = 0; start < audio.getNumSamples(); start += segmentSamples)
    {
        const int n = std::min (segmentSamples, audio.getNumSamples() - start);
        float* pointers[2] = { const_cast<float*> (audio.getReadPointer (0, start)),
                               channels > 1 ? const_cast<float*> (audio.getReadPointer (1, start)) : nullptr };
        juce::AudioBuffer<float> view (pointers, channels, n);
        segments.push_back (encodeSegment (view, n));
    }

    return segments;
}

juce::Result checkTakesCached (const Project& project, const TakeCache& cache)
{
    for (const auto& track : project.tracks)
    {
        for (const auto& region : track.regions)
        {
            const auto* take = project.findTake (region.take);

            if (take != nullptr && cache.sampleCount (take->id) < take->samples)
                return juce::Result::fail (juce::String (juce::CharPointer_UTF8 (
                    "Esper\xc3\xa1 a que terminen de descargarse las tomas antes de exportar")));
        }
    }

    return juce::Result::ok();
}

juce::Result exportMix (std::shared_ptr<const Project> project, const SampleSource& source, const juce::File& wav,
                        std::function<bool (float)> progress, ExportInfo* info)
{
    if (project == nullptr)
        return juce::Result::fail ("No project");

    // Written next to the target and moved over it only when complete, so a failed or cancelled export
    // never touches an existing file.
    const auto temp = wav.getParentDirectory().getNonexistentChildFile (wav.getFileNameWithoutExtension() + ".partial", ".tmp", false);
    std::unique_ptr<juce::OutputStream> stream (temp.createOutputStream());

    if (stream == nullptr)
        return juce::Result::fail ("Cannot write " + wav.getFullPathName());

    juce::WavAudioFormat format;
    std::unique_ptr<juce::AudioFormatWriter> writer (format.createWriterFor (stream.get(), targetRate, 2, 24, {}, 0));

    if (writer == nullptr)
        return juce::Result::fail ("Cannot create the WAV writer");

    stream.release();   // owned by the writer now

    Renderer renderer;
    renderer.prepare (exportBlock);
    renderer.setProject (project);
    renderer.snapRamps();

    const int64_t total = project->endSample();
    std::vector<float> left (exportBlock), right (exportBlock);
    juce::Random random (1);

    // TPDF: the difference of two uniform variables in [0, 1) is triangular over ±1 LSB. The dithered value is
    // rounded to the 24-bit grid here and handed to the writer as integers (left-aligned in an int32, the writer
    // keeps the top 24 bits), so the stored value is exactly the dithered one.
    const auto quantise = [&random] (float x) -> int
    {
        const float r1 = random.nextFloat(), r2 = random.nextFloat();
        const float y = x + (r1 - r2) * lsb24;
        const int k = (int) std::floor (y / lsb24 + 0.5f);
        return std::clamp (k, -8388608, 8388607) * 256;
    };

    std::vector<int> intLeft (exportBlock), intRight (exportBlock);
    ExportInfo measured;

    bool cancelled = false;

    for (int64_t pos = 0; pos < total; pos += exportBlock)
    {
        if (progress && ! progress ((float) pos / (float) total))
        {
            cancelled = true;
            break;
        }

        const int n = (int) std::min<int64_t> (exportBlock, total - pos);
        renderer.render (source, pos, n, left.data(), right.data());

        for (int i = 0; i < n; ++i)
        {
            for (const float x : { left[(size_t) i], right[(size_t) i] })
            {
                const float magnitude = std::abs (x);
                measured.peak = std::max (measured.peak, magnitude);

                if (magnitude > 1.0f)
                    ++measured.clippedSamples;
            }

            intLeft[(size_t) i] = quantise (left[(size_t) i]);
            intRight[(size_t) i] = quantise (right[(size_t) i]);
        }

        const int* channels[2] = { intLeft.data(), intRight.data() };

        if (! writer->write (channels, n))
        {
            writer.reset();
            temp.deleteFile();
            return juce::Result::fail ("Writing " + wav.getFileName() + " failed (disk full?)");
        }
    }

    const bool finalised = cancelled || writer->flush();   // writes the header
    writer.reset();

    if (cancelled)
    {
        temp.deleteFile();
        return juce::Result::fail ("Export cancelled");
    }

    if (! finalised || ! temp.replaceFileIn (wav))
    {
        temp.deleteFile();
        return juce::Result::fail ("Could not finish writing " + wav.getFileName());
    }

    if (progress)
        progress (1.0f);   // the file is complete, so a late cancel has nothing left to stop

    if (info != nullptr)
        *info = measured;

    return juce::Result::ok();
}
}
