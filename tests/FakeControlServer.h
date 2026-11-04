#pragma once
// In-test stand-in for ServerHost: a TCP control responder (JSON lines) and a UDP echo that answers
// every audio packet with a silent mix packet. Implements only the subset of the protocol that
// SessionClient needs: create/join → joined + participants, ping → pong, transport → transport_state,
// track_upload_begin + raw bytes → track_ready + transport_state, gain/jitter ignored.

#include "core/Protocol.h"
#include "core/Sha1.h"

#include <juce_core/juce_core.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <vector>

namespace ensayo
{
class FakeControlServer
{
public:
    FakeControlServer (int tcpPortToUse, int udpPortToUse, FrameSize frameSizeToUse = FrameSize::f128)
        : tcpPort (tcpPortToUse), udpPort (udpPortToUse), frameSize (frameSizeToUse)
    {
        juce::Random random;
        sessionId = generateSessionId (random);
    }

    ~FakeControlServer() { stop(); }

    bool start()
    {
        if (! listener.createListener (tcpPort, "127.0.0.1"))
            return false;

        if (! udp.bindToPort (udpPort, "127.0.0.1"))
            return false;

        acceptThread = std::make_unique<AcceptThread> (*this);
        acceptThread->startThread();
        udpThread = std::make_unique<UdpThread> (*this);
        udpThread->startThread (juce::Thread::Priority::high);
        return true;
    }

    void stop()
    {
        if (acceptThread != nullptr)
        {
            acceptThread->signalThreadShouldExit();
            listener.close();
            acceptThread->stopThread (3000);
            acceptThread = nullptr;
        }

        if (udpThread != nullptr)
        {
            udpThread->signalThreadShouldExit();
            udpThread->stopThread (3000);
            udpThread = nullptr;
        }

        std::vector<std::unique_ptr<Connection>> toClose;

        {
            const juce::ScopedLock lock (stateLock);
            toClose.swap (connections);
        }

        for (auto& c : toClose)
        {
            c->signalThreadShouldExit();
            c->socket->close();
            c->stopThread (3000);
        }

        udp.shutdown();
    }

    juce::String getSessionId() const { return sessionId; }
    int getParticipantCount() const { const juce::ScopedLock lock (stateLock); return (int) participants.size(); }
    int getUdpPacketsReceived() const { return udpPacketsReceived.load(); }
    int getUploadedBytes() const { return uploadedBytes.load(); }
    juce::String getLastUploadSha1() const { const juce::ScopedLock lock (stateLock); return lastUploadSha1; }
    TransportState getTransport() const { const juce::ScopedLock lock (stateLock); return transport; }

private:
    struct Participant { uint32_t id; juce::String name; juce::String role; };

    struct Connection : public juce::Thread
    {
        Connection (FakeControlServer& o, juce::StreamingSocket* s)
            : juce::Thread ("FakeControlConnection"), owner (o), socket (s) {}

        void run() override { owner.serve (*this); }

        // StreamingSocket::write is a bare send() on a socket that is non-blocking after the first
        // non-blocking read, so partial writes and WSAEWOULDBLOCK have to be retried.
        void sendLine (const juce::String& line)
        {
            const juce::ScopedLock lock (writeLock);
            const auto* bytes = line.toRawUTF8();
            const int len = (int) std::strlen (bytes);
            int written = 0;

            while (written < len && ! threadShouldExit())
            {
                const int w = socket->write (bytes + written, len - written);

                if (w > 0)
                    written += w;
                else if (socket->waitUntilReady (false, 100) < 0)
                    return;
            }
        }

        FakeControlServer& owner;
        std::unique_ptr<juce::StreamingSocket> socket;
        juce::CriticalSection writeLock;
        uint32_t clientId = 0;
    };

    struct AcceptThread : public juce::Thread
    {
        explicit AcceptThread (FakeControlServer& o) : juce::Thread ("FakeControlAccept"), owner (o) {}

        void run() override
        {
            while (! threadShouldExit())
            {
                auto* s = owner.listener.waitForNextConnection();

                if (s == nullptr)
                    continue;

                auto connection = std::make_unique<Connection> (owner, s);
                connection->startThread();
                const juce::ScopedLock lock (owner.stateLock);
                owner.connections.push_back (std::move (connection));
            }
        }

        FakeControlServer& owner;
    };

    struct UdpThread : public juce::Thread
    {
        explicit UdpThread (FakeControlServer& o) : juce::Thread ("FakeControlUdp"), owner (o) {}

