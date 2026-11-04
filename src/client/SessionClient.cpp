#include "client/SessionClient.h"
#include "core/Sha1.h"

#if JUCE_WINDOWS
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <mswsock.h>
 #include <avrt.h>
#endif

#include <algorithm>
#include <cstring>

namespace ensayo
{
namespace
{
    juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

    constexpr int pingIntervalMs = 1000;
    constexpr int connectTimeoutMs = 5000;
    constexpr size_t maxLineBytes = 64 * 1024;
    constexpr int uploadChunkBytes = 64 * 1024;

    constexpr int writeStallTimeoutMs = 10000;

    // juce::StreamingSocket::write is a bare send() on a non-blocking socket, so writes can be partial or fail
    // with WSAEWOULDBLOCK and must be retried. The retry must NOT use waitUntilReady: JUCE implements it with a
    // try-lock on the socket's read lock, which the control thread holds almost permanently inside its own
    // waitUntilReady (true, 100) loop, so a writer waiting that way fails as soon as the send buffer is full
    // (and, when it does win the lock, makes the reader's wait fail instead). A short sleep is enough: the
    // kernel drains the send buffer independently of this thread.
    bool writeAll (juce::StreamingSocket& socket, const void* data, int len, const juce::Thread* thread = nullptr)
    {
        const auto* bytes = static_cast<const char*> (data);
        int written = 0;
        int stalledMs = 0;

        while (written < len)
        {
            if (thread != nullptr && thread->threadShouldExit())
                return false;

            if (! socket.isConnected())
                return false;

            const int w = socket.write (bytes + written, len - written);

            if (w > 0)
            {
                written += w;
                stalledMs = 0;
                continue;
            }

           #if JUCE_WINDOWS
            if (w < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
           #else
            if (w < 0 && errno != EWOULDBLOCK && errno != EAGAIN)
                return false;
           #endif

            if (stalledMs >= writeStallTimeoutMs)
                return false;

            juce::Thread::sleep (2);
            stalledMs += 2;
        }

        return true;
    }

    std::optional<FrameSize> frameSizeFromVar (const juce::var& v)
    {
        if (v.isString())
        {
            const auto text = v.toString().trim().toLowerCase();

            if (text == "64" || text == "f64")   return FrameSize::f64;
            if (text == "128" || text == "f128") return FrameSize::f128;
            return std::nullopt;
        }

        const int value = (int) v;

        if (auto fs = frameSizeForSamples (value))
            return fs;

        if (value == 0) return FrameSize::f64;    // frame code
        if (value == 1) return FrameSize::f128;
        return std::nullopt;
    }
}

//==============================================================================
// Reads lines from the control socket and pings once per second.
class SessionClient::ControlThread : public juce::Thread
{
public:
    ControlThread (SessionClient& o, juce::String hostToUse, int portToUse, juce::var helloMessage)
        : juce::Thread ("EnsayoControl"), owner (o), host (std::move (hostToUse)), port (portToUse), hello (std::move (helloMessage))
    {
    }

    void run() override
    {
        if (! owner.socket.connect (host, port, connectTimeoutMs))
        {
            owner.handleDisconnected ("No se pudo conectar a " + host + ":" + juce::String (port));
            return;
        }

        owner.sendLine (hello);

        std::vector<char> pending;
        std::vector<char> chunk (16 * 1024);
        auto nextPing = juce::Time::getMillisecondCounter() + 200;

        while (! threadShouldExit())
        {
            const auto now = juce::Time::getMillisecondCounter();

            if (now >= nextPing)
            {
                auto ping = makeMessage ("ping");
                ping.getDynamicObject()->setProperty ("t0", juce::Time::getMillisecondCounterHiRes());

                if (owner.trySendLine (ping))
                    nextPing = now + (juce::uint32) pingIntervalMs;
                else
                    nextPing = now + 100;   // an upload holds the socket; retry soon
            }

            const int ready = owner.socket.waitUntilReady (true, 100);

            if (ready < 0)
                break;

            if (ready == 0)
                continue;

            const int n = owner.socket.read (chunk.data(), (int) chunk.size(), false);

            if (n <= 0)
                break;

            pending.insert (pending.end(), chunk.begin(), chunk.begin() + n);

            for (;;)
            {
                const auto newline = std::find (pending.begin(), pending.end(), '\n');

                if (newline == pending.end())
                    break;

                const juce::String line (juce::CharPointer_UTF8 (pending.data()), juce::CharPointer_UTF8 (&*newline));
                pending.erase (pending.begin(), newline + 1);
                owner.handleLine (line);
            }

            if (pending.size() > maxLineBytes)
            {
                owner.handleDisconnected ("El servidor " + utf8 ("envi\xc3\xb3") + " una " + utf8 ("l\xc3\xadnea") + " demasiado larga");
                return;
            }
        }

        if (! threadShouldExit())
            owner.handleDisconnected (utf8 ("Se perdi\xc3\xb3 la conexi\xc3\xb3n con el servidor"));
    }

private:
    SessionClient& owner;
    const juce::String host;
    const int port;
    const juce::var hello;
};

//==============================================================================
// Raw Winsock UDP socket: drains the outgoing queue and feeds mix packets into the jitter buffer.
class SessionClient::UdpThread : public juce::Thread
{
public:
    UdpThread (SessionClient& o, juce::String hostToUse, int portToUse)
        : juce::Thread ("EnsayoUdp"), owner (o), host (std::move (hostToUse)), port (portToUse)
    {
    }

