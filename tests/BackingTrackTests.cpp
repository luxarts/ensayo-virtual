#include "server/BackingTrack.h"

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

namespace ensayo
{
class BackingTrackTests : public juce::UnitTest
{
public:
    BackingTrackTests() : juce::UnitTest ("BackingTrack", "EnsayoVirtual") {}

    // 1 s, 44.1 kHz stereo, a +1.0 click every 250 ms (samples 0, 11025, 22050, 33075).
    static juce::File writeClickWav (juce::UnitTest& test)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("EnsayoVirtual").getChildFile ("tests");
        dir.createDirectory();
        auto file = dir.getChildFile ("click.wav");
        file.deleteFile();

        const double rate = 44100.0;
        juce::AudioBuffer<float> buffer (2, (int) rate);
        buffer.clear();

        for (int i = 0; i < (int) rate; i += 11025)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, 1.0f);

        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (file.createOutputStream());
        test.expect (stream != nullptr, "could not open " + file.getFullPathName());

        if (stream != nullptr)
        {
            const auto options = juce::AudioFormatWriterOptions().withSampleRate (rate).withNumChannels (2).withBitsPerSample (16);
            auto writer = wav.createWriterFor (stream, options);
            test.expect (writer != nullptr);

            if (writer != nullptr)
                writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        return file;
    }

    static int peakIndex (const std::vector<float>& data, int start, int end)
    {
        int best = start;

        for (int i = start; i < end; ++i)
            if (std::abs (data[(size_t) i]) > std::abs (data[(size_t) best]))
                best = i;

        return best;
    }

