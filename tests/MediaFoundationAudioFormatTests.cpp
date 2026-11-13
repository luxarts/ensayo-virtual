#include "server/MediaFoundationAudioFormat.h"
#include "client/Offline.h"

#include <cmath>

namespace ensayo
{
class MediaFoundationAudioFormatTests : public juce::UnitTest
{
public:
    MediaFoundationAudioFormatTests() : juce::UnitTest ("MediaFoundationAudioFormat", "EnsayoVirtual") {}

    static constexpr double rate = 44100.0;
    static constexpr int numSineSamples = (int) rate * 2;   // 2 s

    // 2 s, 44.1 kHz stereo: 440 Hz sine on the left, 1 kHz sine on the right.
    static juce::File writeSineWav (juce::UnitTest& test)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("mf");
        dir.createDirectory();
        auto file = dir.getChildFile ("sine.wav");
        file.deleteFile();

        juce::AudioBuffer<float> buffer (2, numSineSamples);

        for (int i = 0; i < numSineSamples; ++i)
        {
            buffer.setSample (0, i, (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * i / rate));
            buffer.setSample (1, i, (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * i / rate));
        }

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        test.expect (stream != nullptr, "could not open " + file.getFullPathName());

        if (stream != nullptr)
        {
            // 32-bit float keeps the sine exact so the tolerance below only measures the decoder.
            const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (32);
            auto writer = wav.createWriterFor (stream, options);
            test.expect (writer != nullptr);

            if (writer != nullptr)
                writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        return file;
    }

    // tests/fixtures/youtube-sample.m4a: a DASH m4a as yt-dlp writes it without ffmpeg (not committed; the
    // youtube-track integration script or a manual yt-dlp run leaves one there). Looked up from the working
    // directory and from the test exe's folder (build/EnsayoTests_artefacts/<config>/ → repo root).
    static juce::File fragmentedM4a()
    {
        const auto relative = juce::String ("tests/fixtures/youtube-sample.m4a");
        auto dir = juce::File::getCurrentWorkingDirectory();

        for (int up = 0; up < 4; ++up, dir = dir.getParentDirectory())
            if (dir.getChildFile (relative).existsAsFile())
                return dir.getChildFile (relative);

        dir = juce::File::getSpecialLocation (juce::File::currentExecutableFile).getParentDirectory();

        for (int up = 0; up < 4; ++up, dir = dir.getParentDirectory())
            if (dir.getChildFile (relative).existsAsFile())
                return dir.getChildFile (relative);

        return {};
    }

    static std::unique_ptr<juce::AudioFormatReader> open (MediaFoundationAudioFormat& format, const juce::File& file)
    {
        return std::unique_ptr<juce::AudioFormatReader> (format.createReaderFor (new juce::FileInputStream (file), true));
    }

