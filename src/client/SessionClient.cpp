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
#include <deque>

namespace ensayo
{
namespace
{
    juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

    constexpr int pingIntervalMs = 1000;
    constexpr int connectTimeoutMs = 5000;
    constexpr size_t maxLineBytes = 64 * 1024;
    constexpr int uploadChunkBytes = 64 * 1024;
    constexpr size_t maxQueuedUploadChunks = 8;    // back-pressure on the upload thread: 512 KB ahead of the wire

    constexpr int writeStallTimeoutMs = 10000;

    // juce::StreamingSocket::write is a bare send() on a non-blocking socket, so writes can be partial or fail
    // with WSAEWOULDBLOCK and must be retried. The retry must NOT use waitUntilReady: JUCE implements it with a
    // try-lock on the socket's read lock, which the control thread holds almost permanently inside its own
    // waitUntilReady (true, 100) loop, so a writer waiting that way fails as soon as the send buffer is full
    // (and, when it does win the lock, makes the reader's wait fail instead). A short sleep is enough: the
    // kernel drains the send buffer independently of this thread. Gives up after writeStallTimeoutMs without progress.
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
// Raw Winsock UDP socket, two threads: this one blocks in recvfrom (no timeout) and feeds mix packets into the
// jitter buffer; a nested send thread sleeps on the client's wake event and drains the outgoing queue the moment the
// audio thread pushes a packet, so a packet never waits for a receive to finish or time out. Both ask MMCSS for
// "Pro Audio" (best effort). Shutdown closes the socket from the destructor, which ends the blocking recvfrom.
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
        closeSocket();
        stopThread (3000);
    }

    void run() override
    {
       #if JUCE_WINDOWS
        WSADATA wsaData;

        if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
            return;

        if (! resolve (server))
        {
            owner.udpFailure.store (UdpFailure::resolve);
            WSACleanup();
            return;
        }

        const SOCKET sock = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock == INVALID_SOCKET)
        {
            owner.udpFailure.store (UdpFailure::socket);
            WSACleanup();
            return;
        }

        socketHandle.store (sock);

        if (threadShouldExit())   // the destructor may already have run closeSocket() before the handle was published
        {
            closeSocket();
            WSACleanup();
            return;
        }

        // Do not turn ICMP port-unreachable into recvfrom errors.
        BOOL disable = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl (sock, SIO_UDP_CONNRESET, &disable, sizeof (disable), nullptr, 0, &bytesReturned, nullptr, nullptr);

        const int bufferBytes = 256 * 1024;
        setsockopt (sock, SOL_SOCKET, SO_RCVBUF, (const char*) &bufferBytes, sizeof (bufferBytes));
        setsockopt (sock, SOL_SOCKET, SO_SNDBUF, (const char*) &bufferBytes, sizeof (bufferBytes));

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl (INADDR_ANY);
        local.sin_port = 0;

        if (::bind (sock, (const sockaddr*) &local, sizeof (local)) != 0)
        {
            owner.udpFailure.store (UdpFailure::bind);
            closeSocket();
            WSACleanup();
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);   // best effort

        {
            SendThread sender (*this);
            sender.startThread (juce::Thread::Priority::highest);

            std::vector<uint8_t> incoming (maxPacketBytes);

            while (! threadShouldExit())
            {
                sockaddr_in from {};
                int fromLen = sizeof (from);
                const int n = recvfrom (sock, (char*) incoming.data(), (int) incoming.size(), 0, (sockaddr*) &from, &fromLen);

                if (n < 0)
                {
                    if (threadShouldExit())   // the socket was closed under us
                        break;

                    if (WSAGetLastError() != WSAEMSGSIZE)
                        juce::Thread::sleep (1);   // transient error: keep receiving without spinning

                    continue;
                }

                if (n == 0)
                    continue;

                AudioPacketView view;

                if (! decodeAudioPacket (incoming.data(), (size_t) n, view) || (view.flags & AudioFlags::mix) == 0)
                    continue;

                const auto arrival = juce::Time::getMillisecondCounterHiRes();
                owner.mixPacketsReceived.fetch_add (1, std::memory_order_relaxed);
                owner.trackPosition.store (view.trackPosition, std::memory_order_relaxed);
                owner.buffer->push (view.seq, view.voice, view.voiceLen, arrival);
            }

            // ~SendThread: signals, wakes and joins the sender before the socket goes away.
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);

