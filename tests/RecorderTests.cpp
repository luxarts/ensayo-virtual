#include "client/Recorder.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>
#include <thread>

namespace ensayo
{
class RecorderTests : public juce::UnitTest
{
public:
    RecorderTests() : juce::UnitTest ("Recorder", "EnsayoVirtual") {}

    static juce::File testDir()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual/tests/recorder");
    }

    // Everything the callbacks report, gathered on the writer thread.
    struct Collected
    {
        std::mutex lock;
        std::vector<juce::MemoryBlock> segments;
        std::vector<int> segmentSamples;
        std::vector<Peak> peaks;
        int64_t liveSamples = 0;
        int finishedSegments = -1;
        int64_t finishedSamples = -1;
        int finishedCalls = 0;
    };

    static void attach (Recorder& r, Collected& c)
    {
        r.onSegment = [&c] (int index, const juce::MemoryBlock& flac, int samples)
        {
            std::lock_guard<std::mutex> g (c.lock);
            if ((int) c.segments.size() != index) return;
            c.segments.push_back (flac);
            c.segmentSamples.push_back (samples);
        };
        r.onPeaks = [&c] (int64_t from, const std::vector<Peak>& peaks)
        {
            std::lock_guard<std::mutex> g (c.lock);
            c.peaks.resize (std::max<size_t> (c.peaks.size(), (size_t) from + peaks.size()));
            std::copy (peaks.begin(), peaks.end(), c.peaks.begin() + (std::ptrdiff_t) from);
        };
        r.onLiveAudio = [&c] (const float* const*, int, int n)
        {
            std::lock_guard<std::mutex> g (c.lock);
            c.liveSamples += n;
        };
        r.onFinished = [&c] (int segments, int64_t samples)
        {
            std::lock_guard<std::mutex> g (c.lock);
            c.finishedSegments = segments;
            c.finishedSamples = samples;
            ++c.finishedCalls;
        };
    }

    static float ramp (int64_t i, int64_t total) { return (float) ((double) i / (double) total * 1.8 - 0.9); }

