#include "client/Offline.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <algorithm>
#include <cmath>

namespace ensayo
{
class OfflineTests : public juce::UnitTest
{
public:
    OfflineTests() : juce::UnitTest ("Offline", "EnsayoVirtual") {}

    static juce::File testDir()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual/tests/offline");
    }

    static bool writeWav (const juce::File& f, const juce::AudioBuffer<float>& b, double rate, int bits)
    {
        f.deleteFile();
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> os (f.createOutputStream());

        if (os == nullptr)
            return false;

        std::unique_ptr<juce::AudioFormatWriter> w (wav.createWriterFor (os.get(), rate, (unsigned) b.getNumChannels(), bits, {}, 0));

        if (w == nullptr)
            return false;

        os.release();
        return w->writeFromAudioSampleBuffer (b, 0, b.getNumSamples());
    }

    class SineSource : public SampleSource
    {
    public:
        int channels (const Id& take) const noexcept override { return take == "t" ? 2 : 0; }

        void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept override
        {
            for (int i = 0; i < n; ++i)
            {
                const auto k = offset + i;
                dest[i] = (take == "t" && k >= 0 && k < 200000)
                              ? (float) (0.6 * std::sin (2.0 * juce::MathConstants<double>::pi * (channel == 0 ? 440.0 : 660.0) * (double) k / 48000.0))
                              : 0.0f;
            }
        }
    };

    static std::shared_ptr<const Project> makeTestProject (int64_t length)
    {
        auto p = std::make_shared<Project>();
        Track t;
        t.id = newId();
        t.gainDb = -3.0f;
        t.pan = 0.3f;
        Region r;
        r.id = newId();
        r.take = "t";
        r.start = 1000;
        r.offset = 500;
        r.length = length;
        t.regions.push_back (r);
        p->tracks.push_back (t);
        return p;
    }