    void runTest() override
    {
        MediaFoundationAudioFormat format;

        beginTest ("format metadata");
        {
            expectEquals (format.getFormatName(), juce::String ("Media Foundation"));
            expect (format.canHandleFile (juce::File ("C:\\x\\song.m4a")));
            expect (format.canHandleFile (juce::File ("C:\\x\\song.MP4")));
            expect (! format.canHandleFile (juce::File ("C:\\x\\song.txt")));
            expect (format.canDoStereo() && format.canDoMono());
            expect (format.getPossibleSampleRates().isEmpty());
            expect (format.getPossibleBitDepths().isEmpty());
            // Only file streams are supported.
            expect (format.createReaderFor (new juce::MemoryInputStream ("abc", 3, false), true) == nullptr);
        }

        beginTest ("decodes a stereo WAV with two sines");
        {
            const auto file = writeSineWav (*this);
            auto reader = open (format, file);
            expect (reader != nullptr, "no reader for " + file.getFullPathName());

            if (reader != nullptr)
            {
                expectEquals (reader->sampleRate, rate);
                expectEquals ((int) reader->numChannels, 2);
                expect (std::abs ((long long) reader->lengthInSamples - (long long) numSineSamples) <= 2,
                        "lengthInSamples = " + juce::String (reader->lengthInSamples));
                expect (reader->usesFloatingPointData);

                juce::AudioBuffer<float> decoded (2, (int) reader->lengthInSamples);
                expect (reader->read (&decoded, 0, (int) reader->lengthInSamples, 0, true, true));

                float maxErrL = 0.0f, maxErrR = 0.0f;

                for (int n = 0; n < 100; ++n)
                {
                    const auto l = (float) std::sin (juce::MathConstants<double>::twoPi * 440.0 * n / rate);
                    const auto r = (float) std::sin (juce::MathConstants<double>::twoPi * 1000.0 * n / rate);
                    maxErrL = juce::jmax (maxErrL, std::abs (decoded.getSample (0, n) - l));
                    maxErrR = juce::jmax (maxErrR, std::abs (decoded.getSample (1, n) - r));
                }

                expect (maxErrL < 1.0e-3f, "left error " + juce::String (maxErrL));
                expect (maxErrR < 1.0e-3f, "right error " + juce::String (maxErrR));

                // Reading past the end zero-fills rather than failing; AudioFormatReader::read fills channels
                // beyond numChannels with a copy of the last real one.
                juce::AudioBuffer<float> tail (3, 64);
                tail.clear();
                expect (reader->read (&tail, 0, 64, reader->lengthInSamples - 32, true, true));
                expect (std::abs (tail.getSample (0, 0)) > 0.0f);
                expectEquals (tail.getSample (0, 40), 0.0f);
                expectEquals (tail.getSample (1, 40), 0.0f);
                expectEquals (tail.getSample (2, 0), tail.getSample (1, 0));
            }
        }

        beginTest ("decodes a fragmented (DASH) m4a from yt-dlp");
        {
            const auto file = fragmentedM4a();

            if (! file.existsAsFile())
            {
                logMessage ("skipped: fixture not present: " + file.getFullPathName());
            }
            else
            {
                auto reader = open (format, file);
                expect (reader != nullptr, "no reader for " + file.getFullPathName());

                if (reader != nullptr)
                {
                    expect (reader->sampleRate == 44100.0 || reader->sampleRate == 48000.0,
                            "sampleRate = " + juce::String (reader->sampleRate));
                    expect (reader->numChannels == 1 || reader->numChannels == 2,
                            "numChannels = " + juce::String ((int) reader->numChannels));
                    const auto seconds = (double) reader->lengthInSamples / reader->sampleRate;
                    expect (seconds >= 15.0 && seconds <= 25.0, "length = " + juce::String (seconds) + " s");
                    expect (reader->usesFloatingPointData);

                    juce::AudioBuffer<float> decoded ((int) reader->numChannels, (int) reader->lengthInSamples);
                    expect (reader->read (&decoded, 0, (int) reader->lengthInSamples, 0, true, true));
                    const float peak = decoded.getMagnitude (0, decoded.getNumSamples());
                    expect (peak > 0.05f, "peak = " + juce::String (peak));

                    logMessage ("m4a: " + juce::String (reader->sampleRate) + " Hz, "
                                + juce::String ((int) reader->numChannels) + " ch, "
                                + juce::String (reader->lengthInSamples) + " samples ("
                                + juce::String (seconds, 2) + " s), peak " + juce::String (peak, 3));
                }
            }
        }

        beginTest ("loadAndResample (import) accepts the m4a");
        {
            const auto file = fragmentedM4a();

            if (! file.existsAsFile())
            {
                logMessage ("skipped: fixture not present: " + file.getFullPathName());
            }
            else
            {
                juce::AudioBuffer<float> audio;
                const auto result = loadAndResample (file, audio);
                expect (result.wasOk(), result.getErrorMessage());
                const auto length = (juce::int64) audio.getNumSamples();
                expect (length > 15LL * sampleRate && length < 25LL * sampleRate, "length = " + juce::String (length));
            }
        }
    }
};

static MediaFoundationAudioFormatTests mediaFoundationAudioFormatTests;
}