    // Pushes `total` samples of ramp (right channel inverted) in blocks of 256, from its own thread.
    static void pushRamp (Recorder& r, int channels, int64_t total, bool throttle)
    {
        std::thread t ([&]
        {
            std::vector<float> l (256), rr (256);

            for (int64_t i = 0; i < total; i += 256)
            {
                const int n = (int) std::min<int64_t> (256, total - i);

                for (int k = 0; k < n; ++k)
                {
                    l[(size_t) k] = ramp (i + k, total);
                    rr[(size_t) k] = -l[(size_t) k];
                }

                const float* ch[2] = { l.data(), rr.data() };
                r.push (ch, n);

                if (throttle && (i / 256) % 64 == 63)
                    std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });
        t.join();
        juce::ignoreUnused (channels);
    }

    void runTest() override
    {
        testDir().deleteRecursively();
        testDir().createDirectory();

        beginTest ("mono take: 3.5 s becomes four exact segments, a full WAV and peaks");
        {
            constexpr int64_t total = 168000;
            Recorder rec;
            Collected c;
            attach (rec, c);
            const auto wavFile = testDir().getChildFile ("mono.wav");
            expect (rec.start (newId(), 1, wavFile).wasOk());
            expect (rec.isRecording());
            pushRamp (rec, 1, total, true);
            rec.stop();
            expect (! rec.isRecording());

            expectEquals (rec.samplesCaptured(), total);
            expectEquals (rec.overflows(), (uint32_t) 0);
            expectEquals ((int) c.segments.size(), 4);
            expectEquals (c.finishedSegments, 4);
            expectEquals (c.finishedSamples, total);
            expectEquals (c.finishedCalls, 1);
            expectEquals (c.liveSamples, total);

            if (c.segments.size() == 4)
            {
                expectEquals (c.segmentSamples[0], 48000);
                expectEquals (c.segmentSamples[1], 48000);
                expectEquals (c.segmentSamples[2], 48000);
                expectEquals (c.segmentSamples[3], 24000);

                int mismatches = 0;
                int64_t at = 0;

                for (auto& flac : c.segments)
                {
                    juce::AudioBuffer<float> decoded;
                    expect (decodeSegment (flac.getData(), flac.getSize(), decoded));
                    expectEquals (decoded.getNumChannels(), 1);

                    for (int i = 0; i < decoded.getNumSamples(); ++i, ++at)
                        if (decoded.getSample (0, i) != quantize24 (ramp (at, total)))
                            ++mismatches;
                }

                expectEquals (at, total);
                expectEquals (mismatches, 0);
            }

            juce::WavAudioFormat format;
            std::unique_ptr<juce::AudioFormatReader> reader (format.createReaderFor (new juce::FileInputStream (wavFile), true));
            expect (reader != nullptr);

            if (reader != nullptr)
            {
                expectEquals ((int64_t) reader->lengthInSamples, total);
                expect (reader->usesFloatingPointData);
                juce::AudioBuffer<float> wav (1, (int) total);
                reader->read (&wav, 0, (int) total, 0, true, true);
                int mismatches = 0;

                for (int i = 0; i < (int) total; ++i)
                    if (wav.getSample (0, i) != ramp (i, total))
                        ++mismatches;

                expectEquals (mismatches, 0);
            }

            beginTest ("peaks cover the whole take, continuous across segment boundaries");
            juce::AudioBuffer<float> all (1, (int) total);

            for (int i = 0; i < (int) total; ++i)
                all.setSample (0, i, ramp (i, total));

            std::vector<Peak> expected;
            appendPeaks (all, (int) total, expected);
            expectEquals ((int) c.peaks.size(), (int) std::ceil ((double) total / 256.0));
            expectEquals ((int) c.peaks.size(), (int) expected.size());

            int bad = 0;

            for (size_t i = 0; i < std::min (c.peaks.size(), expected.size()); ++i)
                if (c.peaks[i].min != expected[i].min || c.peaks[i].max != expected[i].max)
                    ++bad;

            expectEquals (bad, 0);
        }

        beginTest ("stereo take keeps both channels");
        {
            constexpr int64_t total = 60000;
            Recorder rec;
            Collected c;
            attach (rec, c);
            expect (rec.start (newId(), 2, testDir().getChildFile ("stereo.wav")).wasOk());
            pushRamp (rec, 2, total, true);
            rec.stop();

            expectEquals ((int) c.segments.size(), 2);
            expectEquals (c.finishedSamples, total);
            int mismatches = 0;
            int64_t at = 0;

            for (auto& flac : c.segments)
            {
                juce::AudioBuffer<float> decoded;
                expect (decodeSegment (flac.getData(), flac.getSize(), decoded));
                expectEquals (decoded.getNumChannels(), 2);

                for (int i = 0; i < decoded.getNumSamples(); ++i, ++at)
                    if (decoded.getSample (0, i) != quantize24 (ramp (at, total))
                        || decoded.getSample (1, i) != quantize24 (-ramp (at, total)))
                        ++mismatches;
            }

            expectEquals (mismatches, 0);
        }

        beginTest ("overflow drops whole blocks, counts them and never crashes");
        {
            Recorder rec;
            Collected c;
            attach (rec, c);
            expect (rec.start (newId(), 1, testDir().getChildFile ("overflow.wav")).wasOk());
            rec.setWriterPausedForTest (true);
            constexpr int64_t total = 480000;      // 10 s into an 8 s FIFO
            pushRamp (rec, 1, total, false);

            expect (rec.overflows() > 0);
            expect (rec.samplesCaptured() > 0);
            expect (rec.samplesCaptured() < total);
            expect (rec.samplesCaptured() <= 8 * 48000);

            rec.setWriterPausedForTest (false);
            rec.stop();
            expectEquals (c.finishedSamples, rec.samplesCaptured());
            expectEquals (c.liveSamples, rec.samplesCaptured());
        }

        beginTest ("recordingOffset adds the latencies and clamps at zero");
        {
            expectEquals (recordingOffset (96000, 200, 264, 12), (int64_t) 96476);
            expectEquals (recordingOffset (0, 200, 264, -1000), (int64_t) 0);
            expectEquals (recordingOffset (1000, 10, 10, -500), (int64_t) 520);
        }

        beginTest ("start refuses bad arguments and unwritable paths, and can be repeated");
        {
            Recorder rec;
            expect (rec.start (newId(), 3, testDir().getChildFile ("x.wav")).failed());
            expect (! rec.isRecording());

            const auto blocker = testDir().getChildFile ("blocker");
            blocker.replaceWithText ("a file, not a directory");
            expect (rec.start (newId(), 1, blocker.getChildFile ("inner.wav")).failed());
            expect (! rec.isRecording());

            Collected c;
            attach (rec, c);
            expect (rec.start (newId(), 1, testDir().getChildFile ("again.wav")).wasOk());
            expect (rec.start (newId(), 1, testDir().getChildFile ("again2.wav")).failed());
            rec.stop();
            expectEquals (c.finishedSegments, 0);
            expectEquals (c.finishedSamples, (int64_t) 0);

            c.finishedCalls = 0;
            expect (rec.start (newId(), 1, testDir().getChildFile ("again.wav")).wasOk());
            pushRamp (rec, 1, 1000, false);
            rec.stop();
            expectEquals (c.finishedCalls, 1);
            expectEquals (c.finishedSamples, (int64_t) 1000);
        }

        testDir().deleteRecursively();
    }
};

static RecorderTests recorderTests;
}
