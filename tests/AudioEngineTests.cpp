#include "client/AudioEngine.h"

#include <cmath>
#include <vector>

namespace ensayo
{
class AudioEngineTests : public juce::UnitTest
{
public:
    AudioEngineTests() : juce::UnitTest ("AudioEngine", "EnsayoVirtual") {}

    // Collects everything the engine sends and owns the receive jitter buffer the test feeds.
    struct StubTransport : public AudioTransport
    {
        StubTransport() { rx.setAutoSize (false); }

        void sendAudio (const uint8_t* packet, size_t len) noexcept override { sent.push (packet, len); }
        JitterBuffer& receiveBuffer() noexcept override { return rx; }
        uint32_t getClientId() const noexcept override { return 7; }
        uint32_t getTrackPosition() const noexcept override { return 0; }

        PacketQueue sent { 1024 };
        JitterBuffer rx { codedBytesStereo (FrameSize::f128), 128000.0 / sampleRate };
    };

    static std::vector<AudioPacketView> drain (StubTransport& t, std::vector<std::vector<uint8_t>>& storage)
    {
        std::vector<AudioPacketView> views;
        std::vector<uint8_t> packet (maxPacketBytes);
        size_t len = 0;

        while (t.sent.pop (packet.data(), len))
        {
            storage.emplace_back (packet.begin(), packet.begin() + (long) len);
            AudioPacketView view;

            if (decodeAudioPacket (storage.back().data(), len, view))
                views.push_back (view);
        }

        return views;
    }

    // Pushes `blocks` stereo-encoded 440 Hz sine frames (amplitude 0.5) paced close to real time so the
    // jitter buffer window never runs more than a few blocks ahead of the audio thread.
    static void feedSine (StubTransport& t, int blocks)
    {
        OpusMode mode (FrameSize::f128);
        OpusEncoder encoder (mode, 2);
        std::vector<float> pcm (256);
        std::vector<uint8_t> coded (128);
        uint16_t seq = 0;
        const double startMs = juce::Time::getMillisecondCounterHiRes();

        for (int b = 0; b < blocks; ++b)
        {
            for (int i = 0; i < 128; ++i)
            {
                const auto s = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * (float) (b * 128 + i) / (float) sampleRate);
                pcm[(size_t) (2 * i)] = s;
                pcm[(size_t) (2 * i + 1)] = s;
            }

            const int n = encoder.encode (pcm.data(), coded.data(), (int) coded.size());
            t.rx.push (seq++, coded.data(), n, startMs + b * 128000.0 / sampleRate);

            if ((b + 1) % 6 == 0)
                juce::Thread::sleep (16);
        }
    }