        void run() override
        {
            std::vector<uint8_t> in (maxPacketBytes), out (maxPacketBytes);
            std::vector<uint8_t> silence ((size_t) codedBytesStereo (owner.frameSize), 0);
            uint32_t trackPosition = 0;

            while (! threadShouldExit())
            {
                if (owner.udp.waitUntilReady (true, 50) != 1)
                    continue;

                juce::String senderIp;
                int senderPort = 0;
                const int n = owner.udp.read (in.data(), (int) in.size(), false, senderIp, senderPort);

                if (n <= 0)
                    continue;

                AudioPacketView view;

                if (! decodeAudioPacket (in.data(), (size_t) n, view))
                    continue;

                ++owner.udpPacketsReceived;
                trackPosition += (uint32_t) frameSamples (view.frameSize);

                AudioPacketView mix;
                mix.flags = AudioFlags::mix;
                mix.frameSize = owner.frameSize;
                mix.seq = view.seq;
                mix.clientId = 0;
                mix.sampleTime = view.sampleTime;
                mix.trackPosition = trackPosition;
                mix.voice = silence.data();
                mix.voiceLen = (int) silence.size();

                const auto len = encodeAudioPacket (mix, out.data(), out.size());

                if (len > 0)
                    owner.udp.write (senderIp, senderPort, out.data(), (int) len);
            }
        }

        FakeControlServer& owner;
    };

    juce::var participantsVar() const
    {
        juce::Array<juce::var> list;

        for (const auto& p : participants)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", (juce::int64) p.id);
            o->setProperty ("name", p.name);
            o->setProperty ("role", p.role);
            list.add (juce::var (o));
        }

        return juce::var (list);
    }

    void broadcast (const juce::String& line)
    {
        std::vector<Connection*> targets;

        {
            const juce::ScopedLock lock (stateLock);

            for (auto& c : connections)
                if (c->clientId != 0)
                    targets.push_back (c.get());
        }

        for (auto* c : targets)
            c->sendLine (line);
    }

    void broadcastParticipants()
    {
        auto msg = makeMessage ("participants");
        {
            const juce::ScopedLock lock (stateLock);
            msg.getDynamicObject()->setProperty ("participants", participantsVar());
        }
        broadcast (makeLine (msg));
    }

    void broadcastTransport()
    {
        auto msg = makeMessage ("transport_state");
        {
            const juce::ScopedLock lock (stateLock);
            auto t = transportToVar (transport);

            for (const auto& prop : t.getDynamicObject()->getProperties())
                msg.getDynamicObject()->setProperty (prop.name, prop.value);
        }
        broadcast (makeLine (msg));
    }

    void handleJoin (Connection& c, const juce::var& msg)
    {
        juce::var joined = makeMessage ("joined");

        {
            const juce::ScopedLock lock (stateLock);
            Participant p { nextClientId++, msg["name"].toString(), msg["role"].toString() };
            participants.push_back (p);
            c.clientId = p.id;

            auto* o = joined.getDynamicObject();
            o->setProperty ("session", sessionId);
            o->setProperty ("clientId", (juce::int64) p.id);
            o->setProperty ("udpPort", udpPort);
            o->setProperty ("frameSize", frameSamples (frameSize));
            o->setProperty ("participants", participantsVar());
        }

        c.sendLine (makeLine (joined));
        broadcastParticipants();
    }

    void sendError (Connection& c, const juce::String& message)
    {
        auto msg = makeMessage ("error");
        msg.getDynamicObject()->setProperty ("message", message);
        c.sendLine (makeLine (msg));
    }