        closeSocket();
        WSACleanup();
       #endif
    }

private:
   #if JUCE_WINDOWS
    class SendThread : public juce::Thread
    {
    public:
        explicit SendThread (UdpThread& u) : juce::Thread ("EnsayoUdpSend"), udp (u) {}

        ~SendThread() override
        {
            signalThreadShouldExit();
            SetEvent ((HANDLE) udp.owner.sendWake);
            stopThread (3000);
        }

        void run() override
        {
            DWORD taskIndex = 0;
            HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);   // best effort

            std::vector<uint8_t> toSend (maxPacketBytes);
            const HANDLE wake = (HANDLE) udp.owner.sendWake;

            while (! threadShouldExit())
            {
                size_t len = 0;

                while (udp.owner.outgoing.pop (toSend.data(), len))
                {
                    const SOCKET sock = udp.socketHandle.load();

                    if (sock == INVALID_SOCKET)
                        break;

                    sendto (sock, (const char*) toSend.data(), (int) len, 0, (const sockaddr*) &udp.server, sizeof (udp.server));
                }

                // Auto-reset event: a push that lands between the drain and this wait leaves it signalled, so nothing is
                // missed. The timeout only guards against a lost wake-up.
                if (wake != nullptr)
                    WaitForSingleObject (wake, 100);
                else
                    juce::Thread::sleep (1);
            }

            if (mmcss != nullptr)
                AvRevertMmThreadCharacteristics (mmcss);
        }

    private:
        UdpThread& udp;
    };

    // Any thread; idempotent. Closing from the destructor is what unblocks the receive loop's recvfrom.
    void closeSocket()
    {
        const SOCKET sock = socketHandle.exchange (INVALID_SOCKET);

        if (sock != INVALID_SOCKET)
            closesocket (sock);
    }

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

    std::atomic<SOCKET> socketHandle { INVALID_SOCKET };
    sockaddr_in server {};   // written before the send thread starts, read-only afterwards
   #endif

    SessionClient& owner;
    const juce::String host;
    const int port;
};

//==============================================================================
// The only thread that writes to the control socket, so no caller ever waits on the kernel's send buffer: control
// lines (gain, transport, ping, hello) are queued by sendLine and written in order; an upload is queued chunk by
// chunk by the upload thread with back-pressure. A control line jumps ahead of an upload that has not yet started,
// but once the track_upload_begin header is on the wire the server reads exactly `size` raw bytes after it, so
// nothing may be interleaved until the last chunk is out: lines queued meanwhile follow the upload.
class SessionClient::WriterThread : public juce::Thread
{
public:
    struct Chunk
    {
        std::vector<char> bytes;
        bool header = false, last = false;
        juce::int64 done = 0, total = 0;   // bytes of the file on the wire after this chunk, and the file size
    };

    explicit WriterThread (SessionClient& o) : juce::Thread ("EnsayoWriter"), owner (o) {}

    ~WriterThread() override
    {
        signalThreadShouldExit();
        work.signal();
        space.signal();
        stopThread (3000);
    }

    // Any thread. Returns at once.
    void enqueueLine (const juce::String& line)
    {
        const auto* text = line.toRawUTF8();
        std::vector<char> bytes (text, text + std::strlen (text));

        {
            const juce::ScopedLock sl (lock);
            control.push_back (std::move (bytes));
        }

        work.signal();
    }

    // Upload thread. Blocks while maxQueuedUploadChunks are waiting; false when the caller or the writer is going
    // away or an earlier chunk of this upload failed (the failure has already been reported).
    bool enqueueChunk (Chunk chunk, const juce::Thread& caller)
    {
        for (;;)
        {
            if (caller.threadShouldExit() || threadShouldExit() || uploadFailed.load())
                return false;

            {
                const juce::ScopedLock sl (lock);

                if (upload.size() < maxQueuedUploadChunks)
                {
                    upload.push_back (std::move (chunk));
                    break;
                }
            }

            space.wait (100);
        }

        work.signal();
        return true;
    }

