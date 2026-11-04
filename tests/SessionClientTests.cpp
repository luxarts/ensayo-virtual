#include "client/SessionClient.h"
#include "client/SessionLink.h"
#include "core/Sha1.h"
#include "FakeControlServer.h"
#include "MessagePump.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>
#include <vector>

namespace ensayo
{
class SessionClientTests : public juce::UnitTest
{
public:
    SessionClientTests() : juce::UnitTest ("SessionClient", "EnsayoVirtual") {}

    static constexpr int tcpPort = 22170, udpPort = 22171;

    static void pump (int ms) { pumpMessages (ms); }

    // Pumps the message loop until the predicate holds or the timeout passes.
    static bool waitFor (const std::function<bool()>& predicate, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (predicate())
                return true;

            pump (10);
        }

        return predicate();
    }

    static juce::File writeTestWav()
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("EnsayoVirtual").getChildFile ("tests");
        dir.createDirectory();
        const auto file = dir.getChildFile ("session-client-upload.wav");
        file.deleteFile();

        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        auto writer = format.createWriterFor (stream, juce::AudioFormatWriterOptions()
                                                          .withSampleRate (44100.0)
                                                          .withNumChannels (2)
                                                          .withBitsPerSample (16));

        if (writer != nullptr)
        {
            juce::AudioBuffer<float> buffer (2, 44100);
            buffer.clear();

            for (int i = 0; i < 44100; i += 11025)
                for (int ch = 0; ch < 2; ++ch)
                    buffer.setSample (ch, i, 1.0f);

            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
        }

