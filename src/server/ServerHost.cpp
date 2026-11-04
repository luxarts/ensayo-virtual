#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <avrt.h>

#include "ServerHost.h"
#include "core/Sha1.h"

#include <algorithm>
#include <limits>
#include <string>
#include <unordered_map>

namespace ensayo
{
namespace
{
    constexpr size_t maxLineBytes = 8 * 1024;    // longer control lines drop the connection
    constexpr int maxNestingDepth = 32;          // juce::JSON::parse recurses per level: deeper lines never reach it
    constexpr int64_t maxUploadBytes = 512LL * 1024 * 1024;
    constexpr int64_t endpointSilenceMs = 2000;
    constexpr size_t maxConnections = 64;        // live TCP connections (joined or not); more are closed on accept

    // Linear scan of the bracket/brace nesting (ignoring characters inside strings). Unbalanced closers are
    // left to the parser; only the depth matters here.
    bool nestsDeeperThan (const char* data, size_t len, int maxDepth) noexcept
    {
        int depth = 0;
        bool inString = false, escaped = false;

        for (size_t i = 0; i < len; ++i)
        {
            const char c = data[i];

            if (inString)
            {
                if (escaped)            escaped = false;
                else if (c == '\\')     escaped = true;
                else if (c == '"')      inString = false;

                continue;
            }

            if (c == '"')
            {
                inString = true;
            }
            else if (c == '[' || c == '{')
            {
                if (++depth > maxDepth)
                    return true;
            }
            else if (c == ']' || c == '}')
            {
                --depth;
            }
        }

        return false;
    }

    juce::var errorMessage (const juce::String& text)
    {
        auto message = makeMessage ("error");
        message.getDynamicObject()->setProperty ("message", text);
        return message;
    }

    juce::var participantsToVar (const std::vector<ParticipantInfo>& list)
    {
        juce::Array<juce::var> array;

        for (const auto& p : list)
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", (juce::int64) p.id);
            o->setProperty ("name", p.name);
            o->setProperty ("role", roleToString (p.role));
            array.add (juce::var (o));
        }

        return juce::var (array);
    }

    bool isUsableFileName (const juce::String& name) noexcept
    {
        return name.isNotEmpty() && name != "." && name != "..";
    }

    juce::String sanitizeFileName (const juce::String& name)
    {
        const auto trimmed = name.trim();

        if (! isUsableFileName (trimmed))
            return "track";

        auto clean = juce::File::createLegalFileName (trimmed).replaceCharacters ("/\\", "__").substring (0, 120);
        return isUsableFileName (clean) ? clean : "track";
    }

    uint64_t packAddress (const sockaddr_in& address) noexcept
    {
        return ((uint64_t) ntohl (address.sin_addr.s_addr) << 16) | ntohs (address.sin_port);
    }

    sockaddr_in unpackAddress (uint64_t packed) noexcept
    {
        sockaddr_in address {};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl ((u_long) (packed >> 16));
        address.sin_port = htons ((u_short) (packed & 0xFFFFu));
        return address;
    }

    int64_t nowMs() noexcept
    {
        return (int64_t) juce::Time::getMillisecondCounterHiRes();
    }
}

//==============================================================================
struct ServerHost::Session
{
    struct Endpoint
    {
        std::atomic<uint32_t> clientId { 0 };
        std::atomic<uint64_t> address { 0 };     // ip << 16 | port, 0 = unknown
        std::atomic<int64_t> lastSeenMs { 0 };
    };

    Session (ServerHost& h, juce::String sessionId, FrameSize frameSize, int maxParticipants)
        : host (h), id (std::move (sessionId)), mix (frameSize), endpoints ((size_t) juce::jmax (1, maxParticipants))
    {
        sender = [this] (uint32_t clientId, const uint8_t* data, size_t len) { host.sendMix (*this, clientId, data, len); };
    }

    ~Session()
    {
        // Uploaded tracks live in a folder named after the session: gone with it.
        if (isValidSessionId (id))
            host.config.uploadDirectory.getChildFile (id).deleteRecursively();
    }

    Endpoint* findEndpoint (uint32_t clientId) noexcept
    {
        for (auto& e : endpoints)
            if (e.clientId.load (std::memory_order_acquire) == clientId)
                return &e;

        return nullptr;
    }

