#include "server/ProjectHost.h"
#include "core/Segments.h"
#include "server/ProjectHostDetail.h"
#include "core/Sha1.h"

#include <cmath>
#include <deque>
#include <string>
#include <vector>

namespace ensayo
{
class ProjectHostTests : public juce::UnitTest
{
public:
    ProjectHostTests() : juce::UnitTest ("ProjectHost", "EnsayoVirtual") {}

    // A line-oriented JSON client. On a data connection a "segment" line is followed by its raw bytes, which
    // are attached to the message as "payload" (a MemoryBlock). Unmatched messages stay queued.
    struct Client
    {
        explicit Client (int p, juce::String a = "127.0.0.1") : port (p), address (std::move (a)) {}

        bool connect()   { return socket.connect (address, port, 3000); }

        void send (const juce::var& message)
        {
            const auto line = makeLine (message);
            socket.write (line.toRawUTF8(), (int) line.getNumBytesAsUTF8());
        }

        void sendRaw (const void* data, int len)   { socket.write (data, len); }

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

        bool pump (int timeoutMs)
        {
            const auto ready = socket.waitUntilReady (true, timeoutMs);

            if (ready < 0)
                return false;

            if (ready == 0)
                return true;

            char buffer[16384];
            const auto n = socket.read (buffer, sizeof (buffer), false);

            if (n <= 0)
                return false;

            pending.append (buffer, (size_t) n);

            for (;;)
            {
                if (payloadOwner.isObject())
                {
                    const auto need = (size_t) (int64_t) payloadOwner["bytes"];

                    if (pending.size() < need)
                        break;

                    payloadOwner.getDynamicObject()->setProperty ("payload", juce::var (juce::MemoryBlock (pending.data(), need)));
                    pending.erase (0, need);
                    queue.push_back (payloadOwner);
                    payloadOwner = juce::var();
                    continue;
                }

                const auto newline = pending.find ('\n');

                if (newline == std::string::npos)
                    break;

                const auto line = juce::String::fromUTF8 (pending.data(), (int) newline);
                pending.erase (0, newline + 1);

                if (auto parsed = parseLine (line))
                {
                    if ((*parsed)["t"].toString() == "segment")
                        payloadOwner = *parsed;
                    else
                        queue.push_back (*parsed);
                }
            }

            return true;
        }

        bool waitForClose (int timeoutMs)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            while (juce::Time::getMillisecondCounter() < deadline)
                if (! pump (100))
                    return true;

            return false;
        }

        // Sends hello and waits for welcome. Returns the welcome (void on failure).
        juce::var join (const juce::String& session, const Id& user, const juce::String& userName)
        {
            if (! connect())
                return {};

            send (object ({ { "t", "hello" }, { "protocol", 2 }, { "session", session }, { "user", user }, { "name", userName } }));
            welcome = waitFor ("welcome");
            return welcome;
        }

        // Opens the data connection of a joined control client `control`.
        bool dataHello (const Client& control, const juce::var& have = juce::var (new juce::DynamicObject()))
        {
            if (! connect())
                return false;

            send (object ({ { "t", "data_hello" }, { "clientId", control.welcome["clientId"] },
                            { "token", control.welcome["token"] }, { "have", have } }));
            return true;
        }

        void upload (const Id& take, int index, int samples, int channels, const juce::MemoryBlock& flac,
                     juce::String sha1 = {})
        {
            if (sha1.isEmpty())
                sha1 = Sha1::of (flac.getData(), flac.getSize());

            send (object ({ { "t", "segment" }, { "take", take }, { "index", index }, { "samples", samples },
                            { "channels", channels }, { "bytes", (juce::int64) flac.getSize() }, { "sha1", sha1 } }));
            sendRaw (flac.getData(), (int) flac.getSize());
        }

        int port;
        juce::String address;
        juce::StreamingSocket socket;
        std::string pending;
        juce::var payloadOwner;
        std::deque<juce::var> queue;
        juce::var welcome;
    };

    static juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();

        for (const auto& [key, value] : fields)
            o->setProperty (key, value);

