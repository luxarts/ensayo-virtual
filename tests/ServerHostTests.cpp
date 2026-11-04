#include "server/ServerHost.h"
#include "server/HighResClock.h"
#include "core/Sha1.h"

#include <cmath>
#include <deque>
#include <vector>

namespace ensayo
{
class ServerHostTests : public juce::UnitTest
{
public:
    ServerHostTests() : juce::UnitTest ("ServerHost", "EnsayoVirtual") {}

    static constexpr int tcpPort = 22160, udpPort = 22161;

    // A line-oriented JSON client that keeps unmatched messages so broadcasts can be awaited out of order.
    struct Client
    {
        bool connect()
        {
            return socket.connect ("127.0.0.1", tcpPort, 3000);
        }

        void send (const juce::var& message)
        {
            const auto line = makeLine (message);
            socket.write (line.toRawUTF8(), (int) line.getNumBytesAsUTF8());
        }

        void sendRaw (const void* data, int len)
        {
            socket.write (data, len);
        }

        // Returns the first queued or newly received message of `type` accepted by `predicate` within timeoutMs.
        template <typename Predicate>
        juce::var waitFor (const juce::String& type, int timeoutMs, Predicate predicate)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            for (;;)
            {
                for (auto it = queue.begin(); it != queue.end(); ++it)
                {
                    if ((*it)["t"].toString() == type && predicate (*it))
                    {
                        auto message = *it;
                        queue.erase (it);
                        return message;
                    }
                }

                const auto now = juce::Time::getMillisecondCounter();

                if (now >= deadline)
                    return {};

                if (! pump ((int) juce::jmin ((juce::uint32) 100, deadline - now)))
                    return {};
            }
        }

        juce::var waitFor (const juce::String& type, int timeoutMs = 3000)
        {
            return waitFor (type, timeoutMs, [] (const juce::var&) { return true; });
        }

        // Reads whatever is available (waiting up to timeoutMs), parses complete lines. False when closed.
        bool pump (int timeoutMs)
        {
            const auto ready = socket.waitUntilReady (true, timeoutMs);

            if (ready < 0)
                return false;

            if (ready == 0)
                return true;

            char buffer[4096];
            const auto n = socket.read (buffer, sizeof (buffer), false);

            if (n <= 0)
                return false;

            pending += std::string (buffer, (size_t) n);

            for (;;)
            {
                const auto newline = pending.find ('\n');

                if (newline == std::string::npos)
                    break;

                const juce::String line (pending.substr (0, newline));
                pending.erase (0, newline + 1);

                if (auto parsed = parseLine (line))
                    queue.push_back (*parsed);
            }

            return true;
        }

        // True when the server closes the connection within timeoutMs (replies received meanwhile are queued).
        bool waitForClose (int timeoutMs)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            while (juce::Time::getMillisecondCounter() < deadline)
                if (! pump (100))
                    return true;

            return false;
        }