    ~UdpThread() override
    {
        signalThreadShouldExit();
        stopThread (3000);
    }

    void run() override
    {
       #if JUCE_WINDOWS
        WSADATA wsaData;

        if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
            return;

        sockaddr_in server {};

        if (! resolve (server))
        {
            WSACleanup();
            return;
        }

        const SOCKET sock = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock == INVALID_SOCKET)
        {
            WSACleanup();
            return;
        }

        // Do not turn ICMP port-unreachable into recvfrom errors.
        BOOL disable = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl (sock, SIO_UDP_CONNRESET, &disable, sizeof (disable), nullptr, 0, &bytesReturned, nullptr, nullptr);

        const DWORD receiveTimeoutMs = 2;
        setsockopt (sock, SOL_SOCKET, SO_RCVTIMEO, (const char*) &receiveTimeoutMs, sizeof (receiveTimeoutMs));

        const int bufferBytes = 256 * 1024;
        setsockopt (sock, SOL_SOCKET, SO_RCVBUF, (const char*) &bufferBytes, sizeof (bufferBytes));
        setsockopt (sock, SOL_SOCKET, SO_SNDBUF, (const char*) &bufferBytes, sizeof (bufferBytes));

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl (INADDR_ANY);
        local.sin_port = 0;

        if (::bind (sock, (const sockaddr*) &local, sizeof (local)) != 0)
        {
            closesocket (sock);
            WSACleanup();
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);   // best effort

        std::vector<uint8_t> toSend (maxPacketBytes), incoming (maxPacketBytes);

        while (! threadShouldExit())
        {
            size_t len = 0;

            while (owner.outgoing.pop (toSend.data(), len))
                sendto (sock, (const char*) toSend.data(), (int) len, 0, (const sockaddr*) &server, sizeof (server));

            sockaddr_in from {};
            int fromLen = sizeof (from);
            const int n = recvfrom (sock, (char*) incoming.data(), (int) incoming.size(), 0, (sockaddr*) &from, &fromLen);

            if (n <= 0)
                continue;   // timeout or transient error; loop back to drain the queue

            AudioPacketView view;

            if (! decodeAudioPacket (incoming.data(), (size_t) n, view) || (view.flags & AudioFlags::mix) == 0)
                continue;

            const auto arrival = juce::Time::getMillisecondCounterHiRes();
            owner.trackPosition.store (view.trackPosition, std::memory_order_relaxed);
            owner.buffer->push (view.seq, view.voice, view.voiceLen, arrival);
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);

        closesocket (sock);
        WSACleanup();
       #endif
    }

private:
   #if JUCE_WINDOWS
    bool resolve (sockaddr_in& out)
    {
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;

        if (getaddrinfo (host.toRawUTF8(), nullptr, &hints, &result) != 0 || result == nullptr)
            return false;

        std::memcpy (&out, result->ai_addr, sizeof (sockaddr_in));
        out.sin_port = htons ((u_short) port);
        freeaddrinfo (result);
        return true;
    }
   #endif