    void runTest() override
    {
        // expect() needs a current test: beginTest() must precede the fixture (which asserts the file was written).
        beginTest ("load converts to 48 kHz stereo");
        const auto file = writeClickWav (*this);
        BackingTrack track;
        {
            expect (! track.getState().loaded);
            const auto result = track.load (file);
            expect (result.wasOk(), result.getErrorMessage());
            const auto state = track.getState();
            expect (state.loaded);
            expect (! state.playing);
            expect (std::abs ((long long) state.length - 48000LL) <= 1, "length = " + juce::String (state.length));
            expectEquals (state.name, file.getFileName());
            expectEquals ((long long) state.position, 0LL);
        }

        beginTest ("render while paused leaves silence");
        {
            std::vector<float> l (1024, 0.0f), r (1024, 0.0f);
            track.render (l.data(), r.data(), 1024);
            expect (peakIndex (l, 0, 1024) == 0 && l[0] == 0.0f);
            expect (peakIndex (r, 0, 1024) == 0 && r[0] == 0.0f);
            expectEquals ((long long) track.getState().position, 0LL);
        }

        beginTest ("play renders clicks at 12000-sample spacing");
        {
            track.play();
            expect (track.getState().playing);
            std::vector<float> l (24000, 0.0f), r (24000, 0.0f);

            for (int i = 0; i < 24000; i += 120)
                track.render (l.data() + i, r.data() + i, 120);

            const int first = peakIndex (l, 0, 6000);
            const int second = peakIndex (r, 6000, 18000);
            expect (std::abs (first - 0) <= 8, "first click at " + juce::String (first));
            expect (std::abs (second - 12000) <= 8, "second click at " + juce::String (second));
            expect (std::abs (l[(size_t) first]) > 0.5f);
            expect (std::abs (r[(size_t) second]) > 0.5f);
            expectEquals ((long long) track.getState().position, 24000LL);
        }

        beginTest ("seek repositions");
        {
            track.seek (24000);
            expectEquals ((long long) track.getState().position, 24000LL);
            std::vector<float> l (6000, 0.0f), r (6000, 0.0f);
            track.render (l.data(), r.data(), 6000);
            const int idx = peakIndex (l, 0, 6000);
            expect (idx <= 8, "click at " + juce::String (idx));
            expect (std::abs (l[(size_t) idx]) > 0.5f);
        }

        beginTest ("end without loop pauses at length");
        {
            track.setLoop (false);
            track.seek (47000);
            std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
            track.render (l.data(), r.data(), 4096);
            const auto state = track.getState();
            expect (! state.playing);
            expectEquals ((long long) state.position, (long long) state.length);
        }

        beginTest ("loop wraps");
        {
            track.setLoop (true);
            track.seek (47000);
            track.play();
            std::vector<float> l (4096, 0.0f), r (4096, 0.0f);
            track.render (l.data(), r.data(), 4096);
            const auto state = track.getState();
            expect (state.playing);
            expect (state.loop);
            const auto expected = (long long) ((47000 + 4096) - state.length);
            expectEquals ((long long) state.position, expected);
            // The click at the start of the file shows up after the wrap.
            const int wrapIndex = (int) (state.length - 47000);
            const int idx = peakIndex (l, wrapIndex - 8, 4096);
            expect (std::abs (idx - wrapIndex) <= 8, "wrapped click at " + juce::String (idx));
        }

        beginTest ("getState from another thread never makes render skip a block");
        {
            // The audio thread try-locks: a control reader that took the same lock would make render drop the block
            // (track stalls by 120 samples per overlap). Hammer getState while rendering 10 s of looped clicks and
            // check that every block was produced: the clicks stay on their 12000-sample grid and the position
            // advanced by exactly the amount rendered.
            track.setLoop (true);
            track.stop();
            track.play();

            constexpr int blockSize = 120, numBlocks = 4000;   // 480 000 samples = 10 s
            std::vector<float> l ((size_t) blockSize * numBlocks, 0.0f), r (l.size(), 0.0f);
            std::atomic<bool> stopReader { false };
            std::atomic<int> reads { 0 };

            std::thread reader ([&]
            {
                while (! stopReader.load (std::memory_order_relaxed) || reads.load (std::memory_order_relaxed) < 10000)
                {
                    const auto state = track.getState();

                    if (state.loaded)
                        reads.fetch_add (1, std::memory_order_relaxed);
                }
            });

            for (int i = 0; i < numBlocks; ++i)
                track.render (l.data() + i * blockSize, r.data() + i * blockSize, blockSize);

            stopReader.store (true);
            reader.join();
            expect (reads.load() >= 10000, "reads = " + juce::String (reads.load()));

            const auto state = track.getState();
            const long long rendered = (long long) blockSize * numBlocks;
            // The loop wraps lazily: landing exactly on the end reads `length` until the next block.
            const long long expectedPosition = rendered % state.length;
            expect (state.position == expectedPosition || (expectedPosition == 0 && state.position == state.length),
                    "position = " + juce::String (state.position));

            // One click per 12000-sample cell, on the grid (a skipped block would shift it by 120), nothing between.
            int clicks = 0, offGrid = 0, stray = 0;
            const int cells = (int) (rendered / 12000);

            for (int k = 0; k < cells; ++k)
            {
                const int centre = k * 12000;
                const int from = juce::jmax (0, centre - 50), to = juce::jmin ((int) l.size(), centre + 50);
                const int idx = peakIndex (l, from, to);

                if (l[(size_t) idx] > 0.5f)
                    ++clicks;

                if (std::abs (idx - centre) > 8)
                    ++offGrid;

                for (int i = to; i < juce::jmin ((int) l.size(), centre + 12000 - 50); ++i)
                    if (l[(size_t) i] > 0.5f)
                        ++stray;
            }

            expectEquals (clicks, cells);
            expectEquals (offGrid, 0);
            expectEquals (stray, 0);
        }

        beginTest ("stop pauses and rewinds");
        {
            track.stop();
            const auto state = track.getState();
            expect (! state.playing);
            expectEquals ((long long) state.position, 0LL);
        }

        beginTest ("unload clears state");
        {
            track.unload();
            const auto state = track.getState();
            expect (! state.loaded);
            expectEquals ((long long) state.length, 0LL);
        }

        beginTest ("unsupported file fails");
        {
            auto bad = file.getSiblingFile ("not-audio.txt");
            bad.replaceWithText ("hello");
            BackingTrack other;
            expect (other.load (bad).failed());
            expect (! other.getState().loaded);
            expect (other.load (file.getSiblingFile ("missing.wav")).failed());
        }
    }
};

static BackingTrackTests backingTrackTests;
}