    ServerHost& host;
    const juce::String id;
    MixServer mix;
    MixServer::Sender sender;
    std::vector<std::shared_ptr<Connection>> connections;
    std::vector<Endpoint> endpoints;                // fixed size: slots are stable for the session's lifetime
    int missedTicks = 0;                            // tick thread only: blocks owed after a MixServer back-off
};

// Immutable snapshot of the live sessions and the clientId -> audio slot map, rebuilt by control threads under
// sessionsLock and read lock-free by tick() and the UDP loop. A snapshot keeps its sessions alive while in use.
struct ServerHost::Registry
{
    struct Slot
    {
        Session* session;
        Session::Endpoint* endpoint;
    };

    std::vector<std::shared_ptr<Session>> sessions;
    std::unordered_map<uint32_t, Slot> endpoints;
};

//==============================================================================
struct ServerHost::Connection : public juce::Thread
{
    static constexpr int64_t maxWriteStallMs = 2000;   // a peer that does not drain its socket for this long is dropped

    Connection (ServerHost& h, juce::StreamingSocket* s)
        : juce::Thread ("EnsayoConnection"), host (h), socket (s)
    {
        // Non-blocking from the start (JUCE's read (..., false) would switch it anyway) so writeAll() never
        // parks a broadcaster inside ::send.
        u_long nonBlocking = 1;
        ioctlsocket ((SOCKET) socket->getRawSocketHandle(), (long) FIONBIO, &nonBlocking);
    }

    ~Connection() override
    {
        socket->close();
        stopThread (2000);
    }

    void send (const juce::var& message)
    {
        const auto line = makeLine (message);
        writeAll (line.toRawUTF8(), line.getNumBytesAsUTF8());
    }

    // Writes the whole buffer, retrying partial writes and WSAEWOULDBLOCK with short sleeps for at most
    // maxWriteStallMs in total. On failure or timeout the connection is marked dead and closed, which makes
    // run() leave the session and finish; later sends return immediately. Called from any thread.
    bool writeAll (const char* data, size_t len)
    {
        const juce::ScopedLock scoped (writeLock);

        if (dead.load (std::memory_order_acquire))
            return false;

        const auto handle = (SOCKET) socket->getRawSocketHandle();
        const auto deadline = nowMs() + maxWriteStallMs;

        while (len > 0)
        {
            const int n = ::send (handle, data, (int) std::min<size_t> (len, 1u << 20), 0);

            if (n > 0)
            {
                data += n;
                len -= (size_t) n;
                continue;
            }

            if (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK && nowMs() < deadline)
            {
                juce::Thread::sleep (2);
                continue;
            }

            markDead();
            return false;
        }

        return true;
    }

    void markDead()
    {
        if (! dead.exchange (true, std::memory_order_acq_rel))
            socket->close();   // the reader loop sees the closed socket and leaves the session the normal way
    }

    void run() override
    {
        while (! threadShouldExit())
        {
            const int ready = socket->waitUntilReady (true, 100);

            if (ready < 0)
                break;

            if (ready == 0)
                continue;

            char buffer[4096];
            const int n = socket->read (buffer, sizeof (buffer), false);

            if (n <= 0)
                break;

            pending.insert (pending.end(), buffer, buffer + n);

            if (! processPendingLines())
                break;
        }

        host.leaveSession (*this);
        socket->close();
        finished.store (true, std::memory_order_release);
    }

    // Handles every complete line in `pending`. False when the connection must be dropped.
    bool processPendingLines()
    {
        for (;;)
        {
            const auto newline = std::find (pending.begin(), pending.end(), '\n');

            if (newline == pending.end())
                return pending.size() <= maxLineBytes;

            const auto lineLen = (size_t) (newline - pending.begin());

            if (lineLen > maxLineBytes || nestsDeeperThan (pending.data(), lineLen, maxNestingDepth))
            {
                send (errorMessage ("Mensaje demasiado grande"));
                return false;
            }

            const juce::String line = juce::String::fromUTF8 (pending.data(), (int) lineLen);
            pending.erase (pending.begin(), newline + 1);

            if (auto message = parseLine (line))
                host.handleMessage (*this, *message);
            else
                send (errorMessage ("Mensaje invalido"));

            if (threadShouldExit() || ! socket->isConnected())
                return false;
        }
    }