    void runTest() override
    {
        beginTest ("fake device opens through the device manager");
        {
            AudioEngine engine;
            engine.useFakeDevice (128, TestSignal::sine440);
            expect (engine.getDevice() != nullptr);
            expect (engine.getFakeDevice() != nullptr);
            expectEquals (engine.getDevice()->getTypeName(), juce::String ("Fake"));
            expectEquals (engine.getDevice()->getCurrentBufferSizeSamples(), 128);
            expect (engine.getFrameSize() == FrameSize::f128);
            expectEquals (engine.getDeviceManager().getCurrentAudioDeviceType(), juce::String ("Fake"));
            expectEquals (engine.takeDeviceError(), juce::String());

            juce::Thread::sleep (100);
            expect (engine.getCallbackCount() > 20, "callbacks: " + juce::String (engine.getCallbackCount()));
        }

        beginTest ("monitor faders apply to the local mix");
        {
            AudioEngine engine;
            engine.setMonitor ({ 0.0f, 0.0f, false });
            engine.useFakeDevice (128, TestSignal::sine440);
            juce::Thread::sleep (60);
            engine.takePeaks();
            juce::Thread::sleep (100);

            auto peaks = engine.takePeaks();
            logMessage ("voice " + juce::String (peaks.voice) + " instrument " + juce::String (peaks.instrument) + " output " + juce::String (peaks.output));
            expect (std::abs (peaks.voice - 0.5f) < 0.02f, "voice peak " + juce::String (peaks.voice));
            expect (std::abs (peaks.instrument - 0.5f) < 0.02f, "instrument peak " + juce::String (peaks.instrument));
            expect (std::abs (peaks.output - 1.0f) < 0.03f, "output peak " + juce::String (peaks.output));
            expectEquals (peaks.received, 0.0f);

            engine.setMonitor ({ -60.0f, -60.0f, false });
            juce::Thread::sleep (30);
            engine.takePeaks();
            juce::Thread::sleep (60);
            peaks = engine.takePeaks();
            expect (peaks.output < 0.01f, "output peak after -60 dB: " + juce::String (peaks.output));

            engine.setMonitor ({ 0.0f, -60.0f, false });
            juce::Thread::sleep (30);
            engine.takePeaks();
            juce::Thread::sleep (60);
            peaks = engine.takePeaks();
            expect (std::abs (peaks.output - 0.5f) < 0.03f, "voice-only output peak " + juce::String (peaks.output));

            engine.setMonitor ({ 0.0f, 0.0f, true });
            juce::Thread::sleep (30);
            engine.takePeaks();
            juce::Thread::sleep (60);
            peaks = engine.takePeaks();
            expect (peaks.output < 0.01f, "output peak when muted: " + juce::String (peaks.output));
        }

        beginTest ("role voice sends only the voice payload");
        {
            AudioEngine engine;
            StubTransport transport;
            engine.setRole (Role::voice);
            engine.setTransport (&transport);
            engine.useFakeDevice (128, TestSignal::sine440);
            juce::Thread::sleep (150);
            engine.setTransport (nullptr);
            juce::Thread::sleep (20);

            std::vector<std::vector<uint8_t>> storage;
            const auto packets = drain (transport, storage);
            expect (packets.size() >= 30, "packets: " + juce::String (packets.size()));
            expectEquals (packets.size(), storage.size());

            for (size_t i = 0; i < packets.size(); ++i)
            {
                expectEquals ((int) packets[i].flags, (int) AudioFlags::voice);
                expectEquals (packets[i].voiceLen, codedBytesMono (FrameSize::f128));
                expectEquals (packets[i].instrumentLen, 0);
                expectEquals (packets[i].clientId, (uint32_t) 7);
                expect (packets[i].frameSize == FrameSize::f128);

                if (i > 0)
                {
                    expectEquals ((int) packets[i].seq, (int) (uint16_t) (packets[i - 1].seq + 1));
                    expectEquals (packets[i].sampleTime, packets[i - 1].sampleTime + 128);
                }
            }
        }

        beginTest ("role both sends both payloads and block 256 sends two frames per callback");
        {
            AudioEngine engine;
            StubTransport transport;
            engine.setRole (Role::both);
            engine.setTransport (&transport);
            engine.useFakeDevice (256, TestSignal::sine440);
            expect (engine.getFrameSize() == FrameSize::f128);
            juce::Thread::sleep (150);
            engine.setTransport (nullptr);
            juce::Thread::sleep (20);

            const auto callbacks = (int) engine.getCallbackCount();
            std::vector<std::vector<uint8_t>> storage;
            const auto packets = drain (transport, storage);
            logMessage ("callbacks " + juce::String (callbacks) + " packets " + juce::String (packets.size()));
            expect (packets.size() >= 30);
            expect ((int) packets.size() >= 2 * (callbacks - 12) && (int) packets.size() <= 2 * callbacks,
                    "expected two packets per callback");

            for (const auto& p : packets)
            {
                expectEquals ((int) p.flags, (int) (AudioFlags::voice | AudioFlags::instrument));
                expectEquals (p.voiceLen, codedBytesMono (FrameSize::f128));
                expectEquals (p.instrumentLen, codedBytesMono (FrameSize::f128));
                expectEquals ((int) storage.front().size(), (int) audioHeaderBytes + 2 * codedBytesMono (FrameSize::f128));
            }
        }

        beginTest ("role instrument sends the post-plugin instrument only");
        {
            AudioEngine engine;
            StubTransport transport;
            engine.setRole (Role::instrument);
            engine.setTransport (&transport);
            engine.useFakeDevice (64, TestSignal::sine440);
            expect (engine.getFrameSize() == FrameSize::f64);
            juce::Thread::sleep (100);
            engine.setTransport (nullptr);
            juce::Thread::sleep (20);

            std::vector<std::vector<uint8_t>> storage;
            const auto packets = drain (transport, storage);
            expect (packets.size() >= 30);

            for (const auto& p : packets)
            {
                expectEquals ((int) p.flags, (int) AudioFlags::instrument);
                expectEquals (p.voiceLen, 0);
                expectEquals (p.instrumentLen, codedBytesMono (FrameSize::f64));
                expect (p.frameSize == FrameSize::f64);
            }

            // The sent instrument decodes back to the sine: the instrument really was encoded.
            OpusMode mode (FrameSize::f64);
            OpusDecoder decoder (mode, 1);
            std::vector<float> pcm (64);
            float peak = 0.0f;

            for (const auto& p : packets)
            {
                decoder.decode (p.instrument, p.instrumentLen, pcm.data());
                peak = juce::jmax (peak, peakOf (pcm.data(), 64));
            }

            expect (peak > 0.3f && peak < 0.7f, "decoded instrument peak " + juce::String (peak));
        }

        beginTest ("received mix is decoded into the output with the receive gain");
        {
            AudioEngine engine;
            StubTransport transport;
            engine.setMonitor ({ -60.0f, -60.0f, true });
            engine.setReceiveGain (-6.0f);
            engine.setTransport (&transport);
            engine.useFakeDevice (128, TestSignal::silence);
            juce::Thread::sleep (30);
            engine.takePeaks();

            feedSine (transport, 150);
            juce::Thread::sleep (30);

            const auto peaks = engine.takePeaks();
            logMessage ("received " + juce::String (peaks.received) + " output " + juce::String (peaks.output));
            expect (peaks.received > 0.3f && peaks.received < 0.7f, "received peak " + juce::String (peaks.received));
            expect (peaks.output > 0.15f && peaks.output < 0.35f, "output peak " + juce::String (peaks.output));
            expectEquals (peaks.voice, 0.0f);

            const auto stats = transport.rx.getStats();
            expect (stats.primed);
            // The feed loop sleeps between bursts, so in Debug the device can outrun it; the buffer then
            // re-anchors (a resync) instead of dropping the rest of the stream, and the peaks above prove it.

            // When the stream stops, PLC is bounded: after a short fade the output is silent, not comfort noise.
            juce::Thread::sleep (150);
            engine.takePeaks();
            juce::Thread::sleep (100);
            const auto after = engine.takePeaks();
            expect (after.received < 0.01f, "received peak after the stream stopped: " + juce::String (after.received));
            engine.setTransport (nullptr);
        }

        beginTest ("simulated total loss keeps the output silent without crashing");
        {
            AudioEngine engine;
            StubTransport transport;
            engine.setMonitor ({ -60.0f, -60.0f, true });
            engine.setSimulatedLossPercent (100);
            engine.setTransport (&transport);
            engine.useFakeDevice (128, TestSignal::silence);
            juce::Thread::sleep (30);
            engine.takePeaks();

            feedSine (transport, 100);
            juce::Thread::sleep (30);

            const auto peaks = engine.takePeaks();
            expect (peaks.received < 0.01f, "received peak with 100 % loss: " + juce::String (peaks.received));
            expect (peaks.output < 0.01f);
            expect (engine.getCallbackCount() > 50);
            engine.setTransport (nullptr);
        }

        beginTest ("frame hook runs once per frame on the audio thread");
        {
            AudioEngine engine;
            std::atomic<int> hooks { 0 };
            engine.setFrameHook ([&] { hooks.fetch_add (1); });
            engine.useFakeDevice (256, TestSignal::silence);
            juce::Thread::sleep (120);
            const auto callbacks = (int) engine.getCallbackCount();
            const auto count = hooks.load();
            expect (count >= 2 * (callbacks - 4) && count <= 2 * callbacks + 2,
                    "hooks " + juce::String (count) + " callbacks " + juce::String (callbacks));
        }

        beginTest ("channel selection follows setChannels");
        {
            AudioEngine engine;
            engine.setMonitor ({ 0.0f, -60.0f, false });
            engine.setChannels (5, 1, 0);   // mic channel 5 does not exist on the fake device → silence
            engine.useFakeDevice (128, TestSignal::sine440);
            juce::Thread::sleep (50);
            engine.takePeaks();
            juce::Thread::sleep (60);
            auto peaks = engine.takePeaks();
            expectEquals (peaks.voice, 0.0f);
            expect (peaks.output < 0.01f);

            engine.setChannels (1, 0, 3);   // output pair 3 does not exist → nothing audible, but peaks still measure inputs
            juce::Thread::sleep (30);
            engine.takePeaks();
            juce::Thread::sleep (60);
            peaks = engine.takePeaks();
            expect (std::abs (peaks.voice - 0.5f) < 0.02f);
        }

        beginTest ("ASIO device names can be listed and an unknown device fails cleanly");
        {
            AudioEngine engine;
            const auto names = engine.getAsioDeviceNames();
            logMessage ("ASIO drivers: " + names.joinIntoString (", "));
            expect (engine.openDevice ("ASIO", "Ensayo No Such Driver", 48000.0, 128).isNotEmpty());
            expect (engine.openDevice ("Nope", "x", 48000.0, 128).isNotEmpty());
            expect (engine.getDevice() == nullptr);
        }
    }
};

static AudioEngineTests audioEngineTests;
}
