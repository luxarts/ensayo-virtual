#include "client/ProjectClient.h"
#include "server/ProjectHost.h"
#include "core/Segments.h"
#include "MessagePump.h"

#include <atomic>
#include <cmath>
#include <string>
#include <thread>

namespace ensayo
{
class ProjectClientTests : public juce::UnitTest
{
public:
    ProjectClientTests() : juce::UnitTest ("ProjectClient", "EnsayoVirtual") {}

    //==========================================================================
    // One client plus everything its callbacks reported.
    struct Peer
    {
        Peer (const juce::File& root, const Id& id, const juce::String& name)
            : client (std::make_unique<ProjectClient> (ClientConfig { root, id, name }))
        {
            client->onConnected = [this] { connected = true; };
            client->onError = [this] (const juce::String& m) { errors.add (m); };
            client->onRejected = [this] (const juce::String& m) { rejected.add (m); };
            client->onDisconnected = [this] (const juce::String& m) { disconnected.add (m); connected = false; };
        }

        ProjectClient* operator->() { return client.get(); }

        bool connectTo (int port, const juce::String& session)
        {
            connected = false;
            client->connect (SessionLink { "127.0.0.1", port, session });
            return waitUntil ([this] { return connected; }, 8000);
        }

        std::unique_ptr<ProjectClient> client;
        bool connected = false;
        juce::StringArray errors, rejected, disconnected;
    };

    template <typename Predicate>
    static bool waitUntil (Predicate predicate, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (! predicate())
        {
            if (juce::Time::getMillisecondCounter() >= deadline)
                return false;

            pumpMessages (10);
        }

        return true;
    }