    SessionClient& owner;
    const juce::String host;
    const int port;
};

//==============================================================================
// Streams one file through the control socket: the track_upload_begin line followed by the raw bytes.
class SessionClient::UploadThread : public juce::Thread
{
public:
    UploadThread (SessionClient& o, juce::File fileToSend)
        : juce::Thread ("EnsayoUpload"), owner (o), file (std::move (fileToSend))
    {
    }

    ~UploadThread() override
    {
        signalThreadShouldExit();
        stopThread (10000);
    }

    void run() override
    {
        const auto size = file.getSize();

        if (! file.existsAsFile() || size <= 0)
        {
            fail ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        status ("Calculando checksum" + utf8 ("\xe2\x80\xa6"), 0.0f);
        const auto sha1 = Sha1::hexOfFile (file);

        if (sha1.isEmpty())
        {
            fail ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        juce::FileInputStream in (file);

        if (! in.openedOk())
        {
            fail ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        auto begin = makeMessage ("track_upload_begin");
        begin.getDynamicObject()->setProperty ("name", file.getFileName());
        begin.getDynamicObject()->setProperty ("size", (juce::int64) size);
        begin.getDynamicObject()->setProperty ("sha1", sha1);

        status ("Subiendo 0 %", 0.0f);

        // The header line and the raw bytes must not be interleaved with pings, so the socket is
        // held for the whole upload; the control thread keeps reading replies meanwhile.
        const juce::ScopedLock lock (owner.writeLock);

        const auto header = makeLine (begin);
        const auto* headerBytes = header.toRawUTF8();

        if (! writeAll (owner.socket, headerBytes, (int) std::strlen (headerBytes), this))
        {
            fail ("No se pudo enviar el archivo");
            return;
        }

        std::vector<char> chunk ((size_t) uploadChunkBytes);
        juce::int64 sent = 0;
        int lastPercent = -1;

        while (sent < size && ! threadShouldExit())
        {
            const int n = in.read (chunk.data(), (int) juce::jmin ((juce::int64) chunk.size(), size - sent));

            if (n <= 0)
            {
                fail ("No se pudo leer el archivo durante la subida");
                return;
            }

            if (! writeAll (owner.socket, chunk.data(), n, this))
            {
                fail ("No se pudo enviar el archivo");
                return;
            }

            sent += n;
            const int percent = (int) (sent * 100 / size);

            if (percent != lastPercent)
            {
                lastPercent = percent;
                status ("Subiendo " + juce::String (percent) + " %", (float) sent / (float) size);
            }
        }

        if (! threadShouldExit())
            status ("Esperando al servidor" + utf8 ("\xe2\x80\xa6"), 1.0f);
    }

private:
    // The lambdas capture the client, not this thread: the client may delete the thread before they run.
    void status (const juce::String& text, float progress)
    {
        owner.post ([client = &owner, text, progress]
        {
            client->state.trackUploadStatus = text;
            client->state.trackUploadProgress = progress;
            client->notifyStateChanged();
        });
    }

    void fail (const juce::String& message)
    {
        owner.post ([client = &owner, message]
        {
            client->state.trackUploadStatus = message;
            client->state.trackUploadProgress = 0.0f;
            client->notifyStateChanged();

            if (client->onError != nullptr)
                client->onError (message);
        });
    }

    SessionClient& owner;
    const juce::File file;
};

//==============================================================================
SessionClient::SessionClient()
    : buffer (std::make_unique<JitterBuffer> (codedBytesStereo (FrameSize::f128), frameSamples (FrameSize::f128) / 48.0)),
      outgoing (64),
      generation (std::make_shared<std::atomic<int>> (0))
{
}

SessionClient::~SessionClient()
{
    leave();
}

void SessionClient::create (const juce::String& host, int port, const juce::String& name, Role role)
{
    auto hello = makeMessage ("create");
    hello.getDynamicObject()->setProperty ("name", name);
    hello.getDynamicObject()->setProperty ("role", roleToString (role));
    connect (host, port, hello);
}

void SessionClient::join (const SessionLink& link, const juce::String& name, Role role)
{
    auto hello = makeMessage ("join");
    hello.getDynamicObject()->setProperty ("session", link.sessionId);
    hello.getDynamicObject()->setProperty ("name", name);
    hello.getDynamicObject()->setProperty ("role", roleToString (role));
    connect (link.host, link.port, hello);
}

void SessionClient::connect (const juce::String& host, int port, const juce::var& hello)
{
    leave();

    state = SessionState();
    state.host = host;
    state.port = port;
    serverHost = host;
    serverUdpPort = 0;
    clientId.store (0);
    trackPosition.store (0);
    buffer->reset();

    control = std::make_unique<ControlThread> (*this, host, port, hello);
    control->startThread();
}

void SessionClient::stopThreads()
{
    upload = nullptr;

    if (control != nullptr)
    {
        control->signalThreadShouldExit();
        socket.close();
        control->stopThread (3000);
        control = nullptr;
    }
    else
    {
        socket.close();
    }

    udp = nullptr;
}

void SessionClient::leave()
{
    ++(*generation);   // callbacks queued by the old threads are dropped
    stopThreads();

    if (state.connected)
    {
        state.connected = false;
        notifyStateChanged();
    }
}

void SessionClient::sendGain (uint32_t target, const ChannelGain& gain)
{
    if (! state.connected)
        return;

    auto msg = makeMessage ("gain");
    auto* o = msg.getDynamicObject();

    if (target == 0)
        o->setProperty ("target", "track");
    else
        o->setProperty ("target", (juce::int64) target);

    o->setProperty ("voice", (double) gain.voiceDb);
    o->setProperty ("instrument", (double) gain.instrumentDb);
    o->setProperty ("mute", gain.muted);
    sendLine (msg);
}

void SessionClient::sendTransport (const juce::String& cmd, int64_t position, bool loop)
{
    if (! state.connected)
        return;

    auto msg = makeMessage ("transport");
    auto* o = msg.getDynamicObject();
    o->setProperty ("cmd", cmd);
    o->setProperty ("position", (juce::int64) position);
    o->setProperty ("loop", loop);
    sendLine (msg);
}

void SessionClient::uploadTrack (const juce::File& file)
{
    if (! state.connected)
    {
        if (onError != nullptr)
            onError ("No hay " + utf8 ("sesi\xc3\xb3n") + " activa");

        return;
    }

    upload = nullptr;
    state.trackUploadStatus = "Preparando" + utf8 ("\xe2\x80\xa6");
    state.trackUploadProgress = 0.0f;
    notifyStateChanged();

    upload = std::make_unique<UploadThread> (*this, file);
    upload->startThread();
}

const SessionState& SessionClient::getState() const noexcept
{
    return state;
}

//==============================================================================
void SessionClient::sendAudio (const uint8_t* packet, size_t len) noexcept
{
    outgoing.push (packet, len);
}

JitterBuffer& SessionClient::receiveBuffer() noexcept
{
    return *buffer;
}

uint32_t SessionClient::getClientId() const noexcept
{
    return clientId.load (std::memory_order_relaxed);
}

uint32_t SessionClient::getTrackPosition() const noexcept
{
    return trackPosition.load (std::memory_order_relaxed);
}

//==============================================================================
void SessionClient::sendLine (const juce::var& message)
{
    const auto line = makeLine (message);
    const auto* bytes = line.toRawUTF8();
    const juce::ScopedLock lock (writeLock);
    writeAll (socket, bytes, (int) std::strlen (bytes));
}

bool SessionClient::trySendLine (const juce::var& message)
{
    const auto line = makeLine (message);
    const auto* bytes = line.toRawUTF8();
    const juce::ScopedTryLock lock (writeLock);

    if (! lock.isLocked())
        return false;

    writeAll (socket, bytes, (int) std::strlen (bytes));
    return true;
}

void SessionClient::post (std::function<void()> fn)
{
    juce::MessageManager::callAsync ([gen = generation, my = generation->load(), fn = std::move (fn)]
    {
        if (gen->load() == my)
            fn();
    });
}

void SessionClient::notifyStateChanged()
{
    if (onStateChanged != nullptr)
        onStateChanged (state);
}

std::vector<ParticipantInfo> SessionClient::parseParticipants (const juce::var& list)
{
    std::vector<ParticipantInfo> result;

    if (auto* array = list.getArray())
    {
        for (const auto& item : *array)
        {
            ParticipantInfo info;
            info.id = (uint32_t) (juce::int64) item["id"];
            info.name = item["name"].toString();
            info.role = roleFromString (item["role"].toString()).value_or (Role::both);
            result.push_back (info);
        }
    }

    return result;
}

std::vector<ParticipantStats> SessionClient::parseStats (const juce::var& list)
{
    std::vector<ParticipantStats> result;

    if (auto* array = list.getArray())
    {
        for (const auto& item : *array)
        {
            ParticipantStats s;
            s.id = (uint32_t) (juce::int64) item["id"];
            s.jitterBlocks = (int) item["jitterBlocks"];
            s.underruns = (uint32_t) (juce::int64) item["underruns"];
            s.late = (uint32_t) (juce::int64) item["late"];
            s.voicePeak = (float) (double) item["voicePeak"];
            s.instrumentPeak = (float) (double) item["instrumentPeak"];
            result.push_back (s);
        }
    }

    return result;
}

//==============================================================================
// Control thread.
void SessionClient::handleLine (const juce::String& line)
{
    const auto parsed = parseLine (line);

    if (! parsed.has_value())
        return;

    const auto msg = *parsed;
    const auto type = msg["t"].toString();

    if (type == "joined")
    {
        handleJoined (msg);
    }
    else if (type == "error")
    {
        const auto message = msg["message"].toString();

        post ([this, message]
        {
            if (onError != nullptr)
                onError (message.isNotEmpty() ? message : juce::String ("Error del servidor"));
        });
    }
    else if (type == "participants")
    {
        auto participants = parseParticipants (msg.hasProperty ("participants") ? msg["participants"] : msg["list"]);

        post ([this, participants = std::move (participants)]
        {
            state.participants = participants;
            notifyStateChanged();
        });
    }
    else if (type == "pong")
    {
        const auto rtt = juce::Time::getMillisecondCounterHiRes() - (double) msg["t0"];

        post ([this, rtt]
        {
            state.pingMs = juce::jmax (0, juce::roundToInt (rtt));
            notifyStateChanged();
        });
    }
    else if (type == "transport_state")
    {
        auto transport = transportFromVar (msg);
        transport.loaded = transport.loaded || transport.length > 0 || transport.playing;

        post ([this, transport]
        {
            state.transport = transport;
            notifyStateChanged();
        });
    }
    else if (type == "track_ready")
    {
        const auto name = msg["name"].toString();
        const auto length = (int64_t) (juce::int64) msg["lengthSamples"];

        post ([this, name, length]
        {
            state.transport.loaded = true;
            state.transport.name = name;
            state.transport.length = length;
            state.trackUploadStatus = "Listo";
            state.trackUploadProgress = 1.0f;
            notifyStateChanged();
        });
    }
    else if (type == "stats")
    {
        auto stats = parseStats (msg.hasProperty ("stats") ? msg["stats"] : msg["participants"]);

        post ([this, stats = std::move (stats)]
        {
            state.stats = stats;
            notifyStateChanged();
        });
    }
}

void SessionClient::handleJoined (const juce::var& msg)
{
    const auto sessionId = msg["session"].toString();
    const auto id = (uint32_t) (juce::int64) msg["clientId"];
    const int udpPort = msg.hasProperty ("udpPort") ? (int) msg["udpPort"] : state.port;
    const auto frameSize = frameSizeFromVar (msg["frameSize"]).value_or (FrameSize::f128);
    auto participants = parseParticipants (msg["participants"]);

    post ([this, sessionId, id, udpPort, frameSize, participants = std::move (participants)]
    {
        state.sessionId = sessionId;
        state.clientId = id;
        state.frameSize = frameSize;
        state.participants = participants;
        state.connected = true;
        clientId.store (id);
        serverUdpPort = udpPort;

        // No audio is attached before onJoined, so the buffer can be rebuilt for the session's frame size.
        buffer = std::make_unique<JitterBuffer> (codedBytesStereo (frameSize), frameSamples (frameSize) / 48.0);

        udp = std::make_unique<UdpThread> (*this, serverHost, serverUdpPort);
        udp->startThread (juce::Thread::Priority::highest);

        notifyStateChanged();

        if (onJoined != nullptr)
            onJoined();
    });
}

void SessionClient::handleDisconnected (const juce::String& reason)
{
    post ([this, reason]
    {
        state.connected = false;
        stopThreads();
        notifyStateChanged();

        if (onError != nullptr)
            onError (reason);
    });
}
}
