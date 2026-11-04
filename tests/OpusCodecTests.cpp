#include "core/OpusCodec.h"

#include <cmath>
#include <vector>

namespace ensayo
{
class OpusCodecTests : public juce::UnitTest
{
public:
    OpusCodecTests() : juce::UnitTest ("OpusCodec", "EnsayoVirtual") {}

    static std::vector<float> makeSine (int totalSamples, int channels, float frequency)
    {
        std::vector<float> pcm ((size_t) totalSamples * (size_t) channels);

        for (int i = 0; i < totalSamples; ++i)
        {
            const float s = 0.5f * std::sin (juce::MathConstants<float>::twoPi * frequency * (float) i / (float) sampleRate);

            for (int c = 0; c < channels; ++c)
                pcm[(size_t) (i * channels + c)] = s;
        }

        return pcm;
    }

    // SNR (dB) of `decoded` against `reference` searching the codec delay in [0, maxDelay] frames.
    static double bestSnrDb (const std::vector<float>& reference, const std::vector<float>& decoded,
                             int frameIndex, int frame, int channels, int maxDelay)
    {
        double best = -200.0;

        for (int delay = 0; delay <= maxDelay; ++delay)
        {
            double signal = 0.0, noise = 0.0;

            for (int i = 0; i < frame * channels; ++i)
            {
                const auto outIndex = (size_t) (frameIndex * frame * channels + i);
                const auto inIndex = (size_t) ((int) outIndex - delay * channels);
                const double in = reference[inIndex];
                const double out = decoded[outIndex];
                signal += in * in;
                noise += (in - out) * (in - out);
            }

            const double snr = 10.0 * std::log10 (signal / juce::jmax (noise, 1.0e-12));
            best = juce::jmax (best, snr);
        }

        return best;
    }

    void roundTrip (FrameSize size, int channels, double minSnrDb)
    {
        const int frame = frameSamples (size);
        const int numFrames = 12;
        const int expectedBytes = channels == 1 ? codedBytesMono (size) : codedBytesStereo (size);

        OpusMode mode (size);
        expect (mode.get() != nullptr);
        expect (mode.frameSize() == size);

        OpusEncoder encoder (mode, channels);
        OpusDecoder decoder (mode, channels);
        expectEquals (encoder.codedBytes(), expectedBytes);

        const auto input = makeSine (frame * numFrames, channels, 440.0f);
        std::vector<float> output (input.size(), 0.0f);
        std::vector<uint8_t> packet ((size_t) expectedBytes + 32, 0);

        for (int f = 0; f < numFrames; ++f)
        {
            const auto written = encoder.encode (input.data() + (size_t) (f * frame * channels), packet.data(), (int) packet.size());
            expectEquals (written, expectedBytes);
            decoder.decode (packet.data(), written, output.data() + (size_t) (f * frame * channels));
        }

        const auto snr = bestSnrDb (input, output, 10, frame, channels, 2 * frame);
        logMessage ("SNR frame 10 (" + juce::String (channels) + " ch, " + juce::String (frame) + " samples): "
                    + juce::String (snr, 1) + " dB");
        expect (snr >= minSnrDb, "SNR too low: " + juce::String (snr, 1) + " dB");
    }

    void runTest() override
    {
        beginTest ("mono 128 round trip");
        roundTrip (FrameSize::f128, 1, 15.0);

        beginTest ("stereo 64 round trip");
        roundTrip (FrameSize::f64, 2, 15.0);

        beginTest ("stereo 128 and mono 64 produce exactly codedBytes");
        {
            OpusMode mode128 (FrameSize::f128);
            OpusMode mode64 (FrameSize::f64);
            OpusEncoder stereo (mode128, 2);
            OpusEncoder mono (mode64, 1);
            expectEquals (stereo.codedBytes(), codedBytesStereo (FrameSize::f128));
            expectEquals (mono.codedBytes(), codedBytesMono (FrameSize::f64));

            const auto in128 = makeSine (128, 2, 220.0f);
            const auto in64 = makeSine (64, 1, 220.0f);
            std::vector<uint8_t> out (256, 0);

            for (int i = 0; i < 5; ++i)
            {
                expectEquals (stereo.encode (in128.data(), out.data(), (int) out.size()), codedBytesStereo (FrameSize::f128));
                expectEquals (mono.encode (in64.data(), out.data(), (int) out.size()), codedBytesMono (FrameSize::f64));
            }

            // Silence must also be exactly codedBytes (CBR).
            const std::vector<float> silence (256, 0.0f);
            expectEquals (stereo.encode (silence.data(), out.data(), (int) out.size()), codedBytesStereo (FrameSize::f128));
        }

        beginTest ("packet loss concealment outputs finite values");
        {
            OpusMode mode (FrameSize::f128);
            OpusEncoder encoder (mode, 1);
            OpusDecoder decoder (mode, 1);

            const auto input = makeSine (128 * 4, 1, 440.0f);
            std::vector<uint8_t> packet (64, 0);
            std::vector<float> output (128, 0.0f);

            for (int f = 0; f < 4; ++f)
            {
                const auto written = encoder.encode (input.data() + (size_t) (f * 128), packet.data(), (int) packet.size());
                decoder.decode (packet.data(), written, output.data());
            }

            output.assign (128, 123.0f);
            decoder.decode (nullptr, 0, output.data());

            bool allFinite = true;
            float peak = 0.0f;

            for (auto s : output)
            {
                allFinite = allFinite && std::isfinite (s);
                peak = juce::jmax (peak, std::abs (s));
            }

            expect (allFinite, "PLC produced non-finite samples");
            expect (peak < 2.0f, "PLC output unexpectedly loud");
            expect (peak > 0.0f, "PLC after a sine should not be pure silence");
        }

        beginTest ("encode with insufficient capacity returns 0");
        {
            OpusMode mode (FrameSize::f128);
            OpusEncoder encoder (mode, 1);
            const auto input = makeSine (128, 1, 440.0f);
            std::vector<uint8_t> out (64, 0);
            expectEquals (encoder.encode (input.data(), out.data(), encoder.codedBytes() - 1), 0);
            expectEquals (encoder.encode (input.data(), out.data(), 0), 0);
            expectEquals (encoder.encode (nullptr, out.data(), (int) out.size()), 0);
            expectEquals (encoder.encode (input.data(), nullptr, (int) out.size()), 0);
            expectEquals (encoder.encode (input.data(), out.data(), encoder.codedBytes()), encoder.codedBytes());
        }

        beginTest ("decoding garbage does not crash");
        {
            OpusMode mode (FrameSize::f64);
            OpusDecoder decoder (mode, 2);
            std::vector<uint8_t> garbage (48, 0xFF);
            std::vector<float> output (128, 0.0f);
            decoder.decode (garbage.data(), (int) garbage.size(), output.data());
            decoder.decode (garbage.data(), 1, output.data());

            bool allFinite = true;

            for (auto s : output)
                allFinite = allFinite && std::isfinite (s);

            expect (allFinite);
        }
    }
};

static OpusCodecTests opusCodecTests;
}