    // Reads exactly `count` raw bytes (upload body) into `out`. Drains `pending` first.
    bool readExactly (juce::OutputStream& out, int64_t count)
    {
        const auto fromPending = (size_t) std::min<int64_t> (count, (int64_t) pending.size());
        out.write (pending.data(), fromPending);
        pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) fromPending);
        int64_t remaining = count - (int64_t) fromPending;
        char buffer[16384];

        while (remaining > 0 && ! threadShouldExit())
        {
            const int ready = socket->waitUntilReady (true, 5000);

            if (ready < 0)
                return false;

            if (ready == 0)
                continue;

            const int n = socket->read (buffer, (int) std::min<int64_t> (remaining, (int64_t) sizeof (buffer)), false);

            if (n <= 0)
                return false;

            out.write (buffer, (size_t) n);
            remaining -= n;
        }

        return remaining == 0;
    }

    ServerHost& host;
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection writeLock;
    std::vector<char> pending;
    std::shared_ptr<Session> session;         // set while joined (under sessionsLock)
    uint32_t clientId = 0;
    juce::String name;
    std::atomic<bool> dead { false };         // set by writeAll() on a stalled or failed write
    std::atomic<bool> finished { false };
};

//==============================================================================
class ServerHost::AcceptThread : public juce::Thread
{
public:
    explicit AcceptThread (ServerHost& h) : juce::Thread ("EnsayoAccept"), host (h) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            auto* socket = host.listener != nullptr ? host.listener->waitForNextConnection() : nullptr;

            if (socket == nullptr)
            {
                if (threadShouldExit())
                    break;

                sleep (50);
                continue;
            }

            host.onConnectionAccepted (socket);
        }
    }

private:
    ServerHost& host;
};

class ServerHost::HousekeepingThread : public juce::Thread
{
public:
    explicit HousekeepingThread (ServerHost& h) : juce::Thread ("EnsayoHousekeeping"), host (h) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            wait (1000);

            if (threadShouldExit())
                break;

            host.housekeeping();
        }
    }

private:
    ServerHost& host;
};

//==============================================================================
ServerHost::ServerHost (ServerConfig c) : config (std::move (c))
{
    if (config.uploadDirectory == juce::File())
        config.uploadDirectory = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                     .getChildFile ("EnsayoVirtual").getChildFile ("server");

    config.frameSize = config.frameSize == FrameSize::f64 ? FrameSize::f64 : FrameSize::f128;
    config.maxParticipantsPerSession = juce::jmax (1, config.maxParticipantsPerSession);
}

ServerHost::~ServerHost()
{
    stop();
}

void ServerHost::log (const juce::String& text)
{
    if (onLog)
        onLog (text);
}

int ServerHost::getUdpPort() const noexcept
{
    return boundUdpPort;
}

juce::Result ServerHost::start()
{
    if (running.load())
        return juce::Result::ok();

    WSADATA wsaData;

    if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
        return juce::Result::fail ("WSAStartup fallo");

    winsockStarted = true;

    // UDP audio socket.
    SOCKET udp = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    if (udp == INVALID_SOCKET)
    {
        stop();
        return juce::Result::fail ("No se pudo crear el socket UDP");
    }

    {
        BOOL enable = FALSE;
        DWORD bytes = 0;
        WSAIoctl (udp, SIO_UDP_CONNRESET, &enable, sizeof (enable), nullptr, 0, &bytes, nullptr, nullptr);
        const int bufferSize = 1 << 20;
        setsockopt (udp, SOL_SOCKET, SO_RCVBUF, (const char*) &bufferSize, sizeof (bufferSize));
        setsockopt (udp, SOL_SOCKET, SO_SNDBUF, (const char*) &bufferSize, sizeof (bufferSize));
    }

    sockaddr_in local {};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl (INADDR_ANY);
    local.sin_port = htons ((u_short) config.udpPort);

    if (::bind (udp, (const sockaddr*) &local, sizeof (local)) != 0)
    {
        closesocket (udp);
        stop();
        return juce::Result::fail ("No se pudo abrir el puerto UDP " + juce::String (config.udpPort));
    }

    sockaddr_in bound {};
    int boundLen = sizeof (bound);
    getsockname (udp, (sockaddr*) &bound, &boundLen);
    boundUdpPort = ntohs (bound.sin_port);
    udpSocket = (uintptr_t) udp;

    // TCP control listener.
    listener = std::make_unique<juce::StreamingSocket>();

    if (! listener->createListener (config.tcpPort))
    {
        listener.reset();
        closesocket (udp);
        udpSocket = ~(uintptr_t) 0;
        stop();
        return juce::Result::fail ("No se pudo abrir el puerto TCP " + juce::String (config.tcpPort));
    }

    config.uploadDirectory.createDirectory();
    running.store (true);

    udpThread = std::thread ([this] { udpLoop(); });
    acceptThread = std::make_unique<AcceptThread> (*this);
    acceptThread->startThread();
    housekeepingThread = std::make_unique<HousekeepingThread> (*this);
    housekeepingThread->startThread();

    log ("servidor iniciado tcp=" + juce::String (config.tcpPort) + " udp=" + juce::String (boundUdpPort));
    return juce::Result::ok();
}