    void runTest() override
    {
        testDir().deleteRecursively();
        testDir().createDirectory();

        beginTest ("44.1 kHz stereo is resampled to 48 kHz keeping frequency and level");
        {
            juce::AudioBuffer<float> src (2, 88200);

            for (int c = 0; c < 2; ++c)
                for (int i = 0; i < 88200; ++i)
                    src.setSample (c, i, (float) (0.5 * std::sin (2.0 * juce::MathConstants<double>::pi * 1000.0 * i / 44100.0)));

            const auto f = testDir().getChildFile ("s44.wav");
            expect (writeWav (f, src, 44100.0, 24));
            juce::AudioBuffer<float> out;
            const auto r = loadAndResample (f, out);
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals (out.getNumSamples(), 96000);
            expectEquals (out.getNumChannels(), 2);

            // Skip the interpolator edges for the measurements.
            const int from = 2000, to = 94000;
            int crossings = 0, first = -1, last = -1;
            double sumOut = 0.0, sumSrc = 0.0;

            for (int i = from; i < to; ++i)
            {
                const float a = out.getSample (0, i - 1), b = out.getSample (0, i);

                if (a < 0.0f && b >= 0.0f)
                {
                    ++crossings;
                    if (first < 0) first = i;
                    last = i;
                }

                sumOut += (double) b * b;
            }

            for (int i = 0; i < 88200; ++i)
                sumSrc += (double) src.getSample (0, i) * src.getSample (0, i);

            const double freq = (double) (crossings - 1) * 48000.0 / (double) (last - first);
            const double rmsOut = std::sqrt (sumOut / (to - from)), rmsSrc = std::sqrt (sumSrc / 88200);
            const double db = 20.0 * std::log10 (rmsOut / rmsSrc);
            logMessage ("resampled frequency " + juce::String (freq, 4) + " Hz, RMS difference " + juce::String (db, 4) + " dB");
            expect (std::abs (freq - 1000.0) <= 1.0);
            expect (std::abs (db) <= 0.1);
            // The tail must not be truncated: the last 200 samples still carry the sine.
            float tail = 0.0f;
            for (int i = 95800; i < 96000; ++i) tail = std::max (tail, std::abs (out.getSample (0, i)));
            expect (tail > 0.4f);
        }

        beginTest ("48 kHz mono is not resampled");
        {
            juce::AudioBuffer<float> src (1, 5000);
            for (int i = 0; i < 5000; ++i) src.setSample (0, i, (float) (i % 100) / 128.0f - 0.3f);
            const auto f = testDir().getChildFile ("m48.wav");
            expect (writeWav (f, src, 48000.0, 24));
            juce::AudioBuffer<float> out;
            expect (loadAndResample (f, out).wasOk());
            expectEquals (out.getNumChannels(), 1);
            expectEquals (out.getNumSamples(), 5000);
            float maxDiff = 0.0f;
            for (int i = 0; i < 5000; ++i) maxDiff = std::max (maxDiff, std::abs (out.getSample (0, i) - src.getSample (0, i)));
            expect (maxDiff <= 1.0f / 8388608.0f);
            expect (! loadAndResample (testDir().getChildFile ("missing.wav"), out).wasOk());
        }

        beginTest ("makeSegments cuts 130000 samples into 48000, 48000, 34000");
        {
            juce::AudioBuffer<float> a (2, 130000);
            a.clear();
            const auto segs = makeSegments (a);
            expectEquals ((int) segs.size(), 3);
            const int expected[] = { 48000, 48000, 34000 };

            for (size_t i = 0; i < segs.size() && i < 3; ++i)
            {
                juce::AudioBuffer<float> d;
                expect (decodeSegment (segs[i].getData(), segs[i].getSize(), d));
                expectEquals (d.getNumSamples(), expected[i]);
                expectEquals (d.getNumChannels(), 2);
            }
        }

        beginTest ("exportMix matches the renderer within 2 LSB and writes 24-bit 48 kHz stereo");
        {
            const int64_t length = 20000;
            const auto project = makeTestProject (length);
            SineSource source;
            const auto f = testDir().getChildFile ("mix.wav");
            float lastProgress = -1.0f;
            const auto r = exportMix (project, source, f, [&] (float p) { lastProgress = p; return true; });
            expect (r.wasOk(), r.getErrorMessage());
            expectGreaterThan (lastProgress, 0.9f);

            juce::WavAudioFormat wav;
            std::unique_ptr<juce::AudioFormatReader> rd (wav.createReaderFor (f.createInputStream().release(), true));
            expect (rd != nullptr);

            if (rd != nullptr)
            {
                expectEquals ((int) rd->bitsPerSample, 24);
                expectEquals (rd->sampleRate, 48000.0);
                expectEquals ((int) rd->numChannels, 2);
                expectEquals ((int64_t) rd->lengthInSamples, project->endSample());

                const int n = (int) project->endSample();
                juce::AudioBuffer<float> got (2, n);
                rd->read (&got, 0, n, 0, true, true);

                Renderer renderer;
                renderer.prepare (4096);
                renderer.setProject (project);
                renderer.snapRamps();
                std::vector<float> L ((size_t) n), R ((size_t) n);

                for (int pos = 0; pos < n; pos += 4096)
                {
                    const int b = std::min (4096, n - pos);
                    renderer.render (source, pos, b, L.data() + pos, R.data() + pos);
                }

                float maxErr = 0.0f;
                for (int i = 0; i < n; ++i)
                    maxErr = std::max ({ maxErr, std::abs (got.getSample (0, i) - L[(size_t) i]), std::abs (got.getSample (1, i) - R[(size_t) i]) });

                const float lsbs = maxErr * 8388608.0f;
                logMessage ("max export error " + juce::String (lsbs, 3) + " LSB");
                expect (lsbs <= 2.0f);
            }

            const auto empty = testDir().getChildFile ("empty.wav");
            expect (exportMix (std::make_shared<Project>(), source, empty).wasOk());
            std::unique_ptr<juce::AudioFormatReader> er (wav.createReaderFor (empty.createInputStream().release(), true));
            expect (er != nullptr && er->lengthInSamples == 0);
        }

        beginTest ("cancelling at 50 percent deletes the file and fails");
        {
            const auto project = makeTestProject (100000);
            SineSource source;
            const auto f = testDir().getChildFile ("cancel.wav");
            const auto r = exportMix (project, source, f, [] (float p) { return p < 0.5f; });
            expect (r.failed());
            expect (! f.existsAsFile());
        }

        testDir().deleteRecursively();
    }
};

static OfflineTests offlineTests;
}