    static juce::File testRoot()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory)
                   .getChildFile ("EnsayoVirtual").getChildFile ("tests").getChildFile ("projectclient");
    }

    static juce::File testDir (const juce::String& name)
    {
        auto dir = testRoot().getChildFile (name);
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

    static bool hasRegion (const Project& p, const Id& track, const Id& region)
    {
        if (const auto* t = p.findTrack (track))
            for (const auto& r : t->regions)
                if (r.id == region)
                    return true;

        return false;
    }

    static int countChildren (const juce::File& dir)
    {
        return dir.isDirectory() ? dir.getNumberOfChildFiles (juce::File::findFilesAndDirectories) : 0;
    }

    static juce::File pendingDir (const juce::File& cacheRoot, const Project& p)
    {
        return cacheRoot.getChildFile (p.id).getChildFile ("pending");
    }

    // Track `track` owned by `peer`, applied and visible in its replica.
    bool addOwnTrack (Peer& peer, const Id& track, const juce::String& trackName)
    {
        peer->sendOp (Ops::addTrack (track, trackName, TrackKind::voice));
        return waitUntil ([&] { return peer->project() != nullptr && peer->project()->findTrack (track) != nullptr; }, 5000);
    }

    void runTest() override
    {
        testRoot().deleteRecursively();

        testConnectAndOps();
        testRecording();
        testLive();
        testResumeAfterDisconnect();
        testRestartFinishesTake();
        testCacheReuse();
        testVersionGap();
        testPreview();
        testSilentHost();
    }

    //==========================================================================
    void testConnectAndOps()
    {
        beginTest ("connect: replicas equal the host; ops reach both; a rejected op changes nothing");

        const auto root = testDir ("ops");
        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22220, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        Peer a (root.getChildFile ("cacheA"), hostUser, "Lucas");
        Peer b (root.getChildFile ("cacheB"), userB, "Ana");
        expect (a.connectTo (22220, host.getSessionId()), "A connects");
        expect (b.connectTo (22220, host.getSessionId()), "B connects");
        expect (a->isConnected() && b->isConnected());
        expect (a->isHost() && ! b->isHost());
        expect (a->user() == hostUser);
        expect (a->cache() != nullptr && b->cache() != nullptr);

        // B's join is a versioned add_user that A receives as `applied`.
        expect (waitUntil ([&] { return a->project()->version == host.snapshot()->version; }, 3000));
        expectEquals (contentHash (*a->project()), contentHash (*host.snapshot()));
        expectEquals (contentHash (*b->project()), contentHash (*host.snapshot()));

        const auto before = host.snapshot()->version;
        const auto track = newId();
        a->sendOp (Ops::addTrack (track, "Guitarra", TrackKind::instrument));

        expect (waitUntil ([&] { return a->project()->version == before + 1 && b->project()->version == before + 1; }, 3000));
        expectEquals (contentHash (*a->project()), contentHash (*host.snapshot()));
        expectEquals (contentHash (*b->project()), contentHash (*host.snapshot()));

        b->sendOp (Ops::renameTrack (track, "Robada"));
        expect (waitUntil ([&] { return b.rejected.size() == 1; }, 3000), "B gets onRejected");
        pumpMessages (200);
        expect (a.rejected.isEmpty());
        expectEquals ((int) a->project()->version, (int) (before + 1));
        expectEquals ((int) b->project()->version, (int) (before + 1));
        expectEquals (a->project()->findTrack (track)->name, juce::String ("Guitarra"));

        expect (waitUntil ([&] { return a->pingMs() >= 0; }, 3000), "ping measured");

        // Presence lists both users online.
        expect (waitUntil ([&]
        {
            int online = 0;

            for (const auto& p : a->presence())
                if (p.online)
                    ++online;

            return online == 2;
        }, 3000));

        a->disconnect();
        b->disconnect();
        expect (! a->isConnected() && ! b->isConnected());
        host.stop();
    }

    //==========================================================================
    void testRecording()
    {
        beginTest ("recording: B's segments reach A's cache; take complete and region in A's replica");

        const auto root = testDir ("record");
        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22222, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        Peer a (root.getChildFile ("cacheA"), hostUser, "Lucas");
        Peer b (root.getChildFile ("cacheB"), userB, "Ana");
        expect (a.connectTo (22222, host.getSessionId()));
        expect (b.connectTo (22222, host.getSessionId()));

        const auto track = newId(), take = newId();
        expect (addOwnTrack (b, track, "Voz"));

        // Segments are queued before add_take has come back: they must wait for it and then go in order.
        b->beginTake (take, 1, track, 0);
        b->queueSegment (take, 0, makeSegment (48000, 1, 220.0f));
        b->queueSegment (take, 1, makeSegment (48000, 1, 330.0f));
        b->queueSegment (take, 2, makeSegment (12000, 1, 440.0f));
        expectEquals (b->cache()->segmentCount (take), 3);   // local cache right away

        Region region;
        region.id = newId();
        region.take = take;
        region.length = 108000;
        b->finishTake (take, track, region);

        expect (waitUntil ([&]
        {
            const auto p = a->project();
            const auto* t = p->findTake (take);
            return t != nullptr && t->complete && hasRegion (*p, track, region.id) && a->cache()->segmentCount (take) == 3;
        }, 10000), "A has the complete take, the region and 3 cached segments");

        const auto* t = a->project()->findTake (take);
        expect (t != nullptr && t->segments == 3 && t->samples == 108000);
        expectEquals ((int) a->cache()->sampleCount (take), 108000);
        expect (a->cache()->segmentBytes (take, 1) == b->cache()->segmentBytes (take, 1));

        expect (waitUntil ([&] { return countChildren (pendingDir (root.getChildFile ("cacheB"), *b->project())) == 0
                                        && b->transfers().pendingUploads == 0
                                        && contentHash (*b->project()) == contentHash (*host.snapshot()); }, 5000),
                "B's pending dir is empty and its replica equals the host");
        expectEquals (a->transfers().downloadingTakes, 0);
        expect (b.errors.isEmpty() && b.rejected.isEmpty(), b.errors.joinIntoString ("; ") + b.rejected.joinIntoString ("; "));

        a->disconnect();
        b->disconnect();
        host.stop();
    }

    //==========================================================================
    void testLive()
    {
        beginTest ("live: A sees B's recording and peaks; it disappears after rec_stop");

        const auto root = testDir ("live");
        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22223, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        Peer a (root.getChildFile ("cacheA"), hostUser, "Lucas");
        Peer b (root.getChildFile ("cacheB"), userB, "Ana");
        expect (a.connectTo (22223, host.getSessionId()));
        expect (b.connectTo (22223, host.getSessionId()));

        const auto track = newId(), take = newId();
        expect (addOwnTrack (b, track, "Voz"));

        b->beginTake (take, 1, track, 96000);

        std::vector<Peak> peaks (10);

        for (size_t i = 0; i < peaks.size(); ++i)
            peaks[i] = { -0.01f * (float) i, 0.02f * (float) i };

        b->sendPeaks (take, 0, peaks);

        expect (waitUntil ([&]
        {
            const auto live = a->liveRecordings();
            return live.size() == 1 && live[0].peaks.size() == 10;
        }, 5000), "A sees B's live recording");

        auto live = a->liveRecordings();
        expect (live[0].user == userB && live[0].take == take && live[0].track == track && live[0].start == 96000);
        expectWithinAbsoluteError (live[0].peaks[5].max, 0.1f, 0.001f);

        // More peaks than fit in one host line (8 KB): they must be split.
        std::vector<Peak> many (1000, Peak { -0.123456f, 0.654321f });
        b->sendPeaks (take, 10, many);
        expect (waitUntil ([&] { return ! a->liveRecordings().empty() && a->liveRecordings()[0].peaks.size() == 1010; }, 5000),
                "1010 peaks arrive");

        b->queueSegment (take, 0, makeSegment (24000, 1, 220.0f));
        Region region;
        region.id = newId();
        region.take = take;
        region.start = 96000;
        region.length = 24000;
        b->finishTake (take, track, region);

        expect (waitUntil ([&] { return a->liveRecordings().empty() && hasRegion (*a->project(), track, region.id); }, 8000),
                "the live recording ends after rec_stop");

        a->disconnect();
        b->disconnect();
        host.stop();
    }

    //==========================================================================
    void testResumeAfterDisconnect()
    {
        beginTest ("uploads resume after a disconnect from the host's segment count, without duplicates");

        const auto root = testDir ("resume");
        const auto hostUser = newId(), userB = newId();
        const auto hostDir = root.getChildFile ("host");
        const auto cacheB = root.getChildFile ("cacheB");
        ProjectHost host ({ 22224, 8 });
        expect (host.start (hostDir, hostUser, "Lucas", "Tema").wasOk());

        Peer b (cacheB, userB, "Ana");
        expect (b.connectTo (22224, host.getSessionId()));

        const auto track = newId(), take = newId();
        expect (addOwnTrack (b, track, "Voz"));

        b->beginTake (take, 2, track, 0);
        b->queueSegment (take, 0, makeSegment (48000, 2, 220.0f));
        b->queueSegment (take, 1, makeSegment (48000, 2, 330.0f));

        // Both acknowledged (segment_ok received), so neither is pending any more.
        expect (waitUntil ([&] { return b->transfers().pendingUploads == 0; }, 5000));
        expectEquals (host.snapshot()->findTake (take)->segments, 2);
        b->disconnect();

        // Recording goes on while offline.
        b->queueSegment (take, 2, makeSegment (48000, 2, 440.0f));
        b->queueSegment (take, 3, makeSegment (6000, 2, 550.0f));

        Region region;
        region.id = newId();
        region.take = take;
        region.length = 150000;
        b->finishTake (take, track, region);

        const auto pending = pendingDir (cacheB, *b->project()).getChildFile (take);
        expect (pending.getChildFile ("2.flac").existsAsFile() && pending.getChildFile ("3.flac").existsAsFile());
        expect (! pending.getChildFile ("0.flac").exists(), "acknowledged segments are not kept as pending");
        expectEquals (b->transfers().pendingUploads, 2);

        expect (b.connectTo (22224, host.getSessionId()), "B reconnects");

        expect (waitUntil ([&]
        {
            const auto p = host.snapshot();
            const auto* t = p->findTake (take);
            return t != nullptr && t->complete && hasRegion (*p, track, region.id);
        }, 10000), "the host completes the take and gets the region");

        const auto* t = host.snapshot()->findTake (take);
        expect (t != nullptr && t->segments == 4 && t->samples == 150000);
        expectEquals (hostDir.getChildFile ("takes").getChildFile (take).getNumberOfChildFiles (juce::File::findFiles, "*.flac"), 4);
        expect (waitUntil ([&] { return countChildren (pendingDir (cacheB, *b->project())) == 0; }, 5000), "pending dir empty");
        expect (b.errors.isEmpty(), b.errors.joinIntoString ("; "));

        b->disconnect();
        host.stop();
    }

    //==========================================================================
    void testRestartFinishesTake()
    {
        beginTest ("a new client instance finishes a take recorded offline by the previous one");

        const auto root = testDir ("restart");
        const auto hostUser = newId(), userB = newId();
        const auto cacheB = root.getChildFile ("cacheB");
        ProjectHost host ({ 22225, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        const auto track = newId(), take = newId();
        Region region;
        region.id = newId();
        region.take = take;
        region.offset = 1000;
        region.length = 60000;

        {
            Peer b (cacheB, userB, "Ana");
            expect (b.connectTo (22225, host.getSessionId()));
            expect (addOwnTrack (b, track, "Voz"));
            b->disconnect();

            // Everything below happens offline: add_take is not on the host yet.
            b->beginTake (take, 1, track, 0);
            b->queueSegment (take, 0, makeSegment (48000, 1, 220.0f));
            b->queueSegment (take, 1, makeSegment (48000, 1, 330.0f));
            b->queueSegment (take, 2, makeSegment (100, 1, 440.0f));
            b->finishTake (take, track, region);
            expectEquals (b->transfers().pendingUploads, 3);
            expect (host.snapshot()->findTake (take) == nullptr);
        }

        Peer b2 (cacheB, userB, "Ana");
        expectEquals (b2->transfers().pendingUploads, 0);
        expect (b2.connectTo (22225, host.getSessionId()));

        expect (waitUntil ([&]
        {
            const auto p = host.snapshot();
            const auto* t = p->findTake (take);
            return t != nullptr && t->complete && hasRegion (*p, track, region.id);
        }, 10000), "the restarted client uploads, completes and adds the region");

        const auto* t = host.snapshot()->findTake (take);
        expect (t != nullptr && t->segments == 3 && t->samples == 96100 && t->author == userB);
        expect (waitUntil ([&] { return countChildren (pendingDir (cacheB, *b2->project())) == 0; }, 5000), "pending dir empty");
        expect (b2.errors.isEmpty(), b2.errors.joinIntoString ("; "));

        b2->disconnect();
        host.stop();
    }

    //==========================================================================
    void testCacheReuse()
    {
        beginTest ("cache reuse: a new client with the same cache root downloads nothing again");

        const auto root = testDir ("reuse");
        const auto hostUser = newId(), userB = newId();
        const auto cacheB = root.getChildFile ("cacheB");
        ProjectHost host ({ 22226, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        Peer a (root.getChildFile ("cacheA"), hostUser, "Lucas");
        Peer b (cacheB, userB, "Ana");
        expect (a.connectTo (22226, host.getSessionId()));
        expect (b.connectTo (22226, host.getSessionId()));

        const auto track = newId(), take = newId();
        expect (addOwnTrack (a, track, "Bajo"));
        a->beginTake (take, 1, track, 0);
        a->queueSegment (take, 0, makeSegment (48000, 1, 110.0f));
        a->queueSegment (take, 1, makeSegment (30000, 1, 220.0f));
        Region region;
        region.id = newId();
        region.take = take;
        region.length = 78000;
        a->finishTake (take, track, region);

        expect (waitUntil ([&] { return b->cache()->segmentCount (take) == 2 && hasRegion (*b->project(), track, region.id); }, 10000));
        expect (b->bytesDownloaded() > 0);
        expect (waitUntil ([&] { return b->transfers().downloadingTakes == 0; }, 2000));

        // The same user connecting again replaces B's connection on the host: B sees a disconnect.
        Peer b2 (cacheB, userB, "Ana");
        expect (b2.connectTo (22226, host.getSessionId()));
        expect (waitUntil ([&] { return b.disconnected.size() == 1; }, 5000), "the replaced client reports the disconnect");
        expect (! b->isConnected());

        pumpMessages (1000);
        expectEquals ((int) b2->bytesDownloaded(), 0);
        expectEquals (b2->cache()->segmentCount (take), 2);
        expectEquals (b2->transfers().downloadingTakes, 0);

        // The data connection works: a new take of A arrives.
        const auto take2 = newId();
        a->beginTake (take2, 1, track, 78000);
        a->queueSegment (take2, 0, makeSegment (1000, 1, 330.0f));
        expect (waitUntil ([&] { return b2->cache()->segmentCount (take2) == 1; }, 5000));
        expect (b2->bytesDownloaded() > 0);

        a->disconnect();
        b2->disconnect();
        host.stop();
    }

    //==========================================================================
    // A scripted host that skips a version and later sends an op that cannot apply.
    struct LineSocket
    {
        std::unique_ptr<juce::StreamingSocket> socket;
        std::string pending;

        juce::var readMessage (int timeoutMs, const std::atomic<bool>& stop)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            for (;;)
            {
                const auto newline = pending.find ('\n');

                if (newline != std::string::npos)
                {
                    const auto line = juce::String::fromUTF8 (pending.data(), (int) newline);
                    pending.erase (0, newline + 1);

                    if (auto parsed = parseLine (line))
                        return *parsed;

                    continue;
                }

                if (stop.load() || juce::Time::getMillisecondCounter() >= deadline)
                    return {};

                const auto ready = socket->waitUntilReady (true, 50);

                if (ready < 0)
                    return {};

                if (ready == 0)
                    continue;

                char buffer[8192];
                const auto n = socket->read (buffer, sizeof (buffer), false);

                if (n <= 0)
                    return {};

                pending.append (buffer, (size_t) n);
            }
        }

        void send (const juce::var& message)
        {
            const auto line = makeLine (message);
            socket->write (line.toRawUTF8(), (int) line.getNumBytesAsUTF8());
        }
    };

    static juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();

        for (const auto& [key, value] : fields)
            o->setProperty (key, value);

        return juce::var (o);
    }

    void testVersionGap()
    {
        beginTest ("a skipped version or an op that does not apply triggers resync; the replica ends equal to the host");

        const auto root = testDir ("gap");
        const auto user = newId(), hostUser = newId();

        auto p = makeProject ("Fake", hostUser, "Host");
        expect (applyOp (p, Ops::addUser (user, "Ana"), hostUser).ok);   // version 1

        // What the fake host "really" has: two ops the client never sees.
        auto truth = p;
        const auto hidden = newId(), visible = newId();
        expect (applyOp (truth, Ops::addTrack (hidden, "Oculta", TrackKind::voice), hostUser).ok);    // 2 (skipped)
        const auto skippedOp = Ops::addTrack (newId(), "Visible", TrackKind::voice);
        expect (applyOp (truth, skippedOp, hostUser).ok);                                              // 3
        const auto truthAfterGap = truth;
        const auto validOp = Ops::addTrack (visible, "Siguiente", TrackKind::voice);
        expect (applyOp (truth, validOp, hostUser).ok);                                                // 4
        const auto badOp = Ops::renameTrack (newId(), "Nadie");                                        // fails to apply
        expect (applyOp (truth, Ops::setTempo (90.0, 3), hostUser).ok);                                // 5 (host's real op)
        const auto finalTruth = truth;

        juce::StreamingSocket listener;
        expect (listener.createListener (22227, "127.0.0.1"));

        std::atomic<bool> stop { false };
        std::atomic<int> resyncs { 0 };
        std::atomic<bool> scriptOk { false };

        std::thread fake ([&]
        {
            LineSocket control { std::unique_ptr<juce::StreamingSocket> (listener.waitForNextConnection()) };

            if (control.socket == nullptr)
                return;

            const auto hello = control.readMessage (5000, stop);

            if (hello["t"].toString() != "hello" || (int) hello["protocol"] != 2 || hello["user"].toString() != user)
                return;

            control.send (object ({ { "t", "welcome" }, { "clientId", 7 }, { "token", "0123456789abcdef" },
                                    { "udpPort", 22227 }, { "isHost", false }, { "project", toVar (p) } }));

            LineSocket data { std::unique_ptr<juce::StreamingSocket> (listener.waitForNextConnection()) };

            if (data.socket == nullptr)
                return;

            const auto dataHello = data.readMessage (5000, stop);

            if (dataHello["t"].toString() != "data_hello" || (int) dataHello["clientId"] != 7)
                return;

            auto waitResync = [&]
            {
                for (;;)
                {
                    const auto m = control.readMessage (5000, stop);

                    if (! m.isObject())
                        return false;

                    if (m["t"].toString() == "resync")
                        return true;
                }
            };

            // Version 3 arrives while the replica is at 1.
            control.send (object ({ { "t", "applied" }, { "version", 3 }, { "by", hostUser }, { "op", skippedOp } }));

            if (! waitResync())
                return;

            ++resyncs;
            control.send (object ({ { "t", "project" }, { "project", toVar (truthAfterGap) } }));
            control.send (object ({ { "t", "applied" }, { "version", 4 }, { "by", hostUser }, { "op", validOp } }));

            // Version 5 is in sequence but cannot be applied to the replica.
            control.send (object ({ { "t", "applied" }, { "version", 5 }, { "by", hostUser }, { "op", badOp } }));

            if (! waitResync())
                return;

            ++resyncs;
            control.send (object ({ { "t", "project" }, { "project", toVar (finalTruth) } }));
            scriptOk = true;

            while (! stop.load())
                juce::Thread::sleep (20);
        });

        Peer b (root.getChildFile ("cacheB"), user, "Ana");
        expect (b.connectTo (22227, "ABCDEF"), "connects to the fake host");

        expect (waitUntil ([&] { return scriptOk.load() && contentHash (*b->project()) == contentHash (finalTruth); }, 8000),
                "the replica ends equal to the host");
        expectEquals (resyncs.load(), 2);
        expectEquals ((int) b->project()->version, 5);
        expect (b->project()->findTrack (hidden) != nullptr && b->project()->findTrack (visible) != nullptr);

        b->disconnect();
        stop = true;
        listener.close();
        fake.join();
    }

    //==========================================================================
    void testPreview()
    {
        beginTest ("preview: a listening client receives another client's datagrams in its jitter buffer");

        const auto root = testDir ("preview");
        const auto hostUser = newId(), userB = newId();
        ProjectHost host ({ 22228, 8 });
        expect (host.start (root.getChildFile ("host"), hostUser, "Lucas", "Tema").wasOk());

        Peer a (root.getChildFile ("cacheA"), hostUser, "Lucas");
        Peer b (root.getChildFile ("cacheB"), userB, "Ana");
        expect (a.connectTo (22228, host.getSessionId()));
        expect (b.connectTo (22228, host.getSessionId()));

        a->setListening (true);
        pumpMessages (1500);   // A's keepalive teaches the host its UDP endpoint

        std::array<uint8_t, 48> payload {};

        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = (uint8_t) (i * 3 + 1);

        bool received = false;
        std::array<uint8_t, 96> out {};
        int outLen = 0;
        uint16_t seq = 0;
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;

        while (! received && juce::Time::getMillisecondCounter() < deadline)
        {
            AudioPacketView view;
            view.flags = AudioFlags::voice;
            view.frameSize = FrameSize::f128;
            view.seq = seq++;
            view.clientId = 0;   // stamped by the client
            view.voice = payload.data();
            view.voiceLen = (int) payload.size();
            uint8_t packet[maxPacketBytes];
            const auto len = encodeAudioPacket (view, packet, sizeof (packet));
            b->sendPreview (packet, len);
            pumpMessages (3);

            received = a->previewBuffer()->pop (out.data(), outLen);
        }

        expect (received, "A pops a preview block");
        expectEquals (outLen, 48);
        expect (std::equal (payload.begin(), payload.end(), out.begin()));

        a->disconnect();
        b->disconnect();
        host.stop();
    }

    //==========================================================================
    void testSilentHost()
    {
        beginTest ("a host that accepts but never answers hello ends in onError");

        juce::StreamingSocket listener;
        expect (listener.createListener (22229, "127.0.0.1"));
        std::unique_ptr<juce::StreamingSocket> accepted;
        std::thread acceptor ([&] { accepted.reset (listener.waitForNextConnection()); });

        Peer b (testDir ("silent").getChildFile ("cacheB"), newId(), "Ana");
        b->connect (SessionLink { "127.0.0.1", 22229, "ABCDEF" });
        expect (waitUntil ([&] { return b.errors.size() == 1; }, 15000), "onError after the welcome timeout");
        expect (! b.connected && ! b->isConnected() && b.disconnected.isEmpty());

        listener.close();
        acceptor.join();
    }
};

static ProjectClientTests projectClientTests;
}