void ServerHost::stop()
{
    clock.reset();
    running.store (false);

    if (acceptThread != nullptr)
    {
        acceptThread->signalThreadShouldExit();

        if (listener != nullptr)
            listener->close();

        acceptThread->stopThread (3000);
        acceptThread.reset();
    }

    listener.reset();

    if (housekeepingThread != nullptr)
    {
        housekeepingThread->stopThread (3000);
        housekeepingThread.reset();
    }

    // Close every connection; their threads leave their sessions on the way out.
    std::vector<std::shared_ptr<Connection>> toClose;

    {
        const juce::ScopedLock scoped (connectionsLock);
        toClose.swap (connections);
    }

    for (auto& c : toClose)
    {
        c->signalThreadShouldExit();
        c->socket->close();
    }

    for (auto& c : toClose)
        c->stopThread (3000);

    toClose.clear();

    if (udpSocket != ~(uintptr_t) 0)
    {
        closesocket ((SOCKET) udpSocket);
        udpSocket = ~(uintptr_t) 0;
    }

    if (udpThread.joinable())
        udpThread.join();

    {
        // The clock and the UDP thread are gone: nothing reads the registry any more.
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        sessions.clear();
        retiredSessions.clear();
        publishRegistry();
    }

    boundUdpPort = 0;

    if (winsockStarted)
    {
        WSACleanup();
        winsockStarted = false;
    }
}

void ServerHost::startInternalClock()
{
    if (clock != nullptr)
        return;

    const double periodUs = 1.0e6 * (double) frameSamples (config.frameSize) / (double) sampleRate;
    clock = std::make_unique<HighResClock> (periodUs, [this] { tick(); });
    clock->start();
}

void ServerHost::tick() noexcept
{
    // Lock-free and allocation-free: loading the snapshot copies a shared_ptr (a reference count), and the
    // snapshot stays valid for the whole tick even if a control thread publishes a new one meanwhile.
    const auto snapshot = registry.load (std::memory_order_acquire);

    if (snapshot == nullptr)
        return;

    for (auto& session : snapshot->sessions)
    {
        // Safety net: a block MixServer skipped because a control mutation (join/leave/gain) was in flight is
        // reproduced on the next tick, so the mix stream keeps one block per period and the clients' jitter
        // buffers do not drain. With no lock on this path it should essentially never trigger.
        const int rounds = 1 + std::min (maxCatchUpTicks, session->missedTicks);
        session->missedTicks = 0;

        for (int round = 0; round < rounds; ++round)
        {
            const auto backoffs = session->mix.getBackoffCount();
            session->mix.tick (session->sender);

            if (session->mix.getBackoffCount() != backoffs)
            {
                session->missedTicks = rounds - round;
                break;
            }
        }
    }
}

void ServerHost::publishRegistry()
{
    auto next = std::make_shared<Registry>();
    next->sessions = sessions;

    for (auto& session : sessions)
        for (auto& endpoint : session->endpoints)
            if (const auto id = endpoint.clientId.load (std::memory_order_acquire); id != 0)
                next->endpoints.emplace (id, Registry::Slot { session.get(), &endpoint });

    registry.store (std::move (next), std::memory_order_release);
}

int ServerHost::getSessionCount() const
{
    const juce::SpinLock::ScopedLockType scoped (sessionsLock);
    return (int) sessions.size();
}