    // Message thread, before a new upload is queued: drops the chunks of the previous one that are still waiting.
    void discardUpload()
    {
        const juce::ScopedLock sl (lock);
        upload.clear();
        uploadFailed.store (false);
        space.signal();
    }

    bool isUploadOnTheWire() const noexcept { return uploadOnTheWire.load(); }

    void run() override
    {
        int lastPercent = -1;

        while (! threadShouldExit())
        {
            std::vector<char> line;
            Chunk chunk;
            bool haveLine = false, haveChunk = false;

            {
                const juce::ScopedLock sl (lock);

                if (! uploadOnTheWire.load() && ! control.empty())
                {
                    line = std::move (control.front());
                    control.pop_front();
                    haveLine = true;
                }
                else if (! upload.empty())
                {
                    chunk = std::move (upload.front());
                    upload.pop_front();
                    haveChunk = true;

                    if (chunk.header)
                    {
                        uploadOnTheWire.store (true);
                        lastPercent = -1;
                    }
                }
            }

            if (haveLine)
            {
                writeAll (owner.socket, line.data(), (int) line.size(), this);   // a dead socket is reported by the reader
                continue;
            }

            if (haveChunk)
            {
                space.signal();

                if (! writeAll (owner.socket, chunk.bytes.data(), (int) chunk.bytes.size(), this))
                {
                    failUpload();
                    continue;
                }

                if (chunk.last)
                {
                    uploadOnTheWire.store (false);
                    owner.postUploadStatus ("Esperando al servidor" + utf8 ("\xe2\x80\xa6"), 1.0f);
                }
                else if (! chunk.header && chunk.total > 0)
                {
                    const int percent = (int) (chunk.done * 100 / chunk.total);

                    if (percent != lastPercent)
                    {
                        lastPercent = percent;
                        owner.postUploadStatus ("Subiendo " + juce::String (percent) + " %", (float) chunk.done / (float) chunk.total);
                    }
                }

                continue;
            }

            // Nothing to write (or an upload on the wire is waiting for the upload thread to read the next chunk).
            work.wait (100);
        }
    }

private:
    void failUpload()
    {
        {
            const juce::ScopedLock sl (lock);
            upload.clear();
            uploadFailed.store (true);
            uploadOnTheWire.store (false);
        }

        space.signal();

        if (! threadShouldExit())
            owner.postUploadError ("No se pudo enviar el archivo");
    }

    SessionClient& owner;
    juce::CriticalSection lock;
    std::deque<std::vector<char>> control;
    std::deque<Chunk> upload;
    juce::WaitableEvent work, space;
    std::atomic<bool> uploadOnTheWire { false }, uploadFailed { false };
};

//==============================================================================
// Reads one file and feeds it to the writer: the track_upload_begin line followed by the raw bytes in 64 KB chunks.
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
            owner.postUploadError ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        owner.postUploadStatus ("Calculando checksum" + utf8 ("\xe2\x80\xa6"), 0.0f);
        const auto sha1 = Sha1::hexOfFile (file);

        if (sha1.isEmpty())
        {
            owner.postUploadError ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        juce::FileInputStream in (file);

        if (! in.openedOk())
        {
            owner.postUploadError ("No se puede leer el archivo " + file.getFullPathName());
            return;
        }

        auto* target = owner.writer.get();   // outlives this thread: stopThreads destroys the upload first

        if (target == nullptr)
            return;

        auto begin = makeMessage ("track_upload_begin");
        begin.getDynamicObject()->setProperty ("name", file.getFileName());
        begin.getDynamicObject()->setProperty ("size", (juce::int64) size);
        begin.getDynamicObject()->setProperty ("sha1", sha1);

        owner.postUploadStatus ("Subiendo 0 %", 0.0f);

        WriterThread::Chunk header;
        const auto headerLine = makeLine (begin);
        const auto* headerBytes = headerLine.toRawUTF8();
        header.bytes.assign (headerBytes, headerBytes + std::strlen (headerBytes));
        header.header = true;
        header.total = size;

        if (! target->enqueueChunk (std::move (header), *this))
            return;

        juce::int64 sent = 0;

        while (sent < size && ! threadShouldExit())
        {
            WriterThread::Chunk chunk;
            chunk.bytes.resize ((size_t) juce::jmin ((juce::int64) uploadChunkBytes, size - sent));
            const int n = in.read (chunk.bytes.data(), (int) chunk.bytes.size());

            if (n <= 0)
            {
                owner.postUploadError ("No se pudo leer el archivo durante la subida");
                return;
            }

            chunk.bytes.resize ((size_t) n);
            sent += n;
            chunk.done = sent;
            chunk.total = size;
            chunk.last = sent >= size;

            if (! target->enqueueChunk (std::move (chunk), *this))
                return;
        }
    }