        return file;
    }

    static juce::File writeBigFile (int bytes)
    {
        const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                             .getChildFile ("EnsayoVirtual").getChildFile ("tests");
        dir.createDirectory();
        const auto file = dir.getChildFile ("session-client-upload-big.bin");
        file.deleteFile();

        juce::FileOutputStream out (file);
        std::vector<char> block (65536);

        for (int written = 0; out.openedOk() && written < bytes; written += (int) block.size())
        {
            for (size_t i = 0; i < block.size(); ++i)
                block[i] = (char) ((i * 31 + (size_t) written) & 0xff);

            out.write (block.data(), block.size());
        }

        return file;
    }

    void runTest() override
    {
        beginTest ("Sha1 known vectors");
        expectEquals (Sha1::hexOf ("abc", 3), juce::String ("a9993e364706816aba3e25717850c26c9cd0d89d"));
        expectEquals (Sha1::hexOf ("", 0), juce::String ("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
        {
            const juce::String million = juce::String::repeatedString ("a", 1000000);
            expectEquals (Sha1::hexOf (million.toRawUTF8(), 1000000), juce::String ("34aa973cd4c4daa4f61eeb2bdbad27316534016f"));
        }

        FakeControlServer server (tcpPort, udpPort, FrameSize::f128);
        expect (server.start(), "could not start the fake server on ports " + juce::String (tcpPort) + "/" + juce::String (udpPort));

        beginTest ("create joins a new session");

        SessionClient a;
        bool aJoined = false;
        juce::String aError;
        int aChanges = 0;
        a.onJoined = [&] { aJoined = true; };
        a.onError = [&] (const juce::String& e) { aError = e; };
        a.onStateChanged = [&] (const SessionState&) { ++aChanges; };

        a.create ("127.0.0.1", tcpPort, "Ana", Role::both);

        const bool aOk = waitFor ([&] { return aJoined; }, 5000);   // evaluated before the message uses aError
        expect (aOk, "A never joined: " + aError);
        expect (a.getState().connected);
        expectEquals (a.getState().sessionId, server.getSessionId());
        expectEquals ((int) a.getState().clientId, 1);
        expectEquals ((int) a.getClientId(), 1);
        expectEquals (a.getState().host, juce::String ("127.0.0.1"));
        expectEquals (a.getState().port, tcpPort);
        expect (a.getState().frameSize == FrameSize::f128);
        expectEquals ((int) a.getState().participants.size(), 1);
        expect (aChanges > 0, "onStateChanged never fired");

        beginTest ("join with the link");

        SessionLink link;
        link.host = "127.0.0.1";
        link.port = tcpPort;
        link.sessionId = a.getState().sessionId;

        SessionClient b;
        bool bJoined = false;
        juce::String bError;
        b.onJoined = [&] { bJoined = true; };
        b.onError = [&] (const juce::String& e) { bError = e; };

        b.join (link, "Beto", Role::voice);

        const bool bOk = waitFor ([&] { return bJoined; }, 5000);
        expect (bOk, "B never joined: " + bError);
        expectEquals ((int) b.getState().clientId, 2);
        expect (waitFor ([&] { return a.getState().participants.size() == 2 && b.getState().participants.size() == 2; }, 5000),
                "participants not broadcast to both");

        if (b.getState().participants.size() == 2)
        {
            expectEquals (b.getState().participants[0].name, juce::String ("Ana"));
            expect (b.getState().participants[0].role == Role::both);
            expectEquals (b.getState().participants[1].name, juce::String ("Beto"));
            expect (b.getState().participants[1].role == Role::voice);
        }

        beginTest ("join with an unknown session reports an error");
        {
            SessionClient c;
            bool cJoined = false;
            juce::String cError;
            c.onJoined = [&] { cJoined = true; };
            c.onError = [&] (const juce::String& e) { if (cError.isEmpty()) cError = e; };

            SessionLink bad = link;
            bad.sessionId = "ZZZZZZ";
            c.join (bad, "Carla", Role::instrument);

            expect (waitFor ([&] { return cError.isNotEmpty(); }, 5000), "no error for an unknown session");
            expect (! cJoined);
            expect (! c.getState().connected);
        }

        beginTest ("ping updates pingMs");
        expect (waitFor ([&] { return a.getState().pingMs >= 0; }, 5000), "pingMs never updated");

        beginTest ("sendGain and sendTransport");
        a.sendGain (2, ChannelGain { -6.0f, 0.0f, false });
        a.sendGain (0, ChannelGain { 0.0f, 0.0f, true });
        b.sendTransport ("loop", 0, true);
        expect (waitFor ([&] { return a.getState().transport.loop && b.getState().transport.loop; }, 5000),
                "transport_state not received");

        beginTest ("uploadTrack");
        const auto wav = writeTestWav();
        expect (wav.existsAsFile());
        a.uploadTrack (wav);
        const bool loaded = waitFor ([&] { return a.getState().transport.loaded && b.getState().transport.loaded; }, 5000);
        expect (loaded, "transport.loaded not set after upload: " + aError);
        expectEquals ((juce::int64) server.getUploadedBytes(), wav.getSize());
        expectEquals (a.getState().transport.name, wav.getFileName());
        expect (a.getState().trackUploadProgress >= 1.0f, "upload progress did not reach 1");

        a.sendTransport ("play");
        expect (waitFor ([&] { return a.getState().transport.playing && b.getState().transport.playing; }, 5000),
                "transport.playing not set");

        beginTest ("sendGain returns at once while an upload is on the wire");
        {
            const auto big = writeBigFile (8 * 1024 * 1024);
            expect (big.existsAsFile());

            server.setUploadReadDelayMs (5);   // 128 reads of 64 KB: the upload stays on the wire for >= 0.6 s

            a.uploadTrack (big);
            expect (waitFor ([&] { return a.getState().trackUploadStatus.startsWith ("Subiendo"); }, 20000),
                    "upload never started: " + a.getState().trackUploadStatus);
            expect (server.getUploadedBytes() != (int) big.getSize(), "upload already finished before sendGain was called");

            const auto t0 = juce::Time::getMillisecondCounterHiRes();
            a.sendGain (2, ChannelGain { -3.0f, 0.0f, false });
            const auto sendMs = juce::Time::getMillisecondCounterHiRes() - t0;
            expect (sendMs < 50.0, "sendGain blocked for " + juce::String (sendMs, 1) + " ms during an upload");

            const bool bigLoaded = waitFor ([&] { return a.getState().transport.name == big.getFileName()
                                                        && a.getState().trackUploadStatus.startsWith ("Listo"); }, 60000);
            expect (bigLoaded, "big upload did not complete: " + aError + " / " + a.getState().trackUploadStatus);
            expectEquals ((juce::int64) server.getUploadedBytes(), big.getSize());
            expectEquals (server.getLastUploadSha1(), Sha1::hexOfFile (big));
            expect (a.getState().trackUploadProgress >= 1.0f, "upload progress did not reach 1");

            server.setUploadReadDelayMs (0);
            big.deleteFile();
        }

        beginTest ("audio round trip primes the receive buffer");
        {
            std::vector<uint8_t> payload ((size_t) codedBytesMono (FrameSize::f128), 0x11);
            std::vector<uint8_t> packet (maxPacketBytes);
            std::vector<uint8_t> block ((size_t) codedBytesStereo (FrameSize::f128) + 16);
            uint16_t seq = 0;
            uint32_t sampleTime = 0;
            int popped = 0, poppedLen = 0;

            // Plays the audio thread: four packets out, then four pops (the buffer only primes on pop).
            const bool primed = waitFor ([&]
            {
                for (int i = 0; i < 4; ++i)
                {
                    int len = 0;

                    if (b.receiveBuffer().pop (block.data(), len))
                    {
                        ++popped;
                        poppedLen = len;
                    }
                }

                for (int i = 0; i < 4; ++i)
                {
                    AudioPacketView view;
                    view.flags = AudioFlags::voice;
                    view.frameSize = FrameSize::f128;
                    view.seq = seq++;
                    view.clientId = b.getClientId();
                    view.sampleTime = sampleTime;
                    sampleTime += 128;
                    view.voice = payload.data();
                    view.voiceLen = (int) payload.size();

                    const auto len = encodeAudioPacket (view, packet.data(), packet.size());
                    b.sendAudio (packet.data(), len);
                }

                return b.receiveBuffer().getStats().primed && popped > 0;
            }, 5000);

            expect (primed, "B's receive buffer never primed");
            expect (server.getUdpPacketsReceived() > 0, "server received no UDP packets");
            expect (b.getTrackPosition() > 0, "trackPosition not stored from mix packets");
            expect (popped > 0, "no mix block could be popped");
            expectEquals (poppedLen, codedBytesStereo (FrameSize::f128));
        }

        beginTest ("leave disconnects");
        b.leave();
        expect (! b.getState().connected);
        expect (waitFor ([&] { return a.getState().participants.size() == 1; }, 5000), "A not told about B leaving");

        beginTest ("server going away fires onError and disconnects");
        aError.clear();
        server.stop();
        expect (waitFor ([&] { return ! a.getState().connected; }, 5000), "A still connected after the server stopped");
        expect (aError.isNotEmpty(), "no error reported on disconnect");

        a.leave();
        pump (50);
        wav.deleteFile();
    }
};

static SessionClientTests sessionClientTests;
}