std::vector<juce::String> ServerHost::getSessionIds() const
{
    const juce::SpinLock::ScopedLockType scoped (sessionsLock);
    std::vector<juce::String> ids;

    for (const auto& s : sessions)
        ids.push_back (s->id);

    return ids;
}

std::shared_ptr<ServerHost::Session> ServerHost::findSession (const juce::String& id) const
{
    for (const auto& s : sessions)
        if (s->id == id)
            return s;

    return nullptr;
}

//==============================================================================
void ServerHost::onConnectionAccepted (juce::StreamingSocket* socket)
{
    {
        const juce::ScopedLock scoped (connectionsLock);
        const auto live = (size_t) std::count_if (connections.begin(), connections.end(),
                                                  [] (const auto& c) { return ! c->finished.load (std::memory_order_acquire); });

        if (live >= maxConnections)
        {
            const std::unique_ptr<juce::StreamingSocket> refused (socket);
            log ("conexion rechazada desde " + socket->getHostName() + ": limite de " + juce::String (maxConnections));
            return;
        }
    }

    auto connection = std::make_shared<Connection> (*this, socket);

    {
        const juce::ScopedLock scoped (connectionsLock);
        connections.push_back (connection);
    }

    log ("conexion desde " + socket->getHostName());
    connection->startThread();
}