        return juce::var (o);
    }

    static juce::File testDir (const juce::String& name)
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                       .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("projecthost").getChildFile (name);
        dir.deleteRecursively();
        dir.createDirectory();
        return dir;
    }

    static juce::MemoryBlock makeSegment (int samples, int channels, float freq)
    {
        juce::AudioBuffer<float> audio (channels, samples);

        for (int c = 0; c < channels; ++c)
            for (int i = 0; i < samples; ++i)
                audio.setSample (c, i, 0.5f * std::sin (2.0f * juce::MathConstants<float>::pi * freq * (float) i / 48000.0f));

        return encodeSegment (audio, samples);
    }

    static juce::var opMessage (int seq, const juce::var& op)
    {
        return object ({ { "t", "op" }, { "seq", seq }, { "op", op } });
    }

    static const juce::var* findUser (const juce::var& presence, const Id& user)
    {
        if (auto* users = presence["users"].getArray())
            for (auto& u : *users)
                if (u["id"].toString() == user)
                    return &u;

        return nullptr;
    }

    void runTest() override
    {
        testStartAndReopen();
        testHelloErrors();
        testOpsOrderedAndRejected();
        testSegments();
        testCrashSafety();
        testUdpRelay();
        testPresence();
        testDetail();
        testSilentSockets();
        testHostUserFromRemote();
    }

    //==========================================================================
    void testStartAndReopen()
    {
        beginTest ("start creates project.json with the host user; listProjects finds it; reopen keeps the id");

        const auto root = testDir ("start");
        const auto dir = root.getChildFile ("proj");
        const auto hostUser = newId();
        Id projectId;

        {
            ProjectHost host ({ 22200, 8 });
            const auto result = host.start (dir, hostUser, "Lucas", "Mi tema");
            expect (result.wasOk(), result.getErrorMessage());
            expect (dir.getChildFile ("project.json").existsAsFile());
            expect (isValidSessionId (host.getSessionId()));
            expectEquals (host.getPort(), 22200);

            const auto p = host.snapshot();
            expect (p != nullptr);
            expectEquals (p->name, juce::String ("Mi tema"));
            expectEquals (p->hostUser, hostUser);
            expectEquals (p->userName (hostUser), juce::String ("Lucas"));
            projectId = p->id;
            host.stop();
        }

        const auto found = listProjects (root);
        expectEquals (found.size(), 1);
        expect (found.size() == 1 && found[0] == dir);

        juce::String error;
        auto loaded = projectFromJson (dir.getChildFile ("project.json").loadFileAsString(), error);
        expect (loaded.has_value(), error);

        {
            ProjectHost host ({ 22201, 8 });
            const auto result = host.start (dir, hostUser, "Lucas", "ignored");
            expect (result.wasOk(), result.getErrorMessage());
            expectEquals (host.snapshot()->id, projectId);
            expectEquals (host.snapshot()->name, juce::String ("Mi tema"));
        }
    }

    void testHelloErrors()
    {
        beginTest ("hello with protocol 1 or a wrong session is refused");

        ProjectHost host ({ 22202, 8 });
        expect (host.start (testDir ("hello"), newId(), "Lucas", "P").wasOk());

        {
            Client c (22202);
            expect (c.connect());
            c.send (object ({ { "t", "hello" }, { "protocol", 1 }, { "session", host.getSessionId() },
                              { "user", newId() }, { "name", "Ana" } }));
            const auto error = c.waitFor ("error");
            expectEquals (error["message"].toString(), juce::String (juce::CharPointer_UTF8 ("versi\xc3\xb3n incompatible")));
            expect (c.waitForClose (3000));
        }

        {
            Client c (22202);
            expect (c.connect());
            const auto wrong = host.getSessionId() == "AAAAAA" ? juce::String ("BBBBBB") : juce::String ("AAAAAA");
            c.send (object ({ { "t", "hello" }, { "protocol", 2 }, { "session", wrong },
                              { "user", newId() }, { "name", "Ana" } }));
            const auto error = c.waitFor ("error");
            expectEquals (error["message"].toString(), juce::String (juce::CharPointer_UTF8 ("sesi\xc3\xb3n no encontrada")));
            expect (c.waitForClose (3000));
        }
    }

    void testOpsOrderedAndRejected()
    {
        beginTest ("ops are applied in order for everyone; a forbidden op is rejected to its sender only");

        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22204, 8 });
        expect (host.start (testDir ("ops"), hostUser, "Lucas", "P").wasOk());

        Client a (22204), b (22204);
        const auto wa = a.join (host.getSessionId(), hostUser, "Lucas");
        expect (wa.isObject());
        expect ((bool) wa["isHost"]);
        expectEquals (wa["token"].toString().length(), 16);
        expect ((int) wa["clientId"] > 0);
        expectEquals ((int) wa["udpPort"], 22204);

        const auto wb = b.join (host.getSessionId(), userB, "Ana");
        expect (wb.isObject());
        expect (! (bool) wb["isHost"]);
        // B's own registration (add_user) is already in its welcome snapshot.
        const auto base = (int64_t) wb["project"]["version"];
        expectEquals (base, (int64_t) 1);
        a.waitFor ("applied", 3000, [&] (const juce::var& m) { return m["op"]["op"].toString() == "add_user"; });

        const auto track = newId();
        a.send (opMessage (1, Ops::addTrack (track, "Voz", TrackKind::voice)));
        const auto appliedA = a.waitFor ("applied", 3000, [] (const juce::var& m) { return m["op"]["op"].toString() == "add_track"; });
        const auto appliedB = b.waitFor ("applied", 3000, [] (const juce::var& m) { return m["op"]["op"].toString() == "add_track"; });
        expectEquals ((int64_t) appliedA["version"], base + 1);
        expectEquals ((int64_t) appliedB["version"], base + 1);
        expectEquals (appliedB["by"].toString(), hostUser);
        expectEquals (appliedB["op"]["track"].toString(), track);

        b.send (opMessage (7, Ops::renameTrack (track, "Mia")));
        const auto rejected = b.waitFor ("rejected");
        expectEquals ((int) rejected["seq"], 7);
        expect (rejected["reason"].toString().isNotEmpty());
        expect (a.waitFor ("rejected", 300).isVoid());
        expect (a.waitFor ("applied", 300, [] (const juce::var& m) { return m["op"]["op"].toString() == "rename_track"; }).isVoid());
        expectEquals (host.snapshot()->findTrack (track)->name, juce::String ("Voz"));

        // Host-internal ops are refused from clients.
        b.send (opMessage (8, Ops::addUser (newId(), "Intruso")));
        expectEquals ((int) b.waitFor ("rejected")["seq"], 8);

        // ping / resync
        a.send (object ({ { "t", "ping" }, { "ms", 1234 } }));
        expectEquals ((int) a.waitFor ("pong")["ms"], 1234);
        a.send (object ({ { "t", "resync" } }));
        expectEquals ((int64_t) a.waitFor ("project")["project"]["version"], base + 1);
    }

    void testSegments()
    {
        const auto hostUser = newId(), userB = newId(), userC = newId();
        const auto dir = testDir ("segments");
        ProjectHost host ({ 22206, 8 });
        expect (host.start (dir, hostUser, "Lucas", "P").wasOk());

        Client a (22206), b (22206), aData (22206), bData (22206);
        expect (a.join (host.getSessionId(), hostUser, "Lucas").isObject());
        expect (b.join (host.getSessionId(), userB, "Ana").isObject());
        expect (bData.dataHello (b));

        const auto take = newId();
        a.send (opMessage (1, Ops::addTake (take, 1)));
        expect (a.waitFor ("applied", 3000, [] (const juce::var& m) { return m["op"]["op"].toString() == "add_take"; }).isObject());
        expect (aData.dataHello (a));

        beginTest ("uploaded segments are acknowledged, stored, applied and pushed live to the others");

        std::vector<juce::MemoryBlock> flacs { makeSegment (48000, 1, 220.0f), makeSegment (48000, 1, 330.0f),
                                               makeSegment (12000, 1, 440.0f) };
        const int samples[] { 48000, 48000, 12000 };

        for (int i = 0; i < 3; ++i)
            aData.upload (take, i, samples[i], 1, flacs[(size_t) i]);

        for (int i = 0; i < 3; ++i)
        {
            const auto ok = aData.waitFor ("segment_ok", 5000, [&] (const juce::var& m) { return (int) m["index"] == i; });
            expectEquals (ok["take"].toString(), take);
        }

        for (int i = 0; i < 3; ++i)
        {
            const auto seg = bData.waitFor ("segment", 5000);
            expectEquals (seg["take"].toString(), take);
            expectEquals ((int) seg["index"], i);
            expectEquals ((int) seg["samples"], samples[i]);
            expectEquals ((int) seg["channels"], 1);
            expectEquals (seg["sha1"].toString(), Sha1::of (flacs[(size_t) i].getData(), flacs[(size_t) i].getSize()));
            const auto* payload = seg["payload"].getBinaryData();
            expect (payload != nullptr && *payload == flacs[(size_t) i]);
        }

        int64_t lastVersion = 0;

        for (int i = 0; i < 3; ++i)
        {
            const auto applied = b.waitFor ("applied", 3000, [] (const juce::var& m) { return m["op"]["op"].toString() == "update_take"; });
            expectEquals ((int) applied["op"]["segments"], i + 1);
            expectEquals (applied["by"].toString(), hostUser);
            expect ((int64_t) applied["version"] > lastVersion);
            lastVersion = (int64_t) applied["version"];
        }

        for (int i = 0; i < 3; ++i)
            expect (dir.getChildFile ("takes").getChildFile (take).getChildFile (juce::String (i) + ".flac").existsAsFile());

        expectEquals (host.snapshot()->findTake (take)->segments, 3);
        expectEquals (host.snapshot()->findTake (take)->samples, (int64_t) 108000);
        expect (aData.waitFor ("segment", 300).isVoid());   // never echoed to the uploader

        beginTest ("out-of-order, corrupt and foreign uploads get segment_bad");

        aData.upload (take, 5, 48000, 1, flacs[0]);
        auto bad = aData.waitFor ("segment_bad", 3000);
        expectEquals ((int) bad["index"], 5);
        expectEquals ((int) bad["expected"], 3);
        expect (bad["reason"].toString().isNotEmpty());

        aData.upload (take, 3, 48000, 1, flacs[0], juce::String::repeatedString ("0", 40));
        bad = aData.waitFor ("segment_bad", 3000);
        expectEquals ((int) bad["index"], 3);
        expectEquals ((int) bad["expected"], 3);

        bData.upload (take, 3, 48000, 1, flacs[0]);
        bad = bData.waitFor ("segment_bad", 3000);
        expectEquals ((int) bad["index"], 3);
        expectEquals (bad["take"].toString(), take);

        expectEquals (host.snapshot()->findTake (take)->segments, 3);
        expect (! dir.getChildFile ("takes").getChildFile (take).getChildFile ("3.flac").existsAsFile());
        expect (bData.waitFor ("segment", 300).isVoid());

        beginTest ("a late joiner receives the stored segments on its data connection");

        Client c (22206), cData (22206);
        expect (c.join (host.getSessionId(), userC, "Caro").isObject());
        expect (cData.dataHello (c));

        for (int i = 0; i < 3; ++i)
        {
            const auto seg = cData.waitFor ("segment", 5000);
            expectEquals ((int) seg["index"], i);
            expectEquals ((int) seg["samples"], samples[i]);
            const auto* payload = seg["payload"].getBinaryData();
            expect (payload != nullptr && *payload == flacs[(size_t) i]);
        }

        Client dData (22206);
        auto* have = new juce::DynamicObject();
        have->setProperty (take, 2);
        expect (dData.dataHello (c, juce::var (have)));
        const auto onlyLast = dData.waitFor ("segment", 5000);
        expectEquals ((int) onlyLast["index"], 2);
        expect (dData.waitFor ("segment", 300).isVoid());

        beginTest ("take_complete applies update_take complete:true");

        a.send (object ({ { "t", "take_complete" }, { "take", take } }));
        const auto done = b.waitFor ("applied", 3000, [] (const juce::var& m)
                                     { return m["op"]["op"].toString() == "update_take" && (bool) m["op"]["complete"]; });
        expect (done.isObject());
        expectEquals ((int) done["op"]["segments"], 3);
        expectEquals ((int64_t) done["op"]["samples"], (int64_t) 108000);
        expect (host.snapshot()->findTake (take)->complete);
    }

    void testCrashSafety()
    {
        beginTest ("after 20 ops, stop and reopen: version 20 and the same content hash");

        const auto hostUser = newId();
        const auto dir = testDir ("crash");
        juce::String hash;

        {
            ProjectHost host ({ 22208, 8 });
            expect (host.start (dir, hostUser, "Lucas", "P").wasOk());
            Client a (22208);
            expect (a.join (host.getSessionId(), hostUser, "Lucas").isObject());

            const auto track = newId();
            a.send (opMessage (1, Ops::addTrack (track, "Pista", TrackKind::instrument)));

            for (int i = 2; i <= 20; ++i)
                a.send (opMessage (i, Ops::renameTrack (track, "Pista " + juce::String (i))));

            for (int i = 1; i <= 20; ++i)
                expect (a.waitFor ("applied", 3000).isObject());

            expectEquals (host.snapshot()->version, (int64_t) 20);
            hash = contentHash (*host.snapshot());
            host.stop();
        }

        ProjectHost reopened ({ 22209, 8 });
        expect (reopened.start (dir, hostUser, "Lucas", "P").wasOk());
        expectEquals (reopened.snapshot()->version, (int64_t) 20);
        expectEquals (contentHash (*reopened.snapshot()), hash);
    }

    void testUdpRelay()
    {
        beginTest ("preview datagrams are relayed unchanged to listening clients only");

        const auto hostUser = newId();
        ProjectHost host ({ 22210, 8 });
        expect (host.start (testDir ("udp"), hostUser, "Lucas", "P").wasOk());

        Client a (22210), b (22210), c (22210);
        expect (a.join (host.getSessionId(), hostUser, "Lucas").isObject());
        expect (b.join (host.getSessionId(), newId(), "Ana").isObject());
        expect (c.join (host.getSessionId(), newId(), "Caro").isObject());

        juce::DatagramSocket ua, ub, uc;
        expect (ua.bindToPort (0) && ub.bindToPort (0) && uc.bindToPort (0));

        auto keepalive = [] (juce::DatagramSocket& s, const juce::var& welcome)
        {
            const auto id = (uint32_t) (int) welcome["clientId"];
            const uint8_t bytes[5] { 0x00, (uint8_t) id, (uint8_t) (id >> 8), (uint8_t) (id >> 16), (uint8_t) (id >> 24) };
            s.write ("127.0.0.1", 22210, bytes, 5);
        };

        b.send (object ({ { "t", "listen" }, { "enabled", true } }));
        keepalive (ub, b.welcome);
        keepalive (uc, c.welcome);

        uint8_t payload[48];

        for (int i = 0; i < 48; ++i)
            payload[i] = (uint8_t) (i * 7);

        AudioPacketView view;
        view.flags = AudioFlags::voice;
        view.frameSize = FrameSize::f128;
        view.seq = 42;
        view.clientId = (uint32_t) (int) a.welcome["clientId"];
        view.sampleTime = 1000;
        view.trackPosition = 192000;
        view.voice = payload;
        view.voiceLen = 48;
        uint8_t packet[maxPacketBytes];
        const auto len = encodeAudioPacket (view, packet, sizeof (packet));
        expect (len > 0);

        bool received = false;
        uint8_t in[2048];

        for (int attempt = 0; attempt < 20 && ! received; ++attempt)
        {
            keepalive (ub, b.welcome);
            ua.write ("127.0.0.1", 22210, packet, (int) len);

            if (ub.waitUntilReady (true, 100) == 1)
            {
                const auto n = ub.read (in, sizeof (in), false);
                received = n == (int) len && std::memcmp (in, packet, len) == 0;
            }
        }

        expect (received);
        expect (uc.waitUntilReady (true, 300) != 1);
        expect (ua.waitUntilReady (true, 100) != 1);
    }

    void testPresence()
    {
        beginTest ("presence lists both users online, then B offline after it disconnects");

        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22212, 8 });
        expect (host.start (testDir ("presence"), hostUser, "Lucas", "P").wasOk());

        Client a (22212);
        expect (a.join (host.getSessionId(), hostUser, "Lucas").isObject());

        {
            Client b (22212);
            expect (b.join (host.getSessionId(), userB, "Ana").isObject());

            const auto both = a.waitFor ("presence", 3000, [&] (const juce::var& m)
            {
                const auto* ua = findUser (m, hostUser);
                const auto* ub = findUser (m, userB);
                return ua != nullptr && ub != nullptr && (bool) (*ua)["online"] && (bool) (*ub)["online"];
            });
            expect (both.isObject());
            expectEquals ((*findUser (both, userB))["name"].toString(), juce::String ("Ana"));

            // Recording state shows up in presence and rec_* messages are relayed with "user".
            const auto track = newId(), take = newId();
            b.send (opMessage (1, Ops::addTrack (track, "Voz", TrackKind::voice)));
            b.send (opMessage (2, Ops::addTake (take, 1)));
            expect (b.waitFor ("applied", 3000, [] (const juce::var& m) { return m["op"]["op"].toString() == "add_take"; }).isObject());
            b.send (object ({ { "t", "rec_start" }, { "take", take }, { "track", track }, { "start", 192000 } }));
            const auto relayed = a.waitFor ("rec_start");
            expectEquals (relayed["user"].toString(), userB);
            expectEquals ((int64_t) relayed["start"], (int64_t) 192000);

            const auto recording = a.waitFor ("presence", 3000, [&] (const juce::var& m)
            {
                const auto* ub = findUser (m, userB);
                return ub != nullptr && (*ub)["recording"].isObject();
            });
            expect (recording.isObject());
            expectEquals ((*findUser (recording, userB))["recording"]["take"].toString(), take);

            b.send (object ({ { "t", "rec_stop" }, { "take", take } }));
            expectEquals (a.waitFor ("rec_stop")["user"].toString(), userB);
            expect (b.waitFor ("rec_stop", 300).isVoid());   // not echoed to the sender
        }

        const auto after = a.waitFor ("presence", 3000, [&] (const juce::var& m)
        {
            const auto* ub = findUser (m, userB);
            return ub != nullptr && ! (bool) (*ub)["online"];
        });
        expect (after.isObject());
        expect ((bool) (*findUser (after, hostUser))["online"]);
    }

    //==========================================================================
    void testDetail()
    {
        beginTest ("only a loopback peer may claim the host user");

        const auto hostUser = newId(), other = newId();
        expect (hostdetail::mayClaimUser (hostUser, hostUser, true));
        expect (! hostdetail::mayClaimUser (hostUser, hostUser, false));
        expect (hostdetail::mayClaimUser (other, hostUser, false));
        expect (hostdetail::mayClaimUser (other, hostUser, true));

        beginTest ("readInteger accepts integral values in range only");

        expect (hostdetail::readInteger (juce::var (5), 0, 10) == std::optional<int64_t> (5));
        expect (hostdetail::readInteger (juce::var ((juce::int64) 1 << 40), 0, (int64_t) 1 << 41) == std::optional<int64_t> ((int64_t) 1 << 40));
        expect (hostdetail::readInteger (juce::var (7.0), 0, 10) == std::optional<int64_t> (7));
        expect (! hostdetail::readInteger (juce::var (7.5), 0, 10));
        expect (! hostdetail::readInteger (juce::var (1.0e300), 0, INT_MAX));
        expect (! hostdetail::readInteger (juce::var (4294967296.0), 0, INT_MAX));
        expect (! hostdetail::readInteger (juce::var (-1), 0, 10));
        expect (! hostdetail::readInteger (juce::var (11), 0, 10));
        expect (! hostdetail::readInteger (juce::var ("5"), 0, 10));
        expect (! hostdetail::readInteger (juce::var(), 0, 10));

        beginTest ("the write stall timeout restarts on progress; a real stall still fails");

        // A slow but steady peer: 64 KB every 50 ms, 2 MB in total (~1.6 s), with a 200 ms stall timeout.
        juce::MemoryBlock source (2 * 1024 * 1024);

        for (size_t i = 0; i < source.getSize(); ++i)
            source[i] = (char) (i * 31 + 7);

        juce::MemoryOutputStream received;
        auto lastAccept = juce::Time::getMillisecondCounterHiRes();

        auto slowPeer = [&] (const char* data, size_t len) -> int
        {
            const auto now = juce::Time::getMillisecondCounterHiRes();

            if (now - lastAccept < 50.0)
                return 0;

            lastAccept = now;
            const auto n = std::min<size_t> (len, 64 * 1024);
            received.write (data, n);
            return (int) n;
        };

        const auto start = juce::Time::getMillisecondCounterHiRes();
        expect (hostdetail::writeWithStallTimeout (slowPeer, static_cast<const char*> (source.getData()), source.getSize(), 200));
        expect (juce::Time::getMillisecondCounterHiRes() - start > 1000.0);   // far longer than the stall timeout
        expect (received.getMemoryBlock() == source);

        int accepted = 0;
        auto stalledPeer = [&] (const char*, size_t len) -> int
        {
            if (accepted > 0)
                return 0;

            accepted = (int) std::min<size_t> (len, 1000);
            return accepted;
        };

        const auto stallStart = juce::Time::getMillisecondCounterHiRes();
        expect (! hostdetail::writeWithStallTimeout (stalledPeer, static_cast<const char*> (source.getData()), 5000, 200));
        const auto stalledFor = juce::Time::getMillisecondCounterHiRes() - stallStart;
        expect (stalledFor >= 190.0 && stalledFor < 1500.0, juce::String (stalledFor));

        auto failingPeer = [] (const char*, size_t) -> int { return -1; };
        expect (! hostdetail::writeWithStallTimeout (failingPeer, static_cast<const char*> (source.getData()), 10, 200));
    }

    static bool closedWithin (juce::StreamingSocket& s, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            const auto ready = s.waitUntilReady (true, 100);

            if (ready < 0)
                return true;

            if (ready == 1)
            {
                char byte;

                if (s.read (&byte, 1, false) <= 0)
                    return true;
            }
        }

        return false;
    }

    void testSilentSockets()
    {
        beginTest ("silent sockets cannot lock real joiners out and are closed after the first-line deadline");

        const auto hostUser = newId();
        ProjectHost host ({ 22214, 8 });
        expect (host.start (testDir ("silent"), hostUser, "Lucas", "P").wasOk());

        std::vector<std::unique_ptr<juce::StreamingSocket>> silent;

        for (int i = 0; i < 20; ++i)
        {
            silent.push_back (std::make_unique<juce::StreamingSocket>());
            expect (silent.back()->connect ("127.0.0.1", 22214, 3000));
        }

        const auto lastConnected = juce::Time::getMillisecondCounterHiRes();
        juce::Thread::sleep (300);

        Client a (22214);
        const auto welcome = a.join (host.getSessionId(), hostUser, "Lucas");
        expect (welcome.isObject());
        expect (closedWithin (*silent.front(), 1000));   // the oldest silent sockets made room

        expect (closedWithin (*silent.back(), 8000));
        const auto lifetime = juce::Time::getMillisecondCounterHiRes() - lastConnected;
        expect (lifetime > 4000.0 && lifetime < 7500.0, juce::String (lifetime));

        // The identified client is not subject to the deadline.
        a.send (object ({ { "t", "ping" }, { "ms", 99 } }));
        expectEquals ((int) a.waitFor ("pong")["ms"], 99);
    }

    void testHostUserFromRemote()
    {
        beginTest ("the host user id is refused from a non-loopback address");

        juce::StringArray candidates;

        for (const auto& ip : juce::IPAddress::getAllAddresses (false))
        {
            const auto text = ip.toString();

            if (! ip.isNull() && ! text.startsWith ("127.") && ! text.startsWith ("169.254.") && text != "0.0.0.0")
                candidates.addIfNotAlreadyThere (text);
        }

        const auto hostUser = newId();
        ProjectHost host ({ 22216, 8 });
        expect (host.start (testDir ("remotehost"), hostUser, "Lucas", "P").wasOk());

        Client local (22216);
        expect ((bool) local.join (host.getSessionId(), hostUser, "Lucas")["isHost"]);   // loopback host path

        if (candidates.isEmpty())
        {
            logMessage ("no non-loopback IPv4 address on this machine: only mayClaimUser covers the remote case");
            return;
        }

        // The listener binds every interface: the first local address that connects is the non-loopback source.
        juce::String remote;
        std::unique_ptr<Client> claimHolder;

        for (const auto& candidate : candidates)
        {
            auto attempt = std::make_unique<Client> (22216, candidate);

            if (attempt->connect())
            {
                remote = candidate;
                claimHolder = std::move (attempt);
                break;
            }
        }

        if (claimHolder == nullptr)
        {
            expect (false, "no local non-loopback address accepted a connection; tried: " + candidates.joinIntoString (", "));
            return;
        }

        logMessage ("claiming the host id from " + remote);
        auto& claim = *claimHolder;

        claim.send (object ({ { "t", "hello" }, { "protocol", 2 }, { "session", host.getSessionId() },
                              { "user", hostUser }, { "name", "Impostor" } }));
        expectEquals (claim.waitFor ("error")["message"].toString(), juce::String ("usuario reservado"));
        expect (claim.waitForClose (3000));

        // The real host was not kicked, and an ordinary user may join from that address.
        local.send (object ({ { "t", "ping" }, { "ms", 5 } }));
        expectEquals ((int) local.waitFor ("pong")["ms"], 5);

        Client guest (22216, remote);
        expect (guest.join (host.getSessionId(), newId(), "Ana").isObject());
    }
};

static ProjectHostTests projectHostTests;
}