private:
    SessionClient& owner;
    const juce::File file;
};

//==============================================================================
SessionClient::SessionClient()
    : buffer (std::make_unique<JitterBuffer> (codedBytesStereo (FrameSize::f128), frameSamples (FrameSize::f128) / 48.0)),
      outgoing (64),
      generation (std::make_shared<std::atomic<int>> (0))
{
   #if JUCE_WINDOWS
    sendWake = CreateEventW (nullptr, FALSE, FALSE, nullptr);   // auto-reset, initially clear
   #endif
}

SessionClient::~SessionClient()
{
    leave();

   #if JUCE_WINDOWS
    if (sendWake != nullptr)
        CloseHandle ((HANDLE) std::exchange (sendWake, nullptr));
   #endif
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
    mixPacketsReceived.store (0);
    udpFailure.store (UdpFailure::none);
    buffer->reset();

    writer = std::make_unique<WriterThread> (*this);
    writer->startThread();
    control = std::make_unique<ControlThread> (*this, host, port, hello);
    control->startThread();
}

void SessionClient::stopThreads()
{
    upload = nullptr;   // first: it blocks on the writer's back-pressure

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

    writer = nullptr;   // the closed socket ends any write in progress
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

    if (writer != nullptr)
        writer->discardUpload();

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
// Audio thread. SetEvent is the one kernel call the callback makes: it never blocks (it only flips the event and
// readies the waiting send thread), so the packet leaves right away instead of waiting for the UDP thread's loop.
void SessionClient::sendAudio (const uint8_t* packet, size_t len) noexcept
{
    if (! outgoing.push (packet, len))
        return;

   #if JUCE_WINDOWS
    if (sendWake != nullptr)
        SetEvent ((HANDLE) sendWake);
   #endif
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
    if (writer != nullptr)
        writer->enqueueLine (makeLine (message));
}

// Pings carry a timestamp, so one queued behind a long upload would report the upload time as the round trip.
bool SessionClient::trySendLine (const juce::var& message)
{
    if (writer == nullptr || writer->isUploadOnTheWire())
        return false;

    writer->enqueueLine (makeLine (message));
    return true;
}

// The lambdas capture the client, not the calling thread: the client may delete that thread before they run.
void SessionClient::postUploadStatus (const juce::String& text, float progress)
{
    post ([client = this, text, progress]
    {
        client->state.trackUploadStatus = text;
        client->state.trackUploadProgress = progress;
        client->notifyStateChanged();
    });
}

void SessionClient::postUploadError (const juce::String& message)
{
    post ([client = this, message]
    {
        client->state.trackUploadStatus = message;
        client->state.trackUploadProgress = 0.0f;
        client->notifyStateChanged();

        if (client->onError != nullptr)
            client->onError (message);
    });
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
            // Absent (older server) → treated as "unknown", which the UI must not read as "no packets".
            s.packets = item.hasProperty ("packets") ? (uint32_t) (juce::int64) item["packets"] : ~0u;
            s.wrongFrame = (uint32_t) (juce::int64) item["wrongFrame"];
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
            state.trackUploadStatus = "Listo: Play para reproducir";
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