void ServerHost::housekeeping()
{
    // Reap finished connections.
    std::vector<std::shared_ptr<Connection>> dead;

    {
        const juce::ScopedLock scoped (connectionsLock);

        for (auto it = connections.begin(); it != connections.end();)
        {
            if ((*it)->finished.load (std::memory_order_acquire))
            {
                dead.push_back (*it);
                it = connections.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    for (auto& c : dead)
        c->stopThread (2000);

    dead.clear();

    // Destroy retired sessions once nothing else (a snapshot still held by a tick in flight) owns them.
    std::vector<std::shared_ptr<Session>> expired;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);

        for (auto it = retiredSessions.begin(); it != retiredSessions.end();)
        {
            if (it->use_count() == 1)
            {
                expired.push_back (std::move (*it));
                it = retiredSessions.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    expired.clear();   // Session destructors run here, on the housekeeping thread

    // Periodic stats + transport state.
    std::vector<std::shared_ptr<Session>> current;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        current = sessions;
    }

    for (auto& session : current)
    {
        auto message = makeMessage ("stats");
        juce::Array<juce::var> array;

        for (const auto& s : session->mix.getStats())
        {
            auto* o = new juce::DynamicObject();
            o->setProperty ("id", (juce::int64) s.id);
            o->setProperty ("jitterBlocks", s.jitterBlocks);
            o->setProperty ("loss", (juce::int64) (s.underruns + s.late));
            o->setProperty ("underruns", (juce::int64) s.underruns);
            o->setProperty ("late", (juce::int64) s.late);
            o->setProperty ("voicePeak", s.voicePeak);
            o->setProperty ("instrumentPeak", s.instrumentPeak);
            array.add (juce::var (o));
        }

        message.getDynamicObject()->setProperty ("participants", juce::var (array));
        broadcast (*session, message);
        broadcastTransport (*session);
    }
}

void ServerHost::broadcast (Session& session, const juce::var& message)
{
    std::vector<std::shared_ptr<Connection>> targets;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        targets = session.connections;
    }

    for (auto& c : targets)
        c->send (message);
}

void ServerHost::broadcastParticipants (Session& session)
{
    auto message = makeMessage ("participants");
    message.getDynamicObject()->setProperty ("participants", participantsToVar (session.mix.getParticipants()));
    broadcast (session, message);
}

void ServerHost::broadcastTransport (Session& session)
{
    auto message = transportToVar (session.mix.track().getState());
    message.getDynamicObject()->setProperty ("t", "transport_state");
    broadcast (session, message);
}

//==============================================================================
void ServerHost::handleMessage (Connection& connection, const juce::var& message)
{
    const auto type = message["t"].toString();

    if (type == "ping")
    {
        auto pong = makeMessage ("pong");
        pong.getDynamicObject()->setProperty ("t0", message["t0"]);
        connection.send (pong);
        return;
    }

    if (type == "create")   { handleCreate (connection, message); return; }
    if (type == "join")     { handleJoin (connection, message); return; }

    if (connection.session == nullptr)
    {
        connection.send (errorMessage ("Primero hay que unirse a una sesion"));
        return;
    }

    if (type == "gain")                 { handleGain (connection, message); return; }
    if (type == "transport")            { handleTransport (connection, message); return; }
    if (type == "track_upload_begin")   { handleUpload (connection, message); return; }
    if (type == "jitter")               return;   // informative only

    connection.send (errorMessage ("Mensaje desconocido: " + type));
}

void ServerHost::handleCreate (Connection& connection, const juce::var& message)
{
    if (connection.session != nullptr)
    {
        connection.send (errorMessage ("Ya estas en una sesion"));
        return;
    }

    const auto name = message["name"].toString().trim();
    const auto role = roleFromString (message["role"].toString()).value_or (Role::both);

    std::shared_ptr<Session> session;

    for (int attempt = 0; attempt < 100 && session == nullptr; ++attempt)
    {
        juce::String id;

        {
            const juce::SpinLock::ScopedLockType scoped (sessionsLock);
            id = generateSessionId (random);

            if (findSession (id) != nullptr)
                continue;
        }

        session = std::make_shared<Session> (*this, id, config.frameSize, config.maxParticipantsPerSession);

        {
            const juce::SpinLock::ScopedLockType scoped (sessionsLock);

            if (findSession (id) != nullptr)
            {
                session.reset();
                continue;
            }

            sessions.push_back (session);
            publishRegistry();
        }
    }

    if (session == nullptr)
    {
        connection.send (errorMessage ("No se pudo crear la sesion"));
        return;
    }

    log ("sesion " + session->id + " creada por " + name);
    joinSession (connection, session, name, role);
}

void ServerHost::handleJoin (Connection& connection, const juce::var& message)
{
    if (connection.session != nullptr)
    {
        connection.send (errorMessage ("Ya estas en una sesion"));
        return;
    }

    const auto id = message["session"].toString().trim().toUpperCase();
    const auto name = message["name"].toString().trim();
    const auto role = roleFromString (message["role"].toString()).value_or (Role::both);

    if (! isValidSessionId (id))
    {
        connection.send (errorMessage ("Codigo de sesion invalido"));
        return;
    }

    std::shared_ptr<Session> session;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        session = findSession (id);
    }

    if (session == nullptr)
    {
        connection.send (errorMessage ("Sesion no encontrada: " + id));
        return;
    }

    joinSession (connection, session, name, role);
}

void ServerHost::joinSession (Connection& connection, std::shared_ptr<Session> session, const juce::String& name, Role role)
{
    std::shared_ptr<Connection> self;

    {
        const juce::ScopedLock scoped (connectionsLock);

        for (auto& c : connections)
            if (c.get() == &connection)
                self = c;
    }

    if (self == nullptr)
        return;   // being shut down

    uint32_t clientId = 0;
    juce::var joined;
    bool full = false;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        Session::Endpoint* slot = session->findEndpoint (0);
        full = slot == nullptr || (int) session->connections.size() >= config.maxParticipantsPerSession;

        if (full)
        {
            // An empty session nobody can enter is useless: drop it.
            if (session->connections.empty())
            {
                sessions.erase (std::remove (sessions.begin(), sessions.end(), session), sessions.end());
                retiredSessions.push_back (session);
            }
        }
        else
        {

        // Random ids: the UDP endpoint binds to the first datagram carrying the id, so it must not be guessable.
        clientId = allocateClientId();
        session->mix.addParticipantWithId (clientId, name.isEmpty() ? "Musico " + juce::String (session->connections.size() + 1) : name, role);

        slot->address.store (0);
        slot->lastSeenMs.store (0);
        slot->clientId.store (clientId, std::memory_order_release);

        connection.session = session;
        connection.clientId = clientId;
        connection.name = name;
        session->connections.push_back (self);

        joined = makeMessage ("joined");
        auto* o = joined.getDynamicObject();
        o->setProperty ("session", session->id);
        o->setProperty ("clientId", (juce::int64) clientId);
        o->setProperty ("udpPort", boundUdpPort);
        o->setProperty ("frameSize", frameSamples (config.frameSize));
        o->setProperty ("participants", participantsToVar (session->mix.getParticipants()));
        }

        publishRegistry();
    }

    if (full)
    {
        connection.send (errorMessage ("La sesion esta llena"));
        return;
    }

    connection.send (joined);
    broadcastParticipants (*session);
    broadcastTransport (*session);
    log (name + " (" + juce::String (clientId) + ") entro a " + session->id);
}

