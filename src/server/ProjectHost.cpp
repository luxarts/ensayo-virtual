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
#include <bcrypt.h>

#include "ProjectHost.h"
#include "ProjectHostDetail.h"
#include "core/Segments.h"
#include "core/Sha1.h"

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ensayo
{
namespace
{
    constexpr size_t maxLineBytes = 8 * 1024;                // every client line after the first
    constexpr size_t maxFirstLineBytes = 4 * 1024 * 1024;    // hello / data_hello (its "have" map grows with the project)
    constexpr int64_t maxSegmentBytes = 2 * 1024 * 1024;
    constexpr size_t maxQueuedBytes = 64 * 1024 * 1024;      // a peer that falls this far behind is dropped
    constexpr int64_t maxWriteStallMs = 2000;                // a peer that does not drain its socket for this long is dropped
    constexpr int maxNestingDepth = 32;                      // juce::JSON::parse recurses per level
    constexpr size_t maxConnections = 64;                    // live identified TCP connections (control + data)
    constexpr size_t maxUnidentified = 16;                   // connections still waiting for hello / data_hello
    constexpr int64_t firstLineDeadlineMs = 5000;            // no complete hello / data_hello by then: closed
    constexpr int64_t endpointSilenceMs = 2000;
    constexpr uint8_t keepaliveByte = 0x00;
    constexpr int keepaliveBytes = 5;                        // 0x00 + clientId u32 little-endian

    // Linear scan of the bracket/brace nesting (ignoring characters inside strings).
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
                inString = true;
            else if ((c == '[' || c == '{') && ++depth > maxDepth)
                return true;
            else if (c == ']' || c == '}')
                --depth;
        }

        return false;
    }

    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();

        for (const auto& [key, value] : fields)
            o->setProperty (key, value);

        return juce::var (o);
    }

    std::string lineOf (const juce::var& message)
    {
        return makeLine (message).toStdString();
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

    // Cryptographically secure bytes (tokens and clientIds identify a peer on the data connection and over UDP).
    void secureRandom (void* data, size_t size)
    {
        if (! BCRYPT_SUCCESS (BCryptGenRandom (nullptr, static_cast<PUCHAR> (data), (ULONG) size, BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
            juce::Random::getSystemRandom().fillBitsRandomly (data, size);   // never expected; better than zeros
    }

    juce::String randomToken()
    {
        uint8_t bytes[8];
        secureRandom (bytes, sizeof (bytes));
        return juce::String::toHexString (bytes, (int) sizeof (bytes), 0).toLowerCase();   // 16 hex chars
    }

    uint32_t randomClientId()
    {
        uint32_t id = 0;

        while (id == 0)
        {
            secureRandom (&id, sizeof (id));
            id &= 0x7fffffffu;
        }

        return id;
    }

    bool peerIsLoopback (SOCKET handle) noexcept
    {
        sockaddr_storage address {};
        int len = sizeof (address);

        if (getpeername (handle, (sockaddr*) &address, &len) != 0)
            return false;

        if (address.ss_family == AF_INET)
            return (ntohl (((const sockaddr_in*) &address)->sin_addr.s_addr) >> 24) == 127;

        if (address.ss_family == AF_INET6)
        {
            const auto& in6 = ((const sockaddr_in6*) &address)->sin6_addr;
            return IN6_IS_ADDR_LOOPBACK (&in6) || (IN6_IS_ADDR_V4MAPPED (&in6) && in6.s6_addr[12] == 127);
        }

        return false;
    }

    juce::Result writeFileAtomically (const juce::File& target, const void* data, size_t size)
    {
        if (! target.getParentDirectory().createDirectory())
            return juce::Result::fail ("No se pudo crear la carpeta " + target.getParentDirectory().getFullPathName());

        juce::TemporaryFile temp (target);

        {
            juce::FileOutputStream out (temp.getFile());

            if (out.failedToOpen())
                return juce::Result::fail ("No se pudo escribir " + temp.getFile().getFullPathName());

            if (! out.write (data, size))
                return juce::Result::fail ("No se pudo escribir " + temp.getFile().getFullPathName());

            out.flush();

            if (out.getStatus().failed())
                return out.getStatus();
        }

        if (! temp.overwriteTargetFileWithTemporary())
            return juce::Result::fail ("No se pudo reemplazar " + target.getFullPathName());

        return juce::Result::ok();
    }
}

//==============================================================================
namespace hostdetail
{
bool mayClaimUser (const Id& user, const Id& hostUser, bool fromLoopback) noexcept
{
    return user != hostUser || fromLoopback;
}

bool writeWithStallTimeout (const std::function<int (const char*, size_t)>& sendSome, const char* data, size_t len,
                            int64_t stallMs, const std::function<bool()>& shouldAbort)
{
    auto deadline = nowMs() + stallMs;

    while (len > 0)
    {
        const int n = sendSome (data, std::min<size_t> (len, 1u << 20));

        if (n > 0)
        {
            data += n;
            len -= (size_t) n;
            deadline = nowMs() + stallMs;   // progress: the stall timer starts over
            continue;
        }

        if (n == 0 && nowMs() < deadline && ! (shouldAbort && shouldAbort()))
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
            continue;
        }

        return false;
    }

    return true;
}

std::optional<int64_t> readInteger (const juce::var& v, int64_t min, int64_t max)
{
    int64_t value = 0;

    if (v.isInt() || v.isInt64())
    {
        value = (juce::int64) v;
    }
    else if (v.isDouble())
    {
        const double d = v;

        if (! std::isfinite (d) || d != std::floor (d) || std::abs (d) > 9007199254740992.0)
            return std::nullopt;

        value = (int64_t) d;
    }
    else
    {
        return std::nullopt;
    }

    if (value < min || value > max)
        return std::nullopt;

    return value;
}
}

//==============================================================================
juce::File defaultProjectsDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("EnsayoVirtual").getChildFile ("projects");
}

juce::Array<juce::File> listProjects (const juce::File& projectsDirectory)
{
    juce::Array<juce::File> found;

    for (const auto& dir : projectsDirectory.findChildFiles (juce::File::findDirectories, false))
        if (dir.getChildFile ("project.json").existsAsFile())
            found.add (dir);

    std::stable_sort (found.begin(), found.end(), [] (const juce::File& a, const juce::File& b)
    {
        return a.getChildFile ("project.json").getLastModificationTime()
             > b.getChildFile ("project.json").getLastModificationTime();
    });

    return found;
}

juce::Result saveProjectAtomically (const Project& project, const juce::File& projectJson)
{
    const auto json = toJson (project);
    return writeFileAtomically (projectJson, json.toRawUTF8(), json.getNumBytesAsUTF8());
}

//==============================================================================
struct ProjectHost::Impl
{
    //==========================================================================
    // One TCP connection (control or data; decided by its first line). A reader thread parses what the peer sends;
    // a writer thread drains a FIFO queue, so everything enqueued under stateLock reaches the peer in apply order.
    struct Connection
    {
        enum class Kind { unidentified, control, data };

        struct Recording { Id take, track; int64_t start = 0; };

        struct Item
        {
            std::string line;                                   // empty for a catch-up item
            std::shared_ptr<const juce::MemoryBlock> payload;   // raw bytes after the line (segments)
            Id catchUpTake;                                     // catch-up: stream stored segments [from, to)
            int from = 0, to = 0, channels = 1;
            bool closeAfter = false;                            // close the connection once this is written
        };

        Connection (Impl& h, juce::StreamingSocket* s) : host (h), socket (s)
        {
            // Non-blocking from the start so writeAll() never parks inside ::send.
            u_long nonBlocking = 1;
            ioctlsocket ((SOCKET) socket->getRawSocketHandle(), (long) FIONBIO, &nonBlocking);
            fromLoopback = peerIsLoopback ((SOCKET) socket->getRawSocketHandle());
        }

        ~Connection()
        {
            abort();
            join();
            socket->close();
        }

        void start()
        {
            reader = std::thread ([this] { readerLoop(); });
            writer = std::thread ([this] { writerLoop(); });
        }

        void join()
        {
            if (reader.joinable())  reader.join();
            if (writer.joinable())  writer.join();
        }

        bool isFinished() const noexcept
        {
            return readerDone.load (std::memory_order_acquire) && writerDone.load (std::memory_order_acquire);
        }

        // Immediate shutdown (host stopping): pending writes are discarded.
        void abort()
        {
            exiting.store (true, std::memory_order_release);

            {
                const std::lock_guard<std::mutex> scoped (queueLock);
                aborted = true;
            }

            queueSignal.notify_all();
            markDead();
        }

        // Shuts the socket down: the reader wakes up with EOF, later writes fail. The handle is closed in the destructor.
        void markDead()
        {
            if (! dead.exchange (true, std::memory_order_acq_rel))
                ::shutdown ((SOCKET) socket->getRawSocketHandle(), SD_BOTH);

            queueSignal.notify_all();
        }

        void enqueue (Item item)
        {
            const auto size = item.line.size() + (item.payload != nullptr ? item.payload->getSize() : 0);
            bool overflow = false;

            {
                const std::lock_guard<std::mutex> scoped (queueLock);

                if (aborted || draining || dead.load (std::memory_order_acquire))
                    return;

                if (queuedBytes + size > maxQueuedBytes)
                {
                    overflow = true;
                }
                else
                {
                    queuedBytes += size;
                    queue.push_back (std::move (item));
                }
            }

            if (overflow)
            {
                host.log ("conexion descartada: no consume lo que se le envia");
                markDead();
                return;
            }

            queueSignal.notify_one();
        }

        void send (const std::string& line, std::shared_ptr<const juce::MemoryBlock> payload = nullptr)
        {
            Item item;
            item.line = line;
            item.payload = std::move (payload);
            enqueue (std::move (item));
        }

        void send (const juce::var& message)   { send (lineOf (message)); }

        void sendAndClose (const juce::var& message)
        {
            Item item;
            item.line = lineOf (message);
            item.closeAfter = true;
            enqueue (std::move (item));
        }

        //======================================================================
        void writerLoop()
        {
            for (;;)
            {
                Item item;

                {
                    std::unique_lock<std::mutex> scoped (queueLock);
                    queueSignal.wait (scoped, [this]
                    {
                        return aborted || draining || ! queue.empty() || dead.load (std::memory_order_acquire);
                    });

                    if (aborted || dead.load (std::memory_order_acquire) || queue.empty())
                        break;   // aborted, dead, or draining with nothing left

                    item = std::move (queue.front());
                    queue.pop_front();
                    queuedBytes -= item.line.size() + (item.payload != nullptr ? item.payload->getSize() : 0);
                }

                if (item.catchUpTake.isNotEmpty())
                {
                    for (int index = item.from; index < item.to && ! dead.load (std::memory_order_acquire); ++index)
                        if (! sendStoredSegment (item.catchUpTake, index, item.channels))
                            break;
                }
                else
                {
                    if (writeAll (item.line.data(), item.line.size()) && item.payload != nullptr)
                        writeAll (static_cast<const char*> (item.payload->getData()), item.payload->getSize());

                    if (item.closeAfter)
                        break;
                }
            }

            markDead();
            writerDone.store (true, std::memory_order_release);
        }

        // Catch-up: reads takes/<take>/<index>.flac and sends it like a live segment. False when it cannot be read.
        bool sendStoredSegment (const Id& take, int index, int channels)
        {
            const auto file = host.segmentFile (take, index);
            juce::MemoryBlock bytes;

            if (! file.loadFileAsData (bytes) || bytes.getSize() == 0)
            {
                host.log ("no se pudo leer " + file.getFullPathName());
                return false;
            }

            int64_t samples = 0;

            {
                juce::FlacAudioFormat flac;
                std::unique_ptr<juce::AudioFormatReader> flacReader (
                    flac.createReaderFor (new juce::MemoryInputStream (bytes, false), true));

                if (flacReader == nullptr)
                {
                    host.log ("segmento ilegible " + file.getFullPathName());
                    return false;
                }

                samples = flacReader->lengthInSamples;
            }

            const auto line = lineOf (object ({ { "t", "segment" }, { "take", take }, { "index", index },
                                                { "samples", (juce::int64) samples }, { "channels", channels },
                                                { "bytes", (juce::int64) bytes.getSize() },
                                                { "sha1", Sha1::of (bytes.getData(), bytes.getSize()) } }));

            return writeAll (line.data(), line.size())
                && writeAll (static_cast<const char*> (bytes.getData()), bytes.getSize());
        }

        // Writes the whole buffer; a peer that accepts no byte for maxWriteStallMs in a row is marked dead.
        // Writer thread only.
        bool writeAll (const char* data, size_t len)
        {
            if (dead.load (std::memory_order_acquire))
                return false;

            const auto handle = (SOCKET) socket->getRawSocketHandle();

            auto sendSome = [handle] (const char* bytes, size_t count)
            {
                const int n = ::send (handle, bytes, (int) count, 0);

                if (n > 0)
                    return n;

                return (n < 0 && WSAGetLastError() == WSAEWOULDBLOCK) ? 0 : -1;
            };

            auto shouldAbort = [this] { return exiting.load (std::memory_order_acquire); };

            if (hostdetail::writeWithStallTimeout (sendSome, data, len, maxWriteStallMs, shouldAbort))
                return true;

            markDead();
            return false;
        }

        //======================================================================
        void readerLoop()
        {
            while (! exiting.load (std::memory_order_acquire))
            {
                if (kindForReader == Kind::unidentified && nowMs() - acceptedMs > firstLineDeadlineMs)
                {
                    host.log ("conexion cerrada: no envio hello a tiempo");
                    break;
                }

                const int ready = socket->waitUntilReady (true, 100);

                if (ready < 0)
                    break;

                if (ready == 0)
                    continue;

                char buffer[16384];
                const int n = socket->read (buffer, sizeof (buffer), false);

                if (n <= 0)
                    break;

                pending.insert (pending.end(), buffer, buffer + n);

                if (! processPendingLines())
                    break;
            }

            host.onClosed (*this);

            {
                // Let the writer flush what is queued (e.g. a final error) and then close.
                const std::lock_guard<std::mutex> scoped (queueLock);
                draining = true;
            }

            queueSignal.notify_all();
            readerDone.store (true, std::memory_order_release);
        }

        // Handles every complete line in `pending`. False when the connection must be dropped.
        bool processPendingLines()
        {
            for (;;)
            {
                const auto cap = kindForReader == Kind::unidentified ? maxFirstLineBytes : maxLineBytes;
                const auto newline = std::find (pending.begin(), pending.end(), '\n');

                if (newline == pending.end())
                    return pending.size() <= cap;

                const auto lineLen = (size_t) (newline - pending.begin());

                if (lineLen > cap || nestsDeeperThan (pending.data(), lineLen, maxNestingDepth))
                {
                    sendAndClose (object ({ { "t", "error" }, { "message", "mensaje demasiado grande" } }));
                    return false;
                }

                const auto line = juce::String::fromUTF8 (pending.data(), (int) lineLen);
                pending.erase (pending.begin(), newline + 1);

                const auto message = parseLine (line);

                if (! message)
                {
                    sendAndClose (object ({ { "t", "error" }, { "message", juce::String (juce::CharPointer_UTF8 ("mensaje inv\xc3\xa1lido")) } }));
                    return false;
                }

                if (! host.handleMessage (*this, *message))
                    return false;

                if (exiting.load (std::memory_order_acquire) || dead.load (std::memory_order_acquire))
                    return false;
            }
        }

        // Reads exactly `count` raw bytes after a line. Drains `pending` first.
        bool readExactly (juce::MemoryBlock& out, size_t count)
        {
            out.setSize (count, false);
            auto* dest = static_cast<char*> (out.getData());
            const auto fromPending = std::min (count, pending.size());
            std::memcpy (dest, pending.data(), fromPending);
            pending.erase (pending.begin(), pending.begin() + (std::ptrdiff_t) fromPending);
            size_t got = fromPending;

            while (got < count && ! exiting.load (std::memory_order_acquire))
            {
                const int ready = socket->waitUntilReady (true, 100);

                if (ready < 0)
                    return false;

                if (ready == 0)
                    continue;

                const int n = socket->read (dest + got, (int) std::min<size_t> (count - got, 1u << 20), false);

                if (n <= 0)
                    return false;

                got += (size_t) n;
            }

            return got == count;
        }

        Impl& host;
        std::unique_ptr<juce::StreamingSocket> socket;
        std::thread reader, writer;

        std::mutex queueLock;                  // leaf lock
        std::condition_variable queueSignal;
        std::deque<Item> queue;
        size_t queuedBytes = 0;
        bool aborted = false, draining = false;

        std::atomic<bool> exiting { false }, dead { false }, readerDone { false }, writerDone { false };
        std::atomic<bool> identified { false };   // hello / data_hello accepted (counted against maxConnections)
        const int64_t acceptedMs = nowMs();
        bool fromLoopback = false;                // peer address is 127.0.0.0/8 or ::1; fixed after construction
        std::vector<char> pending;             // reader thread only
        Kind kindForReader = Kind::unidentified;   // reader thread's own copy of `kind`

        // Identity: written once by the reader thread under stateLock; read by others under stateLock.
        Kind kind = Kind::unidentified;
        Id user;
        juce::String name, token;
        uint32_t clientId = 0;
        std::optional<Recording> recording;   // control only, under stateLock

        // UDP (read lock-free by the UDP thread through udpPeers).
        std::atomic<bool> listening { false };
        std::atomic<uint64_t> udpAddress { 0 };     // ip << 16 | port, 0 = unknown
        std::atomic<int64_t> udpLastSeenMs { 0 };
    };

    using ConnectionPtr = std::shared_ptr<Connection>;
    using PeerList = std::vector<ConnectionPtr>;

    //==========================================================================
    Impl (ProjectHost& o, HostConfig c) : owner (o), config (c)
    {
        config.maxClients = juce::jmax (1, config.maxClients);
    }

    void log (const juce::String& text)
    {
        if (owner.onLog)
            owner.onLog (text);
    }

    juce::File segmentFile (const Id& take, int index) const
    {
        return projectDir.getChildFile ("takes").getChildFile (take).getChildFile (juce::String (index) + ".flac");
    }

    //==========================================================================
    juce::Result start (const juce::File& dir, const Id& hostUserId, const juce::String& hostName, const juce::String& newProjectName)
    {
        if (running.load())
            return juce::Result::fail (juce::String (juce::CharPointer_UTF8 ("el anfitri\xc3\xb3n ya est\xc3\xa1 iniciado")));

        if (! isValidId (hostUserId))
            return juce::Result::fail (juce::String (juce::CharPointer_UTF8 ("usuario anfitri\xc3\xb3n inv\xc3\xa1lido")));

        if (! dir.createDirectory())
            return juce::Result::fail ("No se pudo crear la carpeta " + dir.getFullPathName());

        projectDir = dir;
        projectFile = dir.getChildFile ("project.json");
        hostUser = hostUserId;
        const auto name = hostName.trim().isNotEmpty() ? hostName.trim() : juce::String (juce::CharPointer_UTF8 ("Anfitri\xc3\xb3n"));

        Project p;

        if (projectFile.existsAsFile())
        {
            juce::String error;
            auto loaded = projectFromJson (projectFile.loadFileAsString(), error);

            if (! loaded)
                return juce::Result::fail (juce::String (juce::CharPointer_UTF8 ("project.json inv\xc3\xa1lido: ")) + error);

            if (loaded->hostUser != hostUserId)
                return juce::Result::fail (juce::String (juce::CharPointer_UTF8 ("este proyecto pertenece a otro anfitri\xc3\xb3n")));

            p = std::move (*loaded);

            if (p.userName (hostUserId) != name)
            {
                const auto result = applyOp (p, Ops::addUser (hostUserId, name), hostUserId);

                if (! result.ok)
                    log ("no se pudo registrar al anfitrion: " + result.error);
                else if (const auto saved = saveProjectAtomically (p, projectFile); saved.failed())
                    return saved;
            }
        }
        else
        {
            const auto projectName = newProjectName.trim().isNotEmpty() ? newProjectName.trim() : juce::String ("Proyecto");
            p = makeProject (projectName, hostUserId, name);

            if (const auto saved = saveProjectAtomically (p, projectFile); saved.failed())
                return saved;
        }

        project.store (std::make_shared<const Project> (std::move (p)));
        sessionId = generateSessionId (juce::Random::getSystemRandom());

        WSADATA wsaData;

        if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
            return juce::Result::fail ("WSAStartup fallo");

        winsockStarted = true;

        // TCP listener (control + data).
        listener = std::make_unique<juce::StreamingSocket>();

        if (! listener->createListener (config.port))
        {
            stop();
            return juce::Result::fail ("No se pudo abrir el puerto TCP " + juce::String (config.port));
        }

        boundPort = listener->getBoundPort();

        // UDP preview relay on the same port number.
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
        local.sin_port = htons ((u_short) boundPort);

        if (::bind (udp, (const sockaddr*) &local, sizeof (local)) != 0)
        {
            closesocket (udp);
            stop();
            return juce::Result::fail ("No se pudo abrir el puerto UDP " + juce::String (boundPort));
        }

        udpSocket = (uintptr_t) udp;
        udpPeers.store (std::make_shared<const PeerList>());
        running.store (true);

        acceptThread = std::thread ([this] { acceptLoop(); });
        udpThread = std::thread ([this] { udpLoop(); });

        log ("anfitrion iniciado: sesion " + sessionId + " puerto " + juce::String (boundPort) + " proyecto "
             + projectFile.getFullPathName());
        return juce::Result::ok();
    }

    void stop()
    {
        running.store (false);

        if (listener != nullptr)
            listener->close();

        if (acceptThread.joinable())
            acceptThread.join();

        listener.reset();

        std::vector<ConnectionPtr> toClose;

        {
            const std::lock_guard<std::mutex> scoped (connectionsLock);
            toClose.swap (connections);
        }

        for (auto& c : toClose)
            c->abort();

        for (auto& c : toClose)
            c->join();

        toClose.clear();

        {
            const std::lock_guard<std::mutex> scoped (stateLock);
            controls.clear();
            datas.clear();
            udpPeers.store (std::make_shared<const PeerList>());
        }

        if (udpSocket != ~(uintptr_t) 0)
        {
            closesocket ((SOCKET) udpSocket);
            udpSocket = ~(uintptr_t) 0;
        }

        if (udpThread.joinable())
            udpThread.join();

        boundPort = 0;

        if (winsockStarted)
        {
            WSACleanup();
            winsockStarted = false;
        }
    }

    //==========================================================================
    void acceptLoop()
    {
        while (running.load (std::memory_order_acquire))
        {
            auto* socket = listener->waitForNextConnection();

            if (socket == nullptr)
            {
                if (! running.load (std::memory_order_acquire))
                    break;

                std::this_thread::sleep_for (std::chrono::milliseconds (50));
                continue;
            }

            if (! running.load (std::memory_order_acquire))
            {
                delete socket;
                break;
            }

            std::vector<ConnectionPtr> finished;
            ConnectionPtr connection;

            {
                const std::lock_guard<std::mutex> scoped (connectionsLock);

                for (auto it = connections.begin(); it != connections.end();)
                {
                    if ((*it)->isFinished())
                    {
                        finished.push_back (*it);
                        it = connections.erase (it);
                    }
                    else
                    {
                        ++it;
                    }
                }

                size_t identifiedCount = 0, unidentifiedCount = 0;
                Connection* oldestUnidentified = nullptr;

                for (auto& c : connections)
                {
                    if (c->dead.load (std::memory_order_acquire))
                        continue;

                    if (c->identified.load (std::memory_order_acquire))
                    {
                        ++identifiedCount;
                    }
                    else
                    {
                        ++unidentifiedCount;

                        if (oldestUnidentified == nullptr)
                            oldestUnidentified = c.get();   // the list is in accept order
                    }
                }

                // Silent sockets cannot lock real joiners out: past the cap the oldest silent one makes room.
                if (unidentifiedCount >= maxUnidentified && oldestUnidentified != nullptr)
                    oldestUnidentified->markDead();

                if (identifiedCount < maxConnections)
                {
                    connection = std::make_shared<Connection> (*this, socket);
                    connections.push_back (connection);
                }
            }

            finished.clear();   // joins their (already finished) threads

            if (connection == nullptr)
            {
                log ("conexion rechazada: limite de " + juce::String ((int) maxConnections));
                delete socket;
                continue;
            }

            connection->start();
        }
    }

    //==========================================================================
    // Reader threads. False drops the connection (after flushing what is queued).
    bool handleMessage (Connection& c, const juce::var& message)
    {
        const auto type = message["t"].toString();

        if (c.kindForReader == Connection::Kind::unidentified)
        {
            if (type == "hello")       return handleHello (c, message);
            if (type == "data_hello")  return handleDataHello (c, message);

            c.sendAndClose (object ({ { "t", "error" }, { "message", "se esperaba hello" } }));
            return false;
        }

        if (c.kindForReader == Connection::Kind::data)
        {
            if (type == "segment")
                return handleSegment (c, message);

            return true;   // unknown data messages are ignored
        }

        if (type == "op")             handleOp (c, message);
        else if (type == "rec_start") handleRecStart (c, message);
        else if (type == "rec_peaks") handleRecPeaks (c, message);
        else if (type == "rec_stop")  handleRecStop (c, message);
        else if (type == "take_complete") handleTakeComplete (c, message);
        else if (type == "listen")    c.listening.store ((bool) message["enabled"], std::memory_order_release);
        else if (type == "ping")      c.send (object ({ { "t", "pong" }, { "ms", message["ms"] } }));
        else if (type == "resync")
        {
            const std::lock_guard<std::mutex> scoped (stateLock);
            c.send (object ({ { "t", "project" }, { "project", toVar (*project.load()) } }));
        }

        return true;
    }

    bool refuse (Connection& c, const juce::String& text)
    {
        c.sendAndClose (object ({ { "t", "error" }, { "message", text } }));
        return false;
    }

    bool handleHello (Connection& c, const juce::var& message)
    {
        if (hostdetail::readInteger (message["protocol"], INT_MIN, INT_MAX) != (int64_t) protocolVersionV2)
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("versi\xc3\xb3n incompatible")));

        if (message["session"].toString() != sessionId)
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("sesi\xc3\xb3n no encontrada")));

        const auto user = message["user"].toString();
        const auto name = message["name"].toString().trim();

        if (! isValidId (user) || name.isEmpty())
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("datos de usuario inv\xc3\xa1lidos")));

        if (! hostdetail::mayClaimUser (user, hostUser, c.fromLoopback))
        {
            log ("hello rechazado: el usuario anfitrion solo puede conectarse desde esta maquina");
            return refuse (c, "usuario reservado");
        }

        const std::lock_guard<std::mutex> scoped (stateLock);

        // A user that reconnects replaces its previous (probably half-dead) control connection.
        for (auto it = controls.begin(); it != controls.end(); ++it)
        {
            if ((*it)->user == user)
            {
                log ("reconexion de " + name + ": se cierra la conexion anterior");
                (*it)->markDead();
                closeDataConnectionsOf ((*it)->clientId);
                controls.erase (it);
                publishUdpPeers();   // even if this join fails below, the old endpoint must stop receiving
                break;
            }
        }

        if ((int) controls.size() >= config.maxClients)
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("sesi\xc3\xb3n llena")));

        auto self = selfOf (c);

        if (self == nullptr)
            return false;   // the host is stopping

        if (project.load()->userName (user) != name)
        {
            const auto result = applyLocked (Ops::addUser (user, name), hostUser);

            if (! result.ok)
                return refuse (c, juce::String (juce::CharPointer_UTF8 ("nombre inv\xc3\xa1lido")));
        }

        uint32_t clientId = randomClientId();

        while (std::any_of (controls.begin(), controls.end(), [clientId] (const ConnectionPtr& o) { return o->clientId == clientId; }))
            clientId = randomClientId();

        c.kind = Connection::Kind::control;
        c.kindForReader = Connection::Kind::control;
        c.user = user;
        c.name = name;
        c.clientId = clientId;
        c.token = randomToken();
        c.identified.store (true, std::memory_order_release);

        c.send (object ({ { "t", "welcome" }, { "clientId", (int) clientId }, { "token", c.token },
                          { "udpPort", boundPort }, { "isHost", user == hostUser },
                          { "project", toVar (*project.load()) } }));

        controls.push_back (std::move (self));
        publishUdpPeers();
        broadcastPresence();
        log (name + " se unio (clientId " + juce::String ((int) clientId) + ")");
        return true;
    }

    bool handleDataHello (Connection& c, const juce::var& message)
    {
        const auto clientIdValue = hostdetail::readInteger (message["clientId"], 1, 0x7fffffff);
        const auto clientId = clientIdValue ? (uint32_t) *clientIdValue : 0u;
        const auto token = message["token"].toString();
        const auto& have = message["have"];

        const std::lock_guard<std::mutex> scoped (stateLock);

        ConnectionPtr control;

        for (auto& o : controls)
            if (clientId != 0 && o->clientId == clientId && token.isNotEmpty() && o->token == token)
                control = o;

        if (control == nullptr)
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("conexi\xc3\xb3n de datos inv\xc3\xa1lida")));

        auto self = selfOf (c);

        if (self == nullptr)
            return false;   // the host is stopping

        c.kind = Connection::Kind::data;
        c.kindForReader = Connection::Kind::data;
        c.user = control->user;
        c.name = control->name;
        c.clientId = clientId;
        c.identified.store (true, std::memory_order_release);

        // Catch-up of everything stored so far; segments stored later are pushed live after these items (same FIFO).
        const auto p = project.load();

        for (const auto& take : p->takes)
        {
            int from = 0;

            // A missing or out-of-range count is treated as "nothing yet".
            if (auto* o = have.getDynamicObject(); o != nullptr)
                if (const auto count = hostdetail::readInteger (o->getProperty (take.id), 0, INT_MAX))
                    from = (int) std::min<int64_t> (*count, take.segments);

            if (from < take.segments)
            {
                Connection::Item item;
                item.catchUpTake = take.id;
                item.from = from;
                item.to = take.segments;
                item.channels = take.channels;
                c.enqueue (std::move (item));
            }
        }

        datas.push_back (std::move (self));
        return true;
    }

    //==========================================================================
    void handleOp (Connection& c, const juce::var& message)
    {
        const auto& op = message["op"];
        const auto name = op["op"].toString();

        auto reject = [&c, &message] (const juce::String& reason)
        {
            c.send (object ({ { "t", "rejected" }, { "seq", message["seq"] }, { "reason", reason } }));
        };

        if (name == "add_user" || name == "update_take")
        {
            reject (juce::String (juce::CharPointer_UTF8 ("operaci\xc3\xb3n reservada al anfitri\xc3\xb3n")));
            return;
        }

        const std::lock_guard<std::mutex> scoped (stateLock);
        const auto result = applyLocked (op, c.user);

        if (! result.ok)
            reject (result.error);
    }

    void handleTakeComplete (Connection& c, const juce::var& message)
    {
        const auto takeId = message["take"].toString();
        const std::lock_guard<std::mutex> scoped (stateLock);
        const auto p = project.load();
        const auto* take = p->findTake (takeId);

        if (take == nullptr || take->author != c.user)
        {
            log ("take_complete ignorado de " + c.name + ": toma ajena o desconocida");
            return;
        }

        if (take->complete)
            return;

        const auto result = applyLocked (Ops::updateTake (take->id, take->segments, take->samples, true), c.user);

        if (! result.ok)
            log ("take_complete fallo: " + result.error);
    }

    // Relays a rec_* message to every other control connection with "user" added.
    void relayToOthers (Connection& c, const juce::var& message)
    {
        auto copy = juce::var (message.getDynamicObject()->clone().release());
        copy.getDynamicObject()->setProperty ("user", c.user);
        const auto line = lineOf (copy);

        for (auto& o : controls)
            if (o.get() != &c)
                o->send (line);
    }

    void handleRecStart (Connection& c, const juce::var& message)
    {
        const auto takeId = message["take"].toString(), trackId = message["track"].toString();
        const auto start = hostdetail::readInteger (message["start"], 0, (int64_t) 1 << 50);

        const std::lock_guard<std::mutex> scoped (stateLock);
        const auto p = project.load();
        const auto* take = p->findTake (takeId);
        const auto* track = p->findTrack (trackId);

        if (take == nullptr || take->author != c.user || track == nullptr || ! canEditTrack (*p, *track, c.user)
             || ! start)
        {
            log ("rec_start ignorado de " + c.name);
            return;
        }

        c.recording = Connection::Recording { takeId, trackId, *start };
        relayToOthers (c, message);
        broadcastPresence();
    }

    void handleRecPeaks (Connection& c, const juce::var& message)
    {
        const std::lock_guard<std::mutex> scoped (stateLock);

        if (c.recording && c.recording->take == message["take"].toString())
            relayToOthers (c, message);
    }

    void handleRecStop (Connection& c, const juce::var& message)
    {
        const std::lock_guard<std::mutex> scoped (stateLock);

        if (! c.recording || c.recording->take != message["take"].toString())
            return;

        c.recording.reset();
        relayToOthers (c, message);
        broadcastPresence();
    }

    //==========================================================================
    bool handleSegment (Connection& c, const juce::var& message)
    {
        const auto byteCount = hostdetail::readInteger (message["bytes"], 1, maxSegmentBytes);

        if (! byteCount)
            return refuse (c, juce::String (juce::CharPointer_UTF8 ("segmento demasiado grande o inv\xc3\xa1lido")));

        juce::MemoryBlock bytes;

        if (! c.readExactly (bytes, (size_t) *byteCount))
            return false;

        const auto takeId = message["take"].toString();
        const auto indexValue = hostdetail::readInteger (message["index"], 0, INT_MAX);
        const auto samplesValue = hostdetail::readInteger (message["samples"], 1, segmentSamples);
        const auto channelsValue = hostdetail::readInteger (message["channels"], 1, 2);
        const auto index = indexValue ? (int) *indexValue : -1;
        const auto samples = samplesValue ? *samplesValue : 0;
        const auto channels = channelsValue ? (int) *channelsValue : 0;
        const auto sha1 = message["sha1"].toString().toLowerCase();

        auto bad = [&] (int expected, const juce::String& reason)
        {
            c.send (object ({ { "t", "segment_bad" }, { "take", takeId }, { "index", message["index"] },
                              { "expected", expected }, { "reason", reason } }));
            return true;
        };

        // Uploads are validated and stored one at a time; stateLock is only taken to read and to apply.
        const std::lock_guard<std::mutex> uploading (uploadLock);
        std::optional<Take> take;

        {
            const std::lock_guard<std::mutex> scoped (stateLock);

            if (const auto* t = project.load()->findTake (takeId))
                take = *t;
        }

        if (! take)
            return bad (0, "toma desconocida");

        if (take->author != c.user)
            return bad (take->segments, "sin permiso: la toma es de otro usuario");

        if (take->complete)
            return bad (take->segments, "la toma ya esta completa");

        if (! indexValue || ! samplesValue || ! channelsValue)
            return bad (take->segments, "segmento malformado");

        if (index != take->segments)
            return bad (take->segments, "fuera de orden");

        if (Sha1::of (bytes.getData(), bytes.getSize()) != sha1)
            return bad (take->segments, "SHA-1 incorrecto");

        juce::AudioBuffer<float> decoded;

        if (channels != take->channels || samples < 1 || samples > segmentSamples
             || ! decodeSegment (bytes.getData(), bytes.getSize(), decoded)
             || decoded.getNumChannels() != take->channels || (int64_t) decoded.getNumSamples() != samples)
            return bad (take->segments, juce::String (juce::CharPointer_UTF8 ("audio inv\xc3\xa1lido")));

        const auto file = segmentFile (take->id, index);

        if (const auto written = writeFileAtomically (file, bytes.getData(), bytes.getSize()); written.failed())
        {
            log ("no se pudo guardar el segmento: " + written.getErrorMessage());
            return bad (take->segments, "no se pudo guardar el segmento");
        }

        const auto payload = std::make_shared<const juce::MemoryBlock> (std::move (bytes));
        const std::lock_guard<std::mutex> scoped (stateLock);
        const auto* current = project.load()->findTake (take->id);

        // Only a take_complete racing this upload can change the take meanwhile.
        if (current == nullptr || current->segments != index || current->complete)
        {
            file.deleteFile();
            return bad (current != nullptr ? current->segments : 0, "la toma cambio durante la subida");
        }

        const auto result = applyLocked (Ops::updateTake (take->id, index + 1, current->samples + samples, false), take->author);

        if (! result.ok)
        {
            file.deleteFile();
            return bad (take->segments, result.error);
        }

        const auto line = lineOf (object ({ { "t", "segment" }, { "take", take->id }, { "index", index },
                                            { "samples", (juce::int64) samples }, { "channels", take->channels },
                                            { "bytes", (juce::int64) payload->getSize() }, { "sha1", sha1 } }));

        for (auto& o : datas)
            if (o.get() != &c)
                o->send (line, payload);

        c.send (object ({ { "t", "segment_ok" }, { "take", take->id }, { "index", index } }));
        return true;
    }

    //==========================================================================
    // Caller holds stateLock. Applies, publishes, saves and broadcasts `applied` to every control connection.
    OpResult applyLocked (const juce::var& op, const Id& by)
    {
        Project next = *project.load();
        auto result = applyOp (next, op, by);

        if (! result.ok)
            return result;

        auto published = std::make_shared<const Project> (std::move (next));
        project.store (published);

        if (const auto saved = saveProjectAtomically (*published, projectFile); saved.failed())
            log ("no se pudo guardar project.json: " + saved.getErrorMessage());

        const auto line = lineOf (object ({ { "t", "applied" }, { "version", (juce::int64) published->version },
                                            { "by", by }, { "op", op } }));

        for (auto& o : controls)
            o->send (line);

        return result;
    }

    // Caller holds stateLock.
    void broadcastPresence()
    {
        const auto p = project.load();
        juce::Array<juce::var> users;

        for (const auto& u : p->users)
        {
            auto entry = object ({ { "id", u.id }, { "name", u.name }, { "online", false } });

            for (const auto& o : controls)
            {
                if (o->user != u.id)
                    continue;

                entry.getDynamicObject()->setProperty ("online", true);

                if (o->recording)
                    entry.getDynamicObject()->setProperty ("recording", object ({ { "take", o->recording->take },
                                                                                  { "track", o->recording->track },
                                                                                  { "start", (juce::int64) o->recording->start } }));
            }

            users.add (entry);
        }

        const auto line = lineOf (object ({ { "t", "presence" }, { "users", users } }));

        for (auto& o : controls)
            o->send (line);
    }

    // Caller holds stateLock.
    void publishUdpPeers()
    {
        udpPeers.store (std::make_shared<const PeerList> (controls));
    }

    // Caller holds stateLock.
    void closeDataConnectionsOf (uint32_t clientId)
    {
        for (auto it = datas.begin(); it != datas.end();)
        {
            if ((*it)->clientId == clientId)
            {
                (*it)->markDead();
                it = datas.erase (it);
            }
            else
            {
                ++it;
            }
        }
    }

    ConnectionPtr selfOf (Connection& c)
    {
        const std::lock_guard<std::mutex> scoped (connectionsLock);

        for (auto& o : connections)
            if (o.get() == &c)
                return o;

        return {};   // only during stop(), when lists are being cleared anyway
    }

    // Reader thread on its way out.
    void onClosed (Connection& c)
    {
        const std::lock_guard<std::mutex> scoped (stateLock);

        if (c.kind == Connection::Kind::control)
        {
            const auto it = std::find_if (controls.begin(), controls.end(), [&c] (const ConnectionPtr& o) { return o.get() == &c; });

            if (it != controls.end())
            {
                controls.erase (it);
                closeDataConnectionsOf (c.clientId);
                publishUdpPeers();
                log (c.name + " se desconecto");
            }

            c.recording.reset();
            broadcastPresence();
        }
        else if (c.kind == Connection::Kind::data)
        {
            datas.erase (std::remove_if (datas.begin(), datas.end(), [&c] (const ConnectionPtr& o) { return o.get() == &c; }),
                         datas.end());
        }
    }

    //==========================================================================
    void udpLoop()
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

            uint32_t clientId = 0;
            const bool keepalive = n == keepaliveBytes && buffer[0] == keepaliveByte;

            if (keepalive)
            {
                clientId = (uint32_t) buffer[1] | ((uint32_t) buffer[2] << 8) | ((uint32_t) buffer[3] << 16)
                         | ((uint32_t) buffer[4] << 24);
            }
            else
            {
                AudioPacketView view;

                if (! decodeAudioPacket (buffer, (size_t) n, view))
                    continue;

                clientId = view.clientId;
            }

            if (clientId == 0)
                continue;

            const auto peers = udpPeers.load (std::memory_order_acquire);
            Connection* sender = nullptr;

            for (auto& p : *peers)
                if (p->clientId == clientId)
                    sender = p.get();

            if (sender == nullptr)
                continue;

            const auto packed = packAddress (from);
            const auto now = nowMs();
            const auto known = sender->udpAddress.load (std::memory_order_acquire);

            if (known != 0 && known != packed && now - sender->udpLastSeenMs.load (std::memory_order_relaxed) < endpointSilenceMs)
                continue;   // someone else is still sending from the recorded address

            if (known != packed)
                sender->udpAddress.store (packed, std::memory_order_release);

            sender->udpLastSeenMs.store (now, std::memory_order_relaxed);

            if (keepalive)
                continue;

            for (auto& p : *peers)
            {
                if (p.get() == sender || ! p->listening.load (std::memory_order_acquire))
                    continue;

                const auto address = p->udpAddress.load (std::memory_order_acquire);

                if (address == 0)
                    continue;

                const auto to = unpackAddress (address);
                sendto ((SOCKET) udpSocket, (const char*) buffer, n, 0, (const sockaddr*) &to, sizeof (to));
            }
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);
    }

    //==========================================================================
    ProjectHost& owner;
    HostConfig config;

    juce::File projectDir, projectFile;      // fixed while running
    Id hostUser;
    juce::String sessionId;
    int boundPort = 0;

    // Lock order: uploadLock -> stateLock -> connectionsLock -> Connection::queueLock.
    std::mutex uploadLock;                   // one segment upload validated + stored at a time
    std::mutex stateLock;                    // project changes, controls/datas, recording, every broadcast
    std::atomic<std::shared_ptr<const Project>> project;   // written under stateLock, read anywhere
    PeerList controls, datas;

    std::atomic<std::shared_ptr<const PeerList>> udpPeers;  // copy of `controls` for the UDP thread

    std::mutex connectionsLock;              // every live Connection object (ownership)
    std::vector<ConnectionPtr> connections;

    std::unique_ptr<juce::StreamingSocket> listener;
    std::thread acceptThread, udpThread;
    uintptr_t udpSocket = ~(uintptr_t) 0;
    std::atomic<bool> running { false };
    bool winsockStarted = false;
};

//==============================================================================
ProjectHost::ProjectHost (HostConfig config) : impl (std::make_unique<Impl> (*this, config)) {}

ProjectHost::~ProjectHost()
{
    stop();
}

juce::Result ProjectHost::start (const juce::File& projectDir, const Id& hostUser, const juce::String& hostName,
                                 const juce::String& newProjectName)
{
    return impl->start (projectDir, hostUser, hostName, newProjectName);
}

void ProjectHost::stop()
{
    impl->stop();
}

juce::String ProjectHost::getSessionId() const
{
    return impl->sessionId;
}

int ProjectHost::getPort() const noexcept
{
    return impl->boundPort;
}

std::shared_ptr<const Project> ProjectHost::snapshot() const
{
    return impl->project.load();
}
}