        juce::StreamingSocket socket;
        std::string pending;
        std::deque<juce::var> queue;
    };

    static juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();

        for (const auto& [key, value] : fields)
            o->setProperty (key, value);

        return juce::var (o);
    }

    static int participantCount (const juce::var& message)
    {
        if (auto* list = message["participants"].getArray())
            return list->size();

        return -1;
    }

    static juce::MemoryBlock makeClickWav()
    {
        juce::AudioBuffer<float> buffer (2, 44100);
        buffer.clear();

        for (int i = 0; i < 44100; i += 11025)
            for (int ch = 0; ch < 2; ++ch)
                buffer.setSample (ch, i, 1.0f);

        juce::MemoryBlock block;
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::OutputStream> stream (new juce::MemoryOutputStream (block, false));
        const auto options = juce::AudioFormatWriterOptions().withSampleRate (44100.0).withNumChannels (2).withBitsPerSample (16);
        auto writer = wav.createWriterFor (stream, options);

        if (writer != nullptr)
        {
            writer->writeFromAudioSampleBuffer (buffer, 0, buffer.getNumSamples());
            writer->flush();
        }

        writer.reset();
        return block;
    }

    void runTest() override
    {
        beginTest ("SHA-1 known answers");
        {
            expectEquals (Sha1::of ("abc", 3), juce::String ("a9993e364706816aba3e25717850c26c9cd0d89d"));
            expectEquals (Sha1::of ("", 0), juce::String ("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
            const std::string longer (1000, 'a');
            expectEquals (Sha1::of (longer.data(), longer.size()), juce::String ("291e9a6c66994949b57ba5e650361e98fc36b1ba"));
        }

        beginTest ("HighResClock keeps its period");
        {
            std::atomic<int> count { 0 };
            HighResClock clock (2666.67, [&] { ++count; });
            clock.start();
            juce::Thread::sleep (500);
            clock.stop();
            const auto stats = clock.getStats();
            logMessage ("ticks " + juce::String (count.load()) + ", max late " + juce::String (stats.maxLateUs, 1)
                        + " us, mean late " + juce::String (stats.meanLateUs, 1) + " us");
            expect (count.load() >= 168 && count.load() <= 206, "ticks = " + juce::String (count.load()));
            expectEquals ((long long) stats.ticks, (long long) count.load());
            expect (stats.maxLateUs < 3000.0, "max late = " + juce::String (stats.maxLateUs, 1));

            // Restartable.
            clock.start();
            juce::Thread::sleep (50);
            clock.stop();
            expect (count.load() > 168);
        }

        ServerConfig config;
        config.tcpPort = tcpPort;
        config.udpPort = udpPort;
        config.frameSize = FrameSize::f128;
        config.uploadDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("uploads");

        ServerHost host (config);
        juce::StringArray logLines;
        juce::CriticalSection logLock;
        host.onLog = [&] (const juce::String& line)
        {
            const juce::ScopedLock scoped (logLock);
            logLines.add (line);
        };

        beginTest ("start binds TCP and UDP");
        {
            const auto result = host.start();
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (host.getUdpPort(), udpPort);
            expectEquals (host.getSessionCount(), 0);
        }

        Client one, two;
        juce::String sessionId;

        beginTest ("create returns joined with a session id and client 1");
        {
            expect (one.connect());
            one.send (object ({ { "t", "create" }, { "name", "Ana" }, { "role", "both" } }));
            const auto joined = one.waitFor ("joined");
            expect (! joined.isVoid(), "no joined message");
            sessionId = joined["session"].toString();
            expect (isValidSessionId (sessionId), "session id " + sessionId);
            expectEquals ((int) joined["clientId"], 1);
            expectEquals ((int) joined["udpPort"], udpPort);
            expectEquals ((int) joined["frameSize"], 128);
            expectEquals (participantCount (joined), 1);
            expectEquals (host.getSessionCount(), 1);
            expect (host.getSessionIds().size() == 1 && host.getSessionIds()[0] == sessionId);
        }

        beginTest ("join adds a second participant and broadcasts the list");
        {
            expect (two.connect());
            two.send (object ({ { "t", "join" }, { "session", sessionId.toLowerCase() }, { "name", "Beto" }, { "role", "voice" } }));
            const auto joined = two.waitFor ("joined");
            expect (! joined.isVoid(), "no joined message");
            expectEquals ((int) joined["clientId"], 2);
            expectEquals (joined["session"].toString(), sessionId);

            const auto p1 = one.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 2; });
            const auto p2 = two.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 2; });
            expect (! p1.isVoid(), "client 1 did not get participants with 2 entries");
            expect (! p2.isVoid(), "client 2 did not get participants with 2 entries");

            if (! p1.isVoid())
            {
                const auto& list = *p1["participants"].getArray();
                expectEquals ((int) list[0]["id"], 1);
                expectEquals (list[0]["name"].toString(), juce::String ("Ana"));
                expectEquals (list[1]["role"].toString(), juce::String ("voice"));
            }

            expectEquals (host.getSessionCount(), 1);
        }

        beginTest ("join with a bad id returns error");
        {
            Client three;
            expect (three.connect());
            three.send (object ({ { "t", "join" }, { "session", "ZZZZZZ" }, { "name", "Caro" }, { "role", "both" } }));
            expect (! three.waitFor ("error").isVoid(), "no error for unknown session");
            three.send (object ({ { "t", "join" }, { "session", "ab" }, { "name", "Caro" }, { "role", "both" } }));
            expect (! three.waitFor ("error").isVoid(), "no error for invalid session");
            three.send (object ({ { "t", "gain" }, { "target", 1 } }));
            expect (! three.waitFor ("error").isVoid(), "no error when not joined");
            three.send (object ({ { "t", "nonsense" } }));
            expect (! three.waitFor ("error").isVoid(), "no error for unknown message");
            expectEquals (host.getSessionCount(), 1);
        }

        beginTest ("oversized or deeply nested control lines disconnect the client without crashing");
        {
            // juce::JSON::parse recurses per nesting level: a line of 60 000 '[' (or 4 000, still under the 8 KB
            // line cap) must never reach it.
            Client huge;
            expect (huge.connect());
            std::string line (60000, '[');
            line += '\n';
            huge.sendRaw (line.data(), (int) line.size());
            expect (huge.waitForClose (3000), "60 000-byte line did not disconnect the client");

            Client nested;
            expect (nested.connect());
            std::string deep (4000, '[');
            deep += '\n';
            nested.sendRaw (deep.data(), (int) deep.size());
            expect (nested.waitForClose (3000), "4 000-level line did not disconnect the client");

            // An ordinary 100-level line is fine for the parser but over the depth cap: also dropped.
            Client overCap;
            expect (overCap.connect());
            std::string capped = "{\"t\":\"ping\",\"x\":" + std::string (40, '[') + std::string (40, ']') + "}\n";
            overCap.sendRaw (capped.data(), (int) capped.size());
            expect (overCap.waitForClose (3000), "41-level line did not disconnect the client");

            // A legitimately nested message under the cap still parses.
            Client under;
            expect (under.connect());
            std::string ok = "{\"t\":\"ping\",\"t0\":7,\"x\":" + std::string (20, '[') + std::string (20, ']') + "}\n";
            under.sendRaw (ok.data(), (int) ok.size());
            expect (! under.waitFor ("pong").isVoid(), "21-level line was not answered");

            expectEquals (host.getSessionCount(), 1);
        }

        beginTest ("ping returns pong with the same t0");
        {
            one.send (object ({ { "t", "ping" }, { "t0", 1234.5 } }));
            const auto pong = one.waitFor ("pong");
            expect (! pong.isVoid());
            expectEquals ((double) pong["t0"], 1234.5);
        }

        beginTest ("gain messages are accepted");
        {
            one.send (object ({ { "t", "gain" }, { "target", 2 }, { "voice", -6.0 }, { "instrument", 0.0 }, { "mute", false } }));
            one.send (object ({ { "t", "gain" }, { "target", "track" }, { "voice", -3.0 }, { "instrument", -3.0 }, { "mute", true } }));
            one.send (object ({ { "t", "jitter" }, { "clientBlocks", 4 } }));
            expect (one.waitFor ("error", 300).isVoid(), "gain produced an error");
        }

        beginTest ("UDP audio is mixed and sent back to the observed address");
        {
            juce::DatagramSocket udp (false);
            expect (udp.bindToPort (0, "127.0.0.1"));
            OpusMode mode (FrameSize::f128);
            OpusEncoder voice (mode, 1), instrument (mode, 1);
            std::vector<float> pcm (128);
            uint8_t voiceBytes[64], instrumentBytes[64], wire[maxPacketBytes];

            for (int i = 0; i < 20; ++i)
            {
                for (int s = 0; s < 128; ++s)
                    pcm[(size_t) s] = 0.5f * std::sin (juce::MathConstants<float>::twoPi * 440.0f * (float) (i * 128 + s) / (float) sampleRate);

                AudioPacketView view;
                view.flags = AudioFlags::voice | AudioFlags::instrument;
                view.frameSize = FrameSize::f128;
                view.seq = (uint16_t) i;
                view.clientId = 1;
                view.sampleTime = (uint32_t) (i * 128);
                view.voiceLen = voice.encode (pcm.data(), voiceBytes, sizeof (voiceBytes));
                view.voice = voiceBytes;
                view.instrumentLen = instrument.encode (pcm.data(), instrumentBytes, sizeof (instrumentBytes));
                view.instrument = instrumentBytes;
                const auto len = encodeAudioPacket (view, wire, sizeof (wire));
                expect (len > 0);
                expect (udp.write ("127.0.0.1", udpPort, wire, (int) len) == (int) len);
                juce::Thread::sleep (5);
                host.tick();
            }

            int received = 0, valid = 0;
            uint8_t in[maxPacketBytes];

            while (udp.waitUntilReady (true, 300) > 0)
            {
                const auto n = udp.read (in, sizeof (in), false);

                if (n <= 0)
                    break;

                ++received;
                AudioPacketView view;

                if (decodeAudioPacket (in, (size_t) n, view) && view.flags == AudioFlags::mix
                     && view.voiceLen == codedBytesStereo (FrameSize::f128) && view.frameSize == FrameSize::f128)
                    ++valid;
            }

            logMessage ("mix packets received: " + juce::String (received));
            expect (received >= 10, "received " + juce::String (received));
            expectEquals (valid, received);
        }

        beginTest ("track upload and transport");
        {
            const auto wav = makeClickWav();
            const auto sha1 = Sha1::of (wav.getData(), wav.getSize());
            one.send (object ({ { "t", "track_upload_begin" }, { "name", "click.wav" }, { "size", (juce::int64) wav.getSize() }, { "sha1", sha1 } }));
            one.sendRaw (wav.getData(), (int) wav.getSize());
            const auto ready = one.waitFor ("track_ready", 10000);
            expect (! ready.isVoid(), "no track_ready");
            expectEquals (ready["name"].toString(), juce::String ("click.wav"));
            expect (std::abs ((int) ready["lengthSamples"] - 48000) <= 1, "lengthSamples = " + ready["lengthSamples"].toString());

            // Everyone gets the transport state after the load.
            const auto loadedState = two.waitFor ("transport_state", 3000, [] (const juce::var& m) { return (bool) m["loaded"]; });
            expect (! loadedState.isVoid(), "client 2 did not get transport_state after load");

            two.send (object ({ { "t", "transport" }, { "cmd", "play" } }));
            const auto playing = one.waitFor ("transport_state", 3000, [] (const juce::var& m) { return (bool) m["playing"]; });
            expect (! playing.isVoid(), "no transport_state with playing == true");
            expectEquals (playing["name"].toString(), juce::String ("click.wav"));

            one.send (object ({ { "t", "transport" }, { "cmd", "loop" }, { "loop", true } }));
            expect (! one.waitFor ("transport_state", 3000, [] (const juce::var& m) { return (bool) m["loop"]; }).isVoid());
            one.send (object ({ { "t", "transport" }, { "cmd", "pause" } }));
            expect (! one.waitFor ("transport_state", 3000, [] (const juce::var& m) { return ! (bool) m["playing"]; }).isVoid());
            one.send (object ({ { "t", "transport" }, { "cmd", "seek" }, { "position", 24000 } }));
            expect (! one.waitFor ("transport_state", 3000, [] (const juce::var& m) { return (int) m["position"] == 24000; }).isVoid());
            one.send (object ({ { "t", "transport" }, { "cmd", "stop" } }));
            expect (! one.waitFor ("transport_state", 3000, [] (const juce::var& m) { return (int) m["position"] == 0 && ! (bool) m["playing"]; }).isVoid());

            // A corrupt upload is rejected.
            one.send (object ({ { "t", "track_upload_begin" }, { "name", "bad.wav" }, { "size", 16 }, { "sha1", "00" } }));
            const char junk[16] = {};
            one.sendRaw (junk, 16);
            expect (! one.waitFor ("error", 5000).isVoid(), "corrupt upload not rejected");
            expect (! config.uploadDirectory.getChildFile (sessionId).getChildFile ("bad.wav").existsAsFile());
            expect (config.uploadDirectory.getChildFile (sessionId).getChildFile ("click.wav").existsAsFile());

            // Names that would leave the session folder fall back to "track" (which the decoder then rejects, having
            // no extension); the folder itself and the loaded track are untouched.
            for (const char* bad : { "..", ".", "  " })
            {
                one.send (object ({ { "t", "track_upload_begin" }, { "name", bad }, { "size", (juce::int64) wav.getSize() }, { "sha1", sha1 } }));
                one.sendRaw (wav.getData(), (int) wav.getSize());
                expect (! one.waitFor ("error", 5000).isVoid(), juce::String ("upload named '") + bad + "' not rejected");
            }

            expect (config.uploadDirectory.getChildFile (sessionId).isDirectory());
            expect (config.uploadDirectory.getChildFile (sessionId).getChildFile ("click.wav").existsAsFile());
            expect (! config.uploadDirectory.getChildFile (sessionId).getChildFile ("track").exists());
            expect (one.waitFor ("error", 300).isVoid(), "stray error after the rejected uploads");
        }

        beginTest ("more than 64 TCP connections are refused");
        {
            std::vector<std::unique_ptr<Client>> extra;
            int refused = 0;

            for (int i = 0; i < 70; ++i)
            {
                auto client = std::make_unique<Client>();

                if (client->connect())
                    extra.push_back (std::move (client));
                else
                    ++refused;
            }

            // A refused connection is accepted by the OS and closed by the server right away: its ping is never answered.
            for (auto& client : extra)
            {
                client->send (object ({ { "t", "ping" }, { "t0", 1.0 } }));

                if (client->waitFor ("pong", 1500).isVoid())
                    ++refused;
            }

            logMessage ("refused connections: " + juce::String (refused));
            expect (refused >= 6 && refused <= 12, "refused = " + juce::String (refused));   // 70 - (64 - live ones)

            one.send (object ({ { "t", "ping" }, { "t0", 2.0 } }));
            expect (! one.waitFor ("pong").isVoid(), "joined client lost service under the connection cap");
            // Closing them frees their slots as soon as their threads notice.
            extra.clear();
            const auto freed = juce::Time::getMillisecondCounter();
            bool slotFree = false;

            while (! slotFree && juce::Time::getMillisecondCounter() - freed < 6000)
            {
                Client probe;

                if (probe.connect())
                {
                    probe.send (object ({ { "t", "ping" }, { "t0", 3.0 } }));
                    slotFree = ! probe.waitFor ("pong", 500).isVoid();
                }

                if (! slotFree)
                    juce::Thread::sleep (100);
            }

            logMessage ("slot free after " + juce::String (juce::Time::getMillisecondCounter() - freed) + " ms");
            expect (slotFree, "closing connections did not free a slot within 6 s");
        }

        beginTest ("periodic stats and transport_state");
        {
            expect (! two.waitFor ("stats", 2500).isVoid(), "no stats within 2.5 s");
            expect (! two.waitFor ("transport_state", 2500).isVoid(), "no periodic transport_state");
        }

        beginTest ("a peer that stops reading is dropped within the stall bound without blocking the others");
        {
            Client slow;
            expect (slow.connect());
            slow.send (object ({ { "t", "join" }, { "session", sessionId }, { "name", "Lento" }, { "role", "voice" } }));
            expect (! slow.waitFor ("joined").isVoid(), "slow client did not join");
            expect (! one.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 3; }).isVoid());
            expect (! two.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 3; }).isVoid());

            // Flood pings and never read the pongs: the server's send buffer towards `slow` fills up and every
            // write to it would block. The connection must be dropped after the bounded stall (2 s) and the
            // other participants must keep receiving their broadcasts meanwhile.
            const auto ping = makeLine (object ({ { "t", "ping" }, { "t0", 0.0 } }));
            const auto start = juce::Time::getMillisecondCounter();
            int sent = 0;
            juce::var dropped;

            while (dropped.isVoid() && juce::Time::getMillisecondCounter() - start < 20000)
            {
                for (int i = 0; i < 200; ++i)
                {
                    if (slow.socket.write (ping.toRawUTF8(), (int) ping.getNumBytesAsUTF8()) > 0)
                        ++sent;
                    else
                        juce::Thread::sleep (1);
                }

                dropped = two.waitFor ("participants", 1, [] (const juce::var& m) { return participantCount (m) == 2; });
            }

            const auto elapsed = juce::Time::getMillisecondCounter() - start;
            logMessage ("slow peer dropped after " + juce::String (elapsed) + " ms, " + juce::String (sent) + " pings sent");
            expect (! dropped.isVoid(), "slow peer was not dropped");
            expect (elapsed < 15000, "drop took " + juce::String (elapsed) + " ms");
            expect (! one.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 2; }).isVoid());
            expect (! one.waitFor ("stats", 2500).isVoid(), "client 1 stopped receiving stats");
            expectEquals (host.getSessionCount(), 1);
        }

        beginTest ("closing a connection removes the participant and empty sessions die");
        {
            one.socket.close();
            const auto p = two.waitFor ("participants", 3000, [] (const juce::var& m) { return participantCount (m) == 1; });
            expect (! p.isVoid(), "client 2 did not get participants with 1 entry");

            if (! p.isVoid())
                expectEquals ((int) (*p["participants"].getArray())[0]["id"], 2);

            expectEquals (host.getSessionCount(), 1);
            two.socket.close();

            const auto deadline = juce::Time::getMillisecondCounter() + 3000;

            while (host.getSessionCount() != 0 && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep (20);

            expectEquals (host.getSessionCount(), 0);

            // The session's upload folder goes with it.
            const auto uploads = config.uploadDirectory.getChildFile (sessionId);

            while (uploads.exists() && juce::Time::getMillisecondCounter() < deadline)
                juce::Thread::sleep (20);

            expect (! uploads.exists(), "upload folder survived the session");
        }

        beginTest ("stop and restart");
        {
            host.stop();
            expect (host.start().wasOk());
            Client again;
            expect (again.connect());
            again.send (object ({ { "t", "create" }, { "name", "Dani" }, { "role", "instrument" } }));
            const auto joined = again.waitFor ("joined");
            expect (! joined.isVoid());
            expect ((int) joined["clientId"] > 2, "client ids keep increasing across sessions");
            host.stop();
            expectEquals (host.getSessionCount(), 0);
        }

        {
            const juce::ScopedLock scoped (logLock);
            logMessage ("host log lines: " + juce::String (logLines.size()));
        }
    }
};

static ServerHostTests serverHostTests;
}