bool ServerHost::isClientIdInUse (uint32_t clientId) const noexcept
{
    for (const auto& session : sessions)
        if (session->findEndpoint (clientId) != nullptr)
            return true;

    return false;
}

uint32_t ServerHost::allocateClientId()
{
    // [1, 2^31 - 1]: travels as a JSON number, so it stays within int32. Unique across every session on this host.
    for (;;)
    {
        const auto id = (uint32_t) (1 + random.nextInt (std::numeric_limits<int32_t>::max()));

        if (! isClientIdInUse (id))
            return id;
    }
}

void ServerHost::leaveSession (Connection& connection)
{
    std::shared_ptr<Session> session;
    bool sessionDied = false;

    {
        const juce::SpinLock::ScopedLockType scoped (sessionsLock);
        session = std::move (connection.session);
        connection.session = nullptr;

        if (session == nullptr)
            return;

        if (auto* slot = session->findEndpoint (connection.clientId))
        {
            slot->clientId.store (0, std::memory_order_release);
            slot->address.store (0);
        }

        session->mix.removeParticipant (connection.clientId);
        session->connections.erase (std::remove_if (session->connections.begin(), session->connections.end(),
                                                    [&] (const auto& c) { return c.get() == &connection; }),
                                    session->connections.end());

        if (session->connections.empty())
        {
            sessions.erase (std::remove (sessions.begin(), sessions.end(), session), sessions.end());
            retiredSessions.push_back (session);
            sessionDied = true;
        }

        publishRegistry();
    }

    log (connection.name + " (" + juce::String (connection.clientId) + ") salio de " + session->id);

    if (sessionDied)
        log ("sesion " + session->id + " cerrada");
    else
        broadcastParticipants (*session);

    connection.clientId = 0;
}

void ServerHost::handleGain (Connection& connection, const juce::var& message)
{
    const auto& target = message["target"];
    uint32_t targetId = 0;

    if (target.isString())
    {
        if (target.toString() != "track")
        {
            connection.send (errorMessage ("Destino de ganancia invalido"));
            return;
        }
    }
    else
    {
        targetId = (uint32_t) (juce::int64) target;
    }

    ChannelGain gain;
    gain.voiceDb = message.hasProperty ("voice") ? (float) (double) message["voice"] : 0.0f;
    gain.instrumentDb = message.hasProperty ("instrument") ? (float) (double) message["instrument"] : 0.0f;
    gain.muted = (bool) message["mute"];
    connection.session->mix.setGain (connection.clientId, targetId, gain);
}

void ServerHost::handleTransport (Connection& connection, const juce::var& message)
{
    auto& track = connection.session->mix.track();
    const auto cmd = message["cmd"].toString();

    if (cmd == "play")       track.play();
    else if (cmd == "pause") track.pause();
    else if (cmd == "stop")  track.stop();
    else if (cmd == "seek")  track.seek ((int64_t) (juce::int64) message["position"]);
    else if (cmd == "loop")  track.setLoop ((bool) message["loop"]);
    else
    {
        connection.send (errorMessage ("Comando de transporte desconocido: " + cmd));
        return;
    }

    broadcastTransport (*connection.session);
}