    // Returns false when the connection should be dropped.
    bool handleLine (Connection& c, const juce::String& line, std::vector<uint8_t>& pending, juce::StreamingSocket& socket)
    {
        const auto parsed = parseLine (line);

        if (! parsed.has_value())
            return true;

        const auto& msg = *parsed;
        const auto type = msg["t"].toString();

        if (type == "create")
        {
            handleJoin (c, msg);
        }
        else if (type == "join")
        {
            if (msg["session"].toString() != sessionId)
                sendError (c, "sesion desconocida");
            else
                handleJoin (c, msg);
        }
        else if (type == "ping")
        {
            auto pong = makeMessage ("pong");
            pong.getDynamicObject()->setProperty ("t0", msg["t0"]);
            c.sendLine (makeLine (pong));
        }
        else if (type == "transport")
        {
            const auto cmd = msg["cmd"].toString();
            {
                const juce::ScopedLock lock (stateLock);

                if (cmd == "play")       transport.playing = transport.loaded;
                else if (cmd == "pause") transport.playing = false;
                else if (cmd == "stop")  { transport.playing = false; transport.position = 0; }
                else if (cmd == "seek")  transport.position = (int64_t) (juce::int64) msg["position"];
                else if (cmd == "loop")  transport.loop = (bool) msg["loop"];
            }
            broadcastTransport();
        }
        else if (type == "track_upload_begin")
        {
            const auto size = (juce::int64) msg["size"];
            const auto name = msg["name"].toString();

            if (size <= 0 || size > (juce::int64) 64 * 1024 * 1024)
            {
                sendError (c, "tamano invalido");
                return false;
            }

            juce::MemoryBlock data;
            data.append (pending.data(), pending.size());
            pending.clear();

            std::vector<uint8_t> chunk (65536);

            while ((juce::int64) data.getSize() < size && ! c.threadShouldExit())
            {
                const auto want = (int) juce::jmin ((juce::int64) chunk.size(), size - (juce::int64) data.getSize());
                const int ready = socket.waitUntilReady (true, 100);

                if (ready < 0)
                    return false;

                if (ready == 0)
                    continue;

                const int n = socket.read (chunk.data(), want, false);

                if (n < 0)
                    return false;

                data.append (chunk.data(), (size_t) n);
            }

            if ((juce::int64) data.getSize() > size)
            {
                const auto extra = data.getSize() - (size_t) size;
                pending.assign ((const uint8_t*) data.getData() + size, (const uint8_t*) data.getData() + size + extra);
                data.setSize ((size_t) size, true);
            }

            const auto sha = Sha1::hexOf (data.getData(), data.getSize());

            if (sha != msg["sha1"].toString().toLowerCase())
            {
                sendError (c, "sha1 invalido");
                return true;
            }

            {
                const juce::ScopedLock lock (stateLock);
                lastUploadSha1 = sha;
                uploadedBytes = (int) data.getSize();
                transport.loaded = true;
                transport.playing = false;
                transport.position = 0;
                transport.length = sampleRate;
                transport.name = name;
            }

            auto ready = makeMessage ("track_ready");
            ready.getDynamicObject()->setProperty ("name", name);
            ready.getDynamicObject()->setProperty ("lengthSamples", (juce::int64) sampleRate);
            broadcast (makeLine (ready));
            broadcastTransport();
        }

        return true;
    }

    void serve (Connection& c)
    {
        auto& socket = *c.socket;
        std::vector<uint8_t> pending;
        std::vector<uint8_t> buffer (65536);

        while (! c.threadShouldExit())
        {
            const int ready = socket.waitUntilReady (true, 100);

            if (ready < 0)
                break;

            if (ready == 0)
                continue;

            const int n = socket.read (buffer.data(), (int) buffer.size(), false);

            if (n <= 0)
                break;

            pending.insert (pending.end(), buffer.begin(), buffer.begin() + n);

            bool keepGoing = true;

            while (keepGoing)
            {
                auto newline = std::find (pending.begin(), pending.end(), (uint8_t) '\n');

                if (newline == pending.end())
                    break;

                const juce::String line (juce::CharPointer_UTF8 ((const char*) pending.data()),
                                         juce::CharPointer_UTF8 ((const char*) &*newline));
                pending.erase (pending.begin(), newline + 1);
                keepGoing = handleLine (c, line, pending, socket);
            }

            if (! keepGoing)
                break;
        }

        if (c.clientId != 0)
        {
            {
                const juce::ScopedLock lock (stateLock);
                participants.erase (std::remove_if (participants.begin(), participants.end(),
                                                    [&] (const Participant& p) { return p.id == c.clientId; }),
                                    participants.end());
            }

            c.clientId = 0;
            broadcastParticipants();
        }

        socket.close();
    }

    const int tcpPort, udpPort;
    const FrameSize frameSize;
    juce::String sessionId;

    juce::StreamingSocket listener;
    juce::DatagramSocket udp;
    std::unique_ptr<AcceptThread> acceptThread;
    std::unique_ptr<UdpThread> udpThread;

    juce::CriticalSection stateLock;
    std::vector<std::unique_ptr<Connection>> connections;
    std::vector<Participant> participants;
    uint32_t nextClientId = 1;
    TransportState transport;
    juce::String lastUploadSha1;
    std::atomic<int> udpPacketsReceived { 0 }, uploadedBytes { 0 };
};
}