void ServerHost::handleUpload (Connection& connection, const juce::var& message)
{
    const auto name = sanitizeFileName (message["name"].toString());
    const auto size = (int64_t) (juce::int64) message["size"];
    const auto expectedSha1 = message["sha1"].toString().trim().toLowerCase();
    auto session = connection.session;

    if (size <= 0 || size > maxUploadBytes)
    {
        connection.send (errorMessage ("Tamano de archivo invalido"));
        return;
    }

    // One folder per session so the loaded track keeps the client's file name.
    const auto folder = config.uploadDirectory.getChildFile (session->id);
    folder.createDirectory();
    const auto file = folder.getChildFile (name);
    file.deleteFile();

    bool received = false;

    {
        juce::FileOutputStream out (file);

        if (out.failedToOpen())
        {
            connection.send (errorMessage ("No se pudo guardar el archivo"));
            return;
        }

        received = connection.readExactly (out, size);
        out.flush();
    }

    if (! received)
    {
        file.deleteFile();
        connection.send (errorMessage ("La subida se interrumpio"));
        return;
    }

    {
        juce::FileInputStream in (file);
        const auto actual = in.openedOk() ? Sha1::of (in) : juce::String();

        if (actual != expectedSha1)
        {
            file.deleteFile();
            connection.send (errorMessage ("El archivo llego corrupto (SHA-1 no coincide)"));
            return;
        }
    }

    const auto result = session->mix.track().load (file);

    if (result.failed())
    {
        file.deleteFile();
        connection.send (errorMessage (result.getErrorMessage()));
        return;
    }

    const auto state = session->mix.track().getState();
    auto ready = makeMessage ("track_ready");
    ready.getDynamicObject()->setProperty ("name", state.name);
    ready.getDynamicObject()->setProperty ("lengthSamples", (juce::int64) state.length);
    broadcast (*session, ready);
    broadcastTransport (*session);
    log ("pista " + state.name + " cargada en " + session->id);
}

//==============================================================================
void ServerHost::udpLoop()
{
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);   // failure is harmless

    uint8_t buffer[2048];

    while (running.load (std::memory_order_acquire))
    {
        sockaddr_in from {};
        int fromLen = sizeof (from);
        const int n = recvfrom ((SOCKET) udpSocket, (char*) buffer, sizeof (buffer), 0, (sockaddr*) &from, &fromLen);

        if (n < 0)
        {
            const int error = WSAGetLastError();

            if (error == WSAEINTR || error == WSAENOTSOCK || error == WSAEBADF || error == WSAESHUTDOWN
                 || ! running.load (std::memory_order_acquire))
                break;

            continue;   // e.g. WSAEMSGSIZE, WSAECONNRESET
        }

        AudioPacketView view;

        if (! decodeAudioPacket (buffer, (size_t) n, view) || (view.flags & AudioFlags::mix) != 0 || view.clientId == 0)
            continue;

        const auto packed = packAddress (from);
        const auto now = nowMs();
        const double arrivalMs = juce::Time::getMillisecondCounterHiRes();

        // Lock-free lookup in the current snapshot: never contends with tick() or the control threads.
        const auto snapshot = registry.load (std::memory_order_acquire);

        if (snapshot == nullptr)
            continue;

        const auto it = snapshot->endpoints.find (view.clientId);

        if (it == snapshot->endpoints.end())
            continue;

        auto* endpoint = it->second.endpoint;

        // The slot may have been handed to someone else since this snapshot was built.
        if (endpoint->clientId.load (std::memory_order_acquire) != view.clientId)
            continue;

        const auto known = endpoint->address.load (std::memory_order_acquire);

        if (known != 0 && known != packed)
        {
            if (now - endpoint->lastSeenMs.load (std::memory_order_relaxed) < endpointSilenceMs)
                continue;   // someone else is still sending from the recorded address

            endpoint->address.store (packed, std::memory_order_release);
        }
        else if (known == 0)
        {
            endpoint->address.store (packed, std::memory_order_release);
        }

        endpoint->lastSeenMs.store (now, std::memory_order_relaxed);
        it->second.session->mix.pushAudio (view, arrivalMs);
    }

    if (mmcss != nullptr)
        AvRevertMmThreadCharacteristics (mmcss);
}

void ServerHost::sendMix (Session& session, uint32_t clientId, const uint8_t* data, size_t len) noexcept
{
    auto* endpoint = session.findEndpoint (clientId);

    if (endpoint == nullptr || udpSocket == ~(uintptr_t) 0)
        return;

    const auto packed = endpoint->address.load (std::memory_order_acquire);

    if (packed == 0)
        return;

    const auto address = unpackAddress (packed);

    // Accepted v1 trade-off (spec section 3.4): the tick runs on the host's audio callback in the embedded server and
    // the mix leaves through sendto() right here, i.e. a syscall on the audio thread. On a bound UDP socket with a large
    // SO_SNDBUF the call never blocks (datagrams are dropped, not queued, when the buffer is full), and it saves one
    // thread hop (~one scheduler quantum of jitter) per block. Moving it behind an SPSC queue is the v2 option.
    sendto ((SOCKET) udpSocket, (const char*) data, (int) len, 0, (const sockaddr*) &address, sizeof (address));
}
}
