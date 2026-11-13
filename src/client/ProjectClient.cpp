#include "client/ProjectClient.h"
#include "core/Sha1.h"
#include "core/SpscQueue.h"
#include <juce_events/juce_events.h>

#if JUCE_WINDOWS
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <winsock2.h>
 #include <ws2tcpip.h>
 #include <mswsock.h>
 #include <avrt.h>
#endif

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstring>
#include <deque>
#include <mutex>
#include <optional>
#include <string>

namespace ensayo
{
namespace
{
    juce::String utf8 (const char* text) { return juce::String (juce::CharPointer_UTF8 (text)); }

    constexpr int connectTimeoutMs = 5000;
    constexpr int pingIntervalMs = 1000;
    constexpr int keepaliveIntervalMs = 1000;
    constexpr int writeStallTimeoutMs = 10000;
    constexpr size_t maxLineBytes = 16 * 1024 * 1024;     // welcome / project can be large
    constexpr int64_t maxSegmentBytes = 2 * 1024 * 1024;  // the host's own cap
    constexpr size_t peaksPerMessage = 100;               // keeps rec_peaks well under the host's 8 KB line cap
    constexpr int64_t maxPeakIndex = (int64_t) TakeCache::maxChunks * segmentSamples / peakBlock;
    constexpr int maxUploadFailures = 3;
    constexpr juce::uint32 welcomeTimeoutMs = 10000;
    constexpr juce::uint32 controlSilenceMs = 15000;   // the host answers a ping every second
    constexpr int clientIdOffset = 6;                     // magic, version, flags, frameCode, seq u16, clientId u32

    juce::var object (std::initializer_list<std::pair<const char*, juce::var>> fields)
    {
        auto* o = new juce::DynamicObject();

        for (const auto& [key, value] : fields)
            o->setProperty (key, value);

        return juce::var (o);
    }

    // Integral values within [min, max] only (int, int64 or an integral double).
    std::optional<int64_t> readInteger (const juce::var& v, int64_t min, int64_t max)
    {
        int64_t value = 0;

        if (v.isInt() || v.isInt64())
            value = (int64_t) v;
        else if (v.isDouble())
        {
            const double d = (double) v;

            if (! std::isfinite (d) || std::floor (d) != d || std::abs (d) > 9007199254740992.0)
                return std::nullopt;

            value = (int64_t) d;
        }
        else
            return std::nullopt;

        if (value < min || value > max)
            return std::nullopt;

        return value;
    }

    // Same retry rule as the old live-mix client: a bare send on a non-blocking socket, retried with a short sleep (never
    // waitUntilReady, whose read lock is held by the reader thread). Gives up after writeStallTimeoutMs without progress.
    bool writeAll (juce::StreamingSocket& socket, const void* data, size_t len, const juce::Thread* thread)
    {
        const auto* bytes = static_cast<const char*> (data);
        size_t written = 0;
        int stalledMs = 0;

        while (written < len)
        {
            if (thread != nullptr && thread->threadShouldExit())
                return false;

            if (! socket.isConnected())
                return false;

            const int w = socket.write (bytes + written, (int) std::min (len - written, (size_t) (1 << 20)));

            if (w > 0)
            {
                written += (size_t) w;
                stalledMs = 0;
                continue;
            }

           #if JUCE_WINDOWS
            if (w < 0 && WSAGetLastError() != WSAEWOULDBLOCK)
                return false;
           #endif

            if (stalledMs >= writeStallTimeoutMs)
                return false;

            juce::Thread::sleep (2);
            stalledMs += 2;
        }

        return true;
    }

    // A juce::Thread running a function; the owner wakes it (closing its socket, signalling its event) before
    // destroying it.
    class FnThread : public juce::Thread
    {
    public:
        FnThread (const juce::String& name, std::function<void (FnThread&)> fn) : juce::Thread (name), body (std::move (fn)) {}
        ~FnThread() override { signalThreadShouldExit(); stopThread (8000); }
        void run() override { body (*this); }

    private:
        std::function<void (FnThread&)> body;
    };

    // The only thread that writes to the control socket: lines are written in the order they were queued.
    class LineWriter : public juce::Thread
    {
    public:
        explicit LineWriter (juce::StreamingSocket& s) : juce::Thread ("EnsayoProjectWriter"), socket (s) {}

        ~LineWriter() override
        {
            signalThreadShouldExit();
            work.signal();
            stopThread (8000);
        }

        void enqueue (const juce::String& line)
        {
            {
                const std::lock_guard<std::mutex> scoped (lock);
                queue.emplace_back (line.toRawUTF8(), line.getNumBytesAsUTF8());
                ++unsent;
            }

            work.signal();
        }

        // Waits until every enqueued line was handed to the socket, or the timeout passes. True when drained.
        bool drain (int timeoutMs)
        {
            const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

            while (unsent.load() > 0 && isThreadRunning())
            {
                if (juce::Time::getMillisecondCounter() >= deadline)
                    return false;

                juce::Thread::sleep (2);
            }

            return unsent.load() == 0;
        }

        void run() override
        {
            while (! threadShouldExit())
            {
                std::string next;
                bool have = false;

                {
                    const std::lock_guard<std::mutex> scoped (lock);

                    if (! queue.empty())
                    {
                        next = std::move (queue.front());
                        queue.pop_front();
                        have = true;
                    }
                }

                if (have)
                {
                    writeAll (socket, next.data(), next.size(), this);   // a dead socket is reported by the reader
                    --unsent;
                }
                else
                    work.wait (100);
            }
        }

    private:
        juce::StreamingSocket& socket;
        std::mutex lock;
        std::deque<std::string> queue;
        std::atomic<int> unsent { 0 };   // enqueued and not yet written
        juce::WaitableEvent work;
    };

    juce::var regionToVar (const Region& r)
    {
        return object ({ { "id", r.id }, { "take", r.take }, { "start", (juce::int64) r.start },
                         { "offset", (juce::int64) r.offset }, { "length", (juce::int64) r.length } });
    }

    std::optional<Region> regionFromVar (const juce::var& v)
    {
        Region r;
        r.id = v["id"].toString();
        r.take = v["take"].toString();
        const auto start = readInteger (v["start"], 0, INT64_MAX / 4);
        const auto offset = readInteger (v["offset"], 0, INT64_MAX / 4);
        const auto length = readInteger (v["length"], 0, INT64_MAX / 4);

        if (! isValidId (r.id) || ! isValidId (r.take) || ! start || ! offset || ! length)
            return std::nullopt;

        r.start = *start;
        r.offset = *offset;
        r.length = *length;
        return r;
    }
}

juce::File defaultCacheRoot()
{
   #if JUCE_WINDOWS
    return juce::File::getSpecialLocation (juce::File::windowsLocalAppData).getChildFile ("EnsayoVirtual").getChildFile ("cache");
   #else
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory).getChildFile ("EnsayoVirtual").getChildFile ("cache");
   #endif
}

//==============================================================================
// Threads (all per connection, created by connect / the welcome, stopped by teardown):
//   control  - connects, sends hello, reads control lines, pings once per second
//   writer   - LineWriter: every control-socket write
//   data     - connects the data socket, sends data_hello, reads segments (→ TakeCache) and upload replies
//   uploader - every data-socket write after data_hello: one segment at a time, the next one after its reply
//   udp      - preview receive (→ previewBuffer) with a nested send thread (sendPreview queue + keepalive)
// Callbacks reach the message thread through post(), tagged with the connection's generation.
struct ProjectClient::Impl
{
    struct Finish { Id track; Region region; };

    // One take this client has to upload. Persisted under <cacheRoot>/<project>/pending/<take>/:
    // take.json (channels, rec track/start, creation time, finish intent) and <index>.flac per unacknowledged segment.
    struct Pending
    {
        Id take;
        int channels = 0;
        Id recTrack;
        int64_t recStart = 0, recOffset = 0, created = 0;
        std::optional<Finish> finish;
        int queued = 0;       // segments 0 .. queued-1 have been queued
        int nextIndex = 0;    // the next index to send (the host's take.segments once hostReady)

        // Per connection.
        bool hostReady = false;      // add_take is applied on the host (the take exists and is ours)
        bool addTakeSent = false, completeSent = false, regionSent = false, failed = false;
        int failures = 0;
    };

    Impl (ProjectClient& o, ClientConfig c)
        : owner (o), config (std::move (c)),
          preview (codedBytesStereo (FrameSize::f128), 128.0 / 48.0),
          outgoing (64),
          generation (std::make_shared<std::atomic<int>> (0))
    {
       #if JUCE_WINDOWS
        sendWake = CreateEventW (nullptr, FALSE, FALSE, nullptr);
       #endif
    }

    ~Impl()
    {
        ++(*generation);
        teardown();
        ++(*generation);   // anything posted with the current generation while the threads were stopping is dropped

       #if JUCE_WINDOWS
        if (sendWake != nullptr)
            CloseHandle ((HANDLE) sendWake);
       #endif
    }

    //==========================================================================
    ProjectClient& owner;
    const ClientConfig config;

    // Message thread.
    SessionLink link;
    bool welcomed = false, connected = false, hostFlag = false, resyncing = false;
    juce::String hostError, token;
    int udpPort = 0;
    std::shared_ptr<const Project> replica;
    std::vector<Presence> presenceList;
    std::vector<LiveRecording> live;
    int nextSeq = 1;
    std::map<int, Id> regionSeqs;   // add_region op seq → take

    std::atomic<int> ping { -1 };
    std::atomic<uint32_t> clientId { 0 };
    std::atomic<int64_t> downloaded { 0 };
    std::atomic<bool> listening { false }, canSend { false }, dataReady { false };
    int connectionGeneration = 0;
    int resyncAttempts = 0;
    static constexpr int maxResyncAttempts = 3;

    // Uploads and the cache (any thread).
    mutable std::mutex lock;
    std::unique_ptr<TakeCache> takeCache;   // replaced only on the message thread, under `lock`
    Id projectId;
    Id pendingsProject;          // the project whose pending list is in memory (loaded from disk once per project)
    std::vector<Pending> pendings;
    bool inFlight = false;
    juce::WaitableEvent uploadWake;

    // Sockets and threads.
    std::unique_ptr<juce::StreamingSocket> controlSocket, dataSocket;
    std::mutex writerLock;
    std::shared_ptr<LineWriter> writer;
    std::unique_ptr<FnThread> control, data, uploader, udp;

    JitterBuffer preview;
    PacketQueue outgoing;
    void* sendWake = nullptr;
   #if JUCE_WINDOWS
    std::atomic<SOCKET> udpSocket { INVALID_SOCKET };
   #endif

    std::shared_ptr<std::atomic<int>> generation;

    // The take being recorded here (beginTake .. finishTake/discardTake) and the peaks sent for it, so a reconnect in
    // the middle of the take can announce it again. Lines for it are enqueued under this lock so rec_start always
    // precedes its peaks on a connection.
    std::mutex activeLock;
    Id activeTake, activeTrack;
    int64_t activeStart = 0;
    std::vector<Peak> activePeaks;

    //==========================================================================
    void post (int gen, std::function<void()> fn)
    {
        juce::MessageManager::callAsync ([g = generation, gen, fn = std::move (fn)]
        {
            if (g->load() == gen)
                fn();
        });
    }

    void post (std::function<void()> fn)    { post (generation->load(), std::move (fn)); }

    void postError (int gen, const juce::String& message)
    {
        post (gen, [this, message] { if (owner.onError != nullptr) owner.onError (message); });
    }

    void postTransfers (int gen)
    {
        post (gen, [this] { if (owner.onTransfersChanged != nullptr) owner.onTransfersChanged(); });
    }

    // From API calls (message thread or the caller's own thread), not from connection threads.
    void postError (const juce::String& message)   { postError (generation->load(), message); }
    void postTransfers()                            { postTransfers (generation->load()); }

    // Any thread.
    void sendPeakLines (const Id& take, int64_t fromPeak, const Peak* peaks, size_t count)
    {
        auto round = [] (float v) { return std::round ((double) v * 10000.0) / 10000.0; };

        for (size_t first = 0; first < count; first += peaksPerMessage)
        {
            juce::Array<juce::var> list;
            const auto last = std::min (count, first + peaksPerMessage);

            for (size_t i = first; i < last; ++i)
                list.add (juce::Array<juce::var> { round (peaks[i].min), round (peaks[i].max) });

            sendLine (object ({ { "t", "rec_peaks" }, { "take", take }, { "from", (juce::int64) (fromPeak + (int64_t) first) },
                                { "peaks", list } }));
        }
    }

    // Message thread, at a welcome: a take still being recorded here is announced again with every peak so far.
    void resendActiveTake()
    {
        const std::lock_guard<std::mutex> scoped (activeLock);

        if (activeTake.isEmpty())
            return;

        sendLine (object ({ { "t", "rec_start" }, { "take", activeTake }, { "track", activeTrack },
                            { "start", (juce::int64) activeStart } }));
        sendPeakLines (activeTake, 0, activePeaks.data(), activePeaks.size());
    }

    void clearActive (const Id& take)
    {
        const std::lock_guard<std::mutex> scoped (activeLock);

        if (activeTake == take)
        {
            activeTake.clear();
            activeTrack.clear();
            activePeaks.clear();
        }
    }

    // Any thread.
    void sendLine (const juce::var& message)
    {
        std::shared_ptr<LineWriter> w;

        {
            const std::lock_guard<std::mutex> scoped (writerLock);
            w = writer;
        }

        if (w != nullptr)
            w->enqueue (makeLine (message));
    }

    //==========================================================================
    // Paths. Caller holds `lock` (projectId).
    juce::File pendingRoot() const   { return config.cacheRoot.getChildFile (projectId).getChildFile ("pending"); }
    juce::File takeDir (const Id& take) const   { return pendingRoot().getChildFile (take); }
    juce::File segmentFile (const Id& take, int index) const   { return takeDir (take).getChildFile (juce::String (index) + ".flac"); }
    juce::File metaFile (const Id& take) const   { return takeDir (take).getChildFile ("take.json"); }

    Pending* findPending (const Id& take)
    {
        for (auto& p : pendings)
            if (p.take == take)
                return &p;

        return nullptr;
    }

    void writeMetaLocked (const Pending& p)
    {
        auto meta = object ({ { "take", p.take }, { "channels", p.channels }, { "track", p.recTrack },
                              { "start", (juce::int64) p.recStart }, { "offset", (juce::int64) p.recOffset },
                              { "created", (juce::int64) p.created },
                              { "queued", p.queued } });

        if (p.finish)
            meta.getDynamicObject()->setProperty ("finish", object ({ { "track", p.finish->track },
                                                                      { "region", regionToVar (p.finish->region) } }));

        takeDir (p.take).createDirectory();

        if (! metaFile (p.take).replaceWithText (juce::JSON::toString (meta)))
            postError ("No se pudo guardar el estado de la toma " + p.take);
    }

    Pending& findOrCreateLocked (const Id& take)
    {
        if (auto* p = findPending (take))
            return *p;

        Pending p;
        p.take = take;
        p.created = juce::Time::currentTimeMillis();
        pendings.push_back (p);
        writeMetaLocked (pendings.back());
        return pendings.back();
    }

    void dropPendingLocked (const Id& take)
    {
        takeDir (take).deleteRecursively();
        pendings.erase (std::remove_if (pendings.begin(), pendings.end(), [&take] (const Pending& p) { return p.take == take; }),
                        pendings.end());
    }

    // Caller holds `lock`. A pending take this user cannot upload (the host says another user made it, e.g. after the
    // settings lost the user id): its folder moves to <project>/pending-orphaned/ so the recording survives.
    void orphanPendingLocked (const Id& take)
    {
        const auto from = takeDir (take);
        const auto orphanedRoot = config.cacheRoot.getChildFile (projectId).getChildFile ("pending-orphaned");
        const auto to = orphanedRoot.getNonexistentChildFile (take, {}, false);

        if (from.isDirectory())
        {
            if (orphanedRoot.createDirectory() && from.moveFileTo (to))
                postError ("La toma " + take + utf8 (" es de otro usuario en el anfitri\xc3\xb3n: no se sube y queda guardada en ")
                           + to.getFullPathName() + " (pending-orphaned)");
            else
                postError ("La toma " + take + utf8 (" es de otro usuario en el anfitri\xc3\xb3n y no se pudo mover a ")
                           + to.getFullPathName() + "; queda en " + from.getFullPathName());
        }

        pendings.erase (std::remove_if (pendings.begin(), pendings.end(), [&take] (const Pending& p) { return p.take == take; }),
                        pendings.end());
    }

    void deleteSegmentFilesLocked (const Pending& p, int from, int to)
    {
        for (int i = std::max (0, from); i < to; ++i)
            segmentFile (p.take, i).deleteFile();
    }

    // Rebuilds the pending list of the current project from disk (per-connection flags start cleared).
    void loadPendingsLocked()
    {
        pendings.clear();
        inFlight = false;

        for (const auto& dir : pendingRoot().findChildFiles (juce::File::findDirectories, false))
        {
            Pending p;
            p.take = dir.getFileName();

            if (! isValidId (p.take))
                continue;

            const auto meta = juce::JSON::parse (metaFile (p.take));

            if (meta.isObject())
            {
                p.channels = (int) readInteger (meta["channels"], 1, 2).value_or (0);
                p.recTrack = meta["track"].toString();
                p.recStart = readInteger (meta["start"], 0, INT64_MAX / 4).value_or (0);
                p.recOffset = readInteger (meta["offset"], 0, INT64_MAX / 4).value_or (0);
                p.created = readInteger (meta["created"], 0, INT64_MAX).value_or (0);

                const auto& finish = meta["finish"];

                if (finish.isObject() && isValidId (finish["track"].toString()))
                    if (const auto region = regionFromVar (finish["region"]))
                        p.finish = Finish { finish["track"].toString(), *region };
            }

            const auto metaQueued = meta.isObject() ? readInteger (meta["queued"], 0, INT_MAX) : std::nullopt;
            int lowest = INT_MAX, highest = -1;

            for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.flac"))
            {
                const auto name = f.getFileNameWithoutExtension();

                if (name.isEmpty() || ! name.containsOnly ("0123456789") || name.length() > 9)
                    continue;

                const int index = name.getIntValue();
                lowest = std::min (lowest, index);
                highest = std::max (highest, index);
            }

            // take.json records how many segments were queued; files above it were queued out of order and wait
            // for the gap to be filled. Older metadata without the count falls back to the files.
            p.queued = metaQueued ? (int) *metaQueued : highest + 1;
            p.nextIndex = (highest >= 0 && lowest < p.queued) ? lowest : p.queued;

            if (p.channels == 0 && takeCache != nullptr)
                p.channels = takeCache->channels (p.take);

            if (meta.isObject() && highest >= p.queued)
                absorbLocked (p, nullptr, 0);   // out-of-order files that now follow on

            if (! meta.isObject() && highest < 0)
            {
                dir.deleteRecursively();   // nothing to finish
                continue;
            }

            pendings.push_back (p);
        }

        std::stable_sort (pendings.begin(), pendings.end(), [] (const Pending& a, const Pending& b) { return a.created < b.created; });
    }

    // Caller holds `lock`. Advances `queued` over every segment file that continues the take (the one just written
    // is passed in memory), appending each to the local cache. False when the cache refused one: the segment is
    // still persisted and uploaded, but this machine will not hear it until the cache is rebuilt.
    bool absorbLocked (Pending& p, const void* firstData, size_t firstSize)
    {
        bool cached = true;

        for (;;)
        {
            const auto file = segmentFile (p.take, p.queued);
            juce::MemoryBlock fromDisk;

            if (firstData == nullptr)
            {
                if (! file.existsAsFile() || ! file.loadFileAsData (fromDisk) || fromDisk.isEmpty())
                    break;
            }

            const void* bytes = firstData != nullptr ? firstData : fromDisk.getData();
            const size_t size = firstData != nullptr ? firstSize : fromDisk.getSize();
            firstData = nullptr;

            if (takeCache != nullptr && ! takeCache->appendSegment (p.take, p.queued, bytes, size)
                 && takeCache->segmentCount (p.take) <= p.queued)
                cached = false;

            ++p.queued;
        }

        writeMetaLocked (p);
        return cached;
    }

    // Caller holds `lock`. After the last acknowledged segment of a finished take: take_complete (once per connection).
    void maybeCompleteLocked (Pending& p)
    {
        if (p.finish && p.hostReady && ! p.failed && ! p.completeSent && p.nextIndex >= p.queued)
        {
            p.completeSent = true;
            sendLine (object ({ { "t", "take_complete" }, { "take", p.take } }));
        }
    }

    //==========================================================================
    // Message thread. Moves every pending take forward from what the replica says; idempotent.
    void reconcile()
    {
        if (! welcomed || replica == nullptr || resyncing)
            return;

        std::vector<juce::var> ops;
        std::vector<std::pair<juce::var, Id>> regionOps;
        std::vector<Id> recStops;

        {
            const std::lock_guard<std::mutex> scoped (lock);
            std::vector<Id> drop, orphaned;

            for (auto& p : pendings)
            {
                const auto* t = replica->findTake (p.take);

                if (t == nullptr)
                {
                    if (! p.addTakeSent && (p.channels == 1 || p.channels == 2))
                    {
                        p.addTakeSent = true;
                        ops.push_back (Ops::addTake (p.take, p.channels));
                    }

                    continue;
                }

                if (t->author != config.user)
                {
                    orphaned.push_back (p.take);   // cannot be ours to upload, but its audio is never deleted
                    continue;
                }

                if (! p.hostReady)
                {
                    p.hostReady = true;
                    deleteSegmentFilesLocked (p, 0, t->segments);   // already on the host
                    p.nextIndex = t->segments;

                    if (t->segments > p.queued)
                    {
                        p.queued = t->segments;
                        absorbLocked (p, nullptr, 0);   // early segments already on disk follow on from the host's count
                    }
                }

                if (t->complete)
                {
                    deleteSegmentFilesLocked (p, p.nextIndex, p.queued);
                    p.nextIndex = p.queued = std::max (p.queued, t->segments);

                    if (! p.finish)
                    {
                        drop.push_back (p.take);
                    }
                    else if (const auto* track = replica->findTrack (p.finish->track);
                             track != nullptr && std::any_of (track->regions.begin(), track->regions.end(),
                                                              [&p] (const Region& r) { return r.id == p.finish->region.id; }))
                    {
                        drop.push_back (p.take);   // the region is on the host: done
                    }
                    else if (! p.regionSent)
                    {
                        p.regionSent = true;
                        regionOps.emplace_back (Ops::addRegion (p.finish->track, p.finish->region), p.take);
                        recStops.push_back (p.take);
                    }

                    continue;
                }

                maybeCompleteLocked (p);
            }

            for (const auto& take : drop)
                dropPendingLocked (take);

            for (const auto& take : orphaned)
                orphanPendingLocked (take);
        }

        for (const auto& op : ops)
            sendOpInternal (op);

        for (const auto& [op, take] : regionOps)
            regionSeqs[sendOpInternal (op)] = take;

        for (const auto& take : recStops)
            sendLine (object ({ { "t", "rec_stop" }, { "take", take } }));

        uploadWake.signal();
    }

    int sendOpInternal (const juce::var& op)
    {
        const int seq = nextSeq++;
        sendLine (object ({ { "t", "op" }, { "seq", seq }, { "op", op } }));
        return seq;
    }

    //==========================================================================
    // Message thread.
    void connect (const SessionLink& target)
    {
        ++(*generation);
        teardown();

        link = target;
        hostError = {};
        downloaded.store (0);
        ping.store (-1);
        clientId.store (0);
        preview.reset();
        connectionGeneration = generation->load();

        controlSocket = std::make_unique<juce::StreamingSocket>();

        {
            const std::lock_guard<std::mutex> scoped (writerLock);
            writer = std::make_shared<LineWriter> (*controlSocket);
        }

        writer->startThread();

        const auto gen = connectionGeneration;
        const auto hello = object ({ { "t", "hello" }, { "protocol", 2 }, { "session", link.sessionId },
                                     { "user", config.user }, { "name", config.userName } });

        control = std::make_unique<FnThread> ("EnsayoProjectControl", [this, gen, hello] (FnThread& t) { controlLoop (t, gen, hello); });
        control->startThread();
    }

    void teardown()
    {
        welcomed = false;
        connected = false;
        canSend.store (false);
        dataReady.store (false);

        if (control != nullptr)
            control->signalThreadShouldExit();

        if (data != nullptr)
            data->signalThreadShouldExit();

        if (uploader != nullptr)
            uploader->signalThreadShouldExit();

        if (controlSocket != nullptr)
            controlSocket->close();

        if (dataSocket != nullptr)
            dataSocket->close();

        uploadWake.signal();
        control = nullptr;
        data = nullptr;
        uploader = nullptr;

        std::shared_ptr<LineWriter> oldWriter;

        {
            const std::lock_guard<std::mutex> scoped (writerLock);
            oldWriter = std::move (writer);
        }

        oldWriter = nullptr;
        stopUdp();

        controlSocket = nullptr;
        dataSocket = nullptr;

        {
            const std::lock_guard<std::mutex> scoped (lock);
            inFlight = false;

            for (auto& p : pendings)
            {
                p.hostReady = p.addTakeSent = p.completeSent = p.regionSent = p.failed = false;
                p.failures = 0;
            }
        }

        regionSeqs.clear();
        live.clear();
        presenceList.clear();
        resyncing = false;
    }

    // Message thread: a connection ended on its own.
    void lost (const juce::String& reason)
    {
        const bool was = connected;
        const auto message = hostError.isNotEmpty() ? hostError : reason;
        ++(*generation);
        teardown();

        if (was)
        {
            if (owner.onDisconnected != nullptr)
                owner.onDisconnected (message);
        }
        else if (owner.onError != nullptr)
        {
            owner.onError (message);
        }
    }

    void postLost (int gen, const juce::String& reason)
    {
        post (gen, [this, reason] { lost (reason); });
    }

    //==========================================================================
    enum class ReadEnd { closed, invalid, timedOut };

    // Reads newline-terminated lines until the socket closes, the thread is asked to exit, or onTick returns false.
    // onLine returns how many raw payload bytes follow the line (0 for none, < 0 to drop the connection); the
    // payload goes to handleDownloaded. `lastRead` is the millisecond counter of the last bytes received.
    template <typename OnLine, typename OnTick>
    ReadEnd readLines (juce::StreamingSocket& socket, FnThread& thread, int gen, juce::uint32& lastRead, OnLine onLine, OnTick onTick)
    {
        lastRead = juce::Time::getMillisecondCounter();
        std::string pending;
        std::vector<char> chunk (64 * 1024);
        size_t payloadNeeded = 0;
        juce::var payloadHeader;

        while (! thread.threadShouldExit())
        {
            if (! onTick())
                return ReadEnd::timedOut;

            const int ready = socket.waitUntilReady (true, 100);

            if (ready < 0)
                return ReadEnd::closed;

            if (ready == 0)
                continue;

            const int n = socket.read (chunk.data(), (int) chunk.size(), false);

            if (n <= 0)
                return ReadEnd::closed;

            lastRead = juce::Time::getMillisecondCounter();
            pending.append (chunk.data(), (size_t) n);
            size_t consumed = 0;

            for (;;)
            {
                if (payloadNeeded > 0)
                {
                    if (pending.size() - consumed < payloadNeeded)
                        break;

                    juce::MemoryBlock payload (pending.data() + consumed, payloadNeeded);
                    consumed += payloadNeeded;
                    payloadNeeded = 0;
                    if (! handleDownloaded (gen, payloadHeader, payload))
                        return ReadEnd::invalid;   // reconnecting repairs the gap through data_hello's `have`

                    payloadHeader = juce::var();
                    continue;
                }

                const auto newline = pending.find ('\n', consumed);

                if (newline == std::string::npos)
                    break;

                const auto line = juce::String::fromUTF8 (pending.data() + consumed, (int) (newline - consumed));
                consumed = newline + 1;

                if (auto parsed = parseLine (line))
                {
                    const auto need = onLine (*parsed);

                    if (need < 0)
                        return ReadEnd::invalid;

                    if (need > 0)
                    {
                        payloadNeeded = (size_t) need;
                        payloadHeader = *parsed;
                    }
                }
            }

            pending.erase (0, consumed);

            if (payloadNeeded == 0 && pending.size() > maxLineBytes)
                return ReadEnd::invalid;
        }

        return ReadEnd::closed;
    }

    //==========================================================================
    // Control thread.
    void controlLoop (FnThread& t, int gen, const juce::var& hello)
    {
        if (! controlSocket->connect (link.host, link.port, connectTimeoutMs))
        {
            if (! t.threadShouldExit())
                postLost (gen, "No se pudo conectar a " + link.host + ":" + juce::String (link.port));

            return;
        }

        sendLine (hello);   // first line on the wire: nothing else is sent before welcome
        const auto started = juce::Time::getMillisecondCounter();
        auto nextPing = started + 200;
        juce::uint32 lastRead = started;
        bool welcomeSeen = false;

        const auto end = readLines (*controlSocket, t, gen, lastRead,
            [this, gen, &welcomeSeen] (const juce::var& m) -> int64_t
            {
                welcomeSeen = welcomeSeen || m["t"].toString() == "welcome";
                handleControlLine (gen, m);
                return 0;
            },
            [this, &nextPing, &lastRead, &welcomeSeen, started]
            {
                const auto now = juce::Time::getMillisecondCounter();

                // No welcome in time, or a half-open connection (no pong for a long while): give up.
                if ((! welcomeSeen && now - started > welcomeTimeoutMs) || now - lastRead > controlSilenceMs)
                    return false;

                if (now >= nextPing)
                {
                    nextPing = now + (juce::uint32) pingIntervalMs;
                    sendLine (object ({ { "t", "ping" }, { "ms", juce::Time::getMillisecondCounterHiRes() } }));
                }

                return true;
            });

        if (t.threadShouldExit())
            return;

        if (end == ReadEnd::closed)
            postLost (gen, utf8 ("Se perdi\xc3\xb3 la conexi\xc3\xb3n con el anfitri\xc3\xb3n"));
        else if (end == ReadEnd::timedOut)
            postLost (gen, utf8 ("El anfitri\xc3\xb3n no responde"));
        else
            postLost (gen, utf8 ("El anfitri\xc3\xb3n envi\xc3\xb3 un mensaje demasiado grande"));
    }

    void handleControlLine (int gen, const juce::var& m)
    {
        const auto type = m["t"].toString();

        if (type == "pong")
        {
            const auto rtt = juce::Time::getMillisecondCounterHiRes() - (double) m["ms"];

            if (std::isfinite (rtt) && rtt >= 0.0 && rtt < 600000.0)
                ping.store (juce::roundToInt (rtt));

            return;
        }

        if (type == "welcome")         post (gen, [this, m] { handleWelcome (m); });
        else if (type == "error")      post (gen, [this, m] { hostError = m["message"].toString(); });
        else if (type == "applied")    post (gen, [this, m] { handleApplied (m); });
        else if (type == "project")    post (gen, [this, m] { handleProject (m); });
        else if (type == "rejected")   post (gen, [this, m] { handleRejected (m); });
        else if (type == "presence")   post (gen, [this, m] { handlePresence (m); });
        else if (type == "rec_start" || type == "rec_peaks" || type == "rec_stop")
            post (gen, [this, m] { handleRec (m); });
    }

    //==========================================================================
    // Message thread handlers.
    void handleWelcome (const juce::var& m)
    {
        juce::String error;
        auto parsed = projectFromVar (m["project"], error);
        const auto id = readInteger (m["clientId"], 1, 0x7fffffff);

        if (! parsed || ! id || welcomed)
        {
            hostError = utf8 ("Bienvenida inv\xc3\xa1lida del anfitri\xc3\xb3n");
            lost (hostError);
            return;
        }

        replica = std::make_shared<const Project> (std::move (*parsed));
        hostFlag = (bool) m["isHost"];
        token = m["token"].toString();
        udpPort = (int) readInteger (m["udpPort"], 1, 65535).value_or (link.port);
        clientId.store ((uint32_t) *id);

        TakeCache* cachePtr = nullptr;

        {
            const std::lock_guard<std::mutex> scoped (lock);

            if (takeCache == nullptr || projectId != replica->id)
            {
                projectId = replica->id;
                takeCache = std::make_unique<TakeCache> (config.cacheRoot.getChildFile (projectId).getChildFile ("takes"));
            }

            cachePtr = takeCache.get();
        }

        cachePtr->loadFromDisk();

        {
            // Only the first welcome of a project reads the pending list from disk. Later welcomes keep the in-memory
            // counts, so a segment queued while reconnecting is never measured against a stale `queued`.
            const std::lock_guard<std::mutex> scoped (lock);

            if (pendingsProject != projectId)
            {
                loadPendingsLocked();
                pendingsProject = projectId;
            }
        }

        auto* have = new juce::DynamicObject();

        for (const auto& take : cachePtr->takes())
            have->setProperty (take, cachePtr->segmentCount (take));

        const auto dataHello = makeLine (object ({ { "t", "data_hello" }, { "clientId", (int) *id }, { "token", token },
                                                   { "have", juce::var (have) } }));

        welcomed = true;
        canSend.store (true);

        const auto gen = connectionGeneration;
        dataSocket = std::make_unique<juce::StreamingSocket>();
        uploader = std::make_unique<FnThread> ("EnsayoProjectUpload", [this, gen] (FnThread& t) { uploadLoop (t, gen); });
        uploader->startThread();
        data = std::make_unique<FnThread> ("EnsayoProjectData", [this, gen, dataHello] (FnThread& t) { dataLoop (t, gen, dataHello); });
        data->startThread();
        startUdp();

        if (listening.load())
            sendLine (object ({ { "t", "listen" }, { "enabled", true } }));

        if (owner.onWelcomeForTest != nullptr)
            owner.onWelcomeForTest();

        reconcile();
        resendActiveTake();

        if (owner.onProjectChanged != nullptr)
            owner.onProjectChanged();
    }

    void handleDataUp()
    {
        connected = true;

        if (owner.onConnected != nullptr)
            owner.onConnected();

        if (owner.onTransfersChanged != nullptr)
            owner.onTransfersChanged();
    }

    void requestResync()
    {
        if (resyncing)
            return;

        resyncing = true;
        resyncAttempts = 1;
        sendLine (makeMessage ("resync"));
    }

    void handleApplied (const juce::var& m)
    {
        if (replica == nullptr || resyncing)
            return;   // the `project` answering the resync supersedes everything before it

        const auto version = readInteger (m["version"], 1, INT64_MAX);

        if (! version || *version != replica->version + 1)
        {
            requestResync();
            return;
        }

        Project next = *replica;
        const auto& op = m["op"];

        if (! applyOp (next, op, m["by"].toString()).ok || next.version != *version)
        {
            requestResync();
            return;
        }

        replica = std::make_shared<const Project> (std::move (next));
        reconcile();

        if (owner.onProjectChanged != nullptr)
            owner.onProjectChanged();

        const auto name = op["op"].toString();

        if ((name == "update_take" || name == "add_take") && owner.onTransfersChanged != nullptr)
            owner.onTransfersChanged();
    }

    void handleProject (const juce::var& m)
    {
        juce::String error;
        auto parsed = projectFromVar (m["project"], error);

        if (! parsed)
        {
            if (resyncAttempts++ < maxResyncAttempts)
            {
                resyncing = true;
                sendLine (makeMessage ("resync"));
            }
            else
            {
                lost (utf8 ("El anfitri\xc3\xb3n envi\xc3\xb3 un proyecto inv\xc3\xa1lido"));
            }

            return;
        }

        replica = std::make_shared<const Project> (std::move (*parsed));
        resyncing = false;
        resyncAttempts = 0;
        reconcile();

        if (owner.onProjectChanged != nullptr)
            owner.onProjectChanged();

        if (owner.onTransfersChanged != nullptr)
            owner.onTransfersChanged();
    }

    void handleRejected (const juce::var& m)
    {
        const auto seq = (int) readInteger (m["seq"], INT_MIN, INT_MAX).value_or (0);
        const auto reason = m["reason"].toString();

        if (const auto it = regionSeqs.find (seq); it != regionSeqs.end())
        {
            // The region cannot be added (track deleted meanwhile, …): the take is on the host; stop trying.
            const std::lock_guard<std::mutex> scoped (lock);
            dropPendingLocked (it->second);
            regionSeqs.erase (it);
        }

        if (owner.onOpRejected != nullptr)
            owner.onOpRejected (seq, reason);

        if (owner.onRejected != nullptr)
            owner.onRejected (reason);
    }

    void handlePresence (const juce::var& m)
    {
        std::vector<Presence> users;
        std::map<Id, std::pair<Id, juce::var>> recordingByTake;   // take → (user, recording)

        if (auto* array = m["users"].getArray())
        {
            for (const auto& u : *array)
            {
                Presence p;
                p.user = u["id"].toString();
                p.name = u["name"].toString();
                p.online = (bool) u["online"];
                users.push_back (p);

                const auto& rec = u["recording"];

                if (p.online && rec.isObject() && p.user != config.user)
                    recordingByTake[rec["take"].toString()] = { p.user, rec };
            }
        }

        presenceList = std::move (users);

        // Presence is authoritative for who is recording: joiners learn recordings in progress, and a recorder that
        // dropped out disappears.
        live.erase (std::remove_if (live.begin(), live.end(), [&] (const LiveRecording& r)
        {
            const auto it = recordingByTake.find (r.take);
            return it == recordingByTake.end() || it->second.first != r.user;
        }), live.end());

        for (const auto& [take, entry] : recordingByTake)
        {
            if (std::any_of (live.begin(), live.end(), [&take] (const LiveRecording& r) { return r.take == take; }))
                continue;

            LiveRecording r;
            r.user = entry.first;
            r.take = take;
            r.track = entry.second["track"].toString();
            r.start = readInteger (entry.second["start"], 0, INT64_MAX / 4).value_or (0);
            live.push_back (std::move (r));
        }

        if (owner.onPresenceChanged != nullptr)
            owner.onPresenceChanged();
    }

    void handleRec (const juce::var& m)
    {
        const auto type = m["t"].toString();
        const auto take = m["take"].toString();
        const auto user = m["user"].toString();

        if (user == config.user)
            return;

        auto it = std::find_if (live.begin(), live.end(), [&take] (const LiveRecording& r) { return r.take == take; });

        if (type == "rec_start")
        {
            if (it == live.end())
            {
                live.push_back ({});
                it = live.end() - 1;
            }

            it->user = user;
            it->take = take;
            it->track = m["track"].toString();
            it->start = readInteger (m["start"], 0, INT64_MAX / 4).value_or (0);
            it->peaks.clear();
        }
        else if (type == "rec_stop")
        {
            if (it != live.end())
                live.erase (it);
        }
        else if (it != live.end())   // rec_peaks
        {
            const auto from = readInteger (m["from"], 0, maxPeakIndex);
            const auto* peaks = m["peaks"].getArray();

            if (! from || peaks == nullptr || *from + peaks->size() > maxPeakIndex)
                return;

            if ((int64_t) it->peaks.size() < *from + peaks->size())
                it->peaks.resize ((size_t) (*from + peaks->size()));

            for (int i = 0; i < peaks->size(); ++i)
            {
                const auto& pair = (*peaks)[i];
                it->peaks[(size_t) (*from + i)] = { (float) (double) pair[0], (float) (double) pair[1] };
            }

            return;   // peaks are polled; only start/stop notify
        }

        if (owner.onPresenceChanged != nullptr)
            owner.onPresenceChanged();
    }

    //==========================================================================
    // Data thread.
    void dataLoop (FnThread& t, int gen, const juce::String& dataHello)
    {
        if (! dataSocket->connect (link.host, link.port, connectTimeoutMs)
             || ! writeAll (*dataSocket, dataHello.toRawUTF8(), dataHello.getNumBytesAsUTF8(), &t))
        {
            if (! t.threadShouldExit())
                postLost (gen, utf8 ("No se pudo abrir la conexi\xc3\xb3n de datos"));

            return;
        }

        dataReady.store (true);
        uploadWake.signal();
        post (gen, [this] { handleDataUp(); });

        juce::uint32 lastRead = 0;
        const auto end = readLines (*dataSocket, t, gen, lastRead,
            [this, gen] (const juce::var& m) -> int64_t { return handleDataLine (gen, m); },
            [] { return true; });

        if (! t.threadShouldExit())
            postLost (gen, end == ReadEnd::closed ? utf8 ("Se perdi\xc3\xb3 la conexi\xc3\xb3n de datos")
                                 : utf8 ("El anfitri\xc3\xb3n envi\xc3\xb3 datos inv\xc3\xa1lidos"));
    }

    // Returns the payload size to read after this line, 0 for none, -1 to drop the connection.
    int64_t handleDataLine (int gen, const juce::var& m)
    {
        const auto type = m["t"].toString();

        if (type == "segment")
            return readInteger (m["bytes"], 1, maxSegmentBytes).value_or (-1);

        if (type == "segment_ok")       handleSegmentOk (gen, m["take"].toString(), (int) readInteger (m["index"], 0, INT_MAX).value_or (-1));
        else if (type == "segment_bad") handleSegmentBad (gen, m);
        else if (type == "error")       post (gen, [this, m] { hostError = m["message"].toString(); });

        return 0;
    }

    // False drops the data connection: a corrupt segment, or one the cache cannot take (a gap), would otherwise
    // leave the take short forever. The next data_hello's `have` resumes exactly at the cached count.
    bool handleDownloaded (int gen, const juce::var& header, const juce::MemoryBlock& payload)
    {
        downloaded.fetch_add ((int64_t) payload.getSize());

        const auto take = header["take"].toString();
        const auto index = readInteger (header["index"], 0, INT_MAX);

        if (! isValidId (take) || ! index || Sha1::of (payload.getData(), payload.getSize()) != header["sha1"].toString().toLowerCase())
            return false;

        TakeCache* cachePtr = nullptr;

        {
            const std::lock_guard<std::mutex> scoped (lock);
            cachePtr = takeCache.get();
        }

        if (cachePtr != nullptr && ! cachePtr->appendSegment (take, (int) *index, payload.getData(), payload.getSize())
             && cachePtr->segmentCount (take) <= (int) *index)
            return false;   // refused and not already cached

        postTransfers (gen);
        return true;
    }

    void handleSegmentOk (int gen, const Id& take, int index)
    {
        {
            const std::lock_guard<std::mutex> scoped (lock);
            inFlight = false;

            if (auto* p = findPending (take); p != nullptr && p->hostReady && index == p->nextIndex)
            {
                segmentFile (take, index).deleteFile();
                ++p->nextIndex;
                p->failures = 0;
                maybeCompleteLocked (*p);
            }
        }

        uploadWake.signal();
        postTransfers (gen);
    }

    void handleSegmentBad (int gen, const juce::var& m)
    {
        const auto take = m["take"].toString();
        const auto expected = (int) readInteger (m["expected"], 0, INT_MAX).value_or (0);
        const auto reason = m["reason"].toString();
        bool reconcileAfter = false;
        juce::String failure;

        {
            const std::lock_guard<std::mutex> scoped (lock);
            inFlight = false;
            auto* p = findPending (take);

            if (p == nullptr)
                return;

            if (reason == "toma desconocida")
            {
                p->hostReady = false;
                p->addTakeSent = false;
                reconcileAfter = true;

                if (++p->failures >= maxUploadFailures)
                    p->failed = true;
            }
            else if (reason == "la toma ya esta completa")
            {
                p->nextIndex = p->queued;   // nothing more can be stored; reconcile handles the region
                reconcileAfter = true;
            }
            else if (expected != p->nextIndex)
            {
                // Resume where the host is (a lower index is re-read from the cache if its pending file is gone).
                if (expected > p->nextIndex)
                    deleteSegmentFilesLocked (*p, p->nextIndex, expected);

                p->nextIndex = expected;
                p->queued = std::max (p->queued, expected);
                maybeCompleteLocked (*p);
            }
            else if (++p->failures >= maxUploadFailures)
            {
                p->failed = true;
            }

            if (p->failed)
                failure = "No se pudo subir la toma: " + reason;
        }

        if (failure.isNotEmpty())
            postError (gen, failure);

        if (reconcileAfter)
            post (gen, [this] { reconcile(); });

        uploadWake.signal();
    }

    //==========================================================================
    // Uploader thread: the only writer of the data socket after data_hello.
    void uploadLoop (FnThread& t, int gen)
    {
        while (! t.threadShouldExit())
        {
            Id take;
            int index = -1;
            juce::File file;
            TakeCache* cachePtr = nullptr;

            if (dataReady.load())
            {
                const std::lock_guard<std::mutex> scoped (lock);

                if (! inFlight)
                {
                    for (auto& p : pendings)
                    {
                        if (p.hostReady && ! p.failed && ! p.completeSent && p.nextIndex < p.queued)
                        {
                            take = p.take;
                            index = p.nextIndex;
                            file = segmentFile (take, index);
                            cachePtr = takeCache.get();
                            inFlight = true;
                            break;
                        }
                    }
                }
            }

            if (index < 0)
            {
                uploadWake.wait (100);
                continue;
            }

            juce::MemoryBlock bytes;

            if (! file.loadFileAsData (bytes) || bytes.isEmpty())
                bytes = cachePtr != nullptr ? cachePtr->segmentBytes (take, index) : juce::MemoryBlock();

            juce::AudioBuffer<float> decoded;

            if (bytes.isEmpty() || ! decodeSegment (bytes.getData(), bytes.getSize(), decoded) || decoded.getNumSamples() == 0)
            {
                {
                    const std::lock_guard<std::mutex> scoped (lock);
                    inFlight = false;

                    if (auto* p = findPending (take))
                        p->failed = true;
                }

                postError (gen, "No se pudo leer el segmento " + juce::String (index) + " de la toma " + take);
                continue;
            }

            const auto line = makeLine (object ({ { "t", "segment" }, { "take", take }, { "index", index },
                                                  { "samples", decoded.getNumSamples() }, { "channels", decoded.getNumChannels() },
                                                  { "bytes", (juce::int64) bytes.getSize() },
                                                  { "sha1", Sha1::of (bytes.getData(), bytes.getSize()) } }));

            if (! writeAll (*dataSocket, line.toRawUTF8(), line.getNumBytesAsUTF8(), &t)
                 || ! writeAll (*dataSocket, bytes.getData(), bytes.getSize(), &t))
                return;   // the data reader reports the broken connection
        }
    }

    //==========================================================================
    // UDP: receive thread (this one) plus a nested send thread.
    void startUdp()
    {
       #if JUCE_WINDOWS
        const auto host = link.host;
        const auto port = udpPort;
        udp = std::make_unique<FnThread> ("EnsayoProjectUdp", [this, host, port] (FnThread& t) { udpLoop (t, host, port); });
        udp->startThread (juce::Thread::Priority::highest);
       #endif
    }

    void stopUdp()
    {
        if (udp == nullptr)
            return;

        udp->signalThreadShouldExit();
        closeUdpSocket();
        udp = nullptr;
    }

    void closeUdpSocket()
    {
       #if JUCE_WINDOWS
        const SOCKET s = udpSocket.exchange (INVALID_SOCKET);

        if (s != INVALID_SOCKET)
            closesocket (s);
       #endif
    }

   #if JUCE_WINDOWS
    void udpLoop (FnThread& t, const juce::String& host, int port)
    {
        WSADATA wsaData;

        if (WSAStartup (MAKEWORD (2, 2), &wsaData) != 0)
            return;

        sockaddr_in server {};
        addrinfo hints {};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* result = nullptr;

        if (getaddrinfo (host.toRawUTF8(), nullptr, &hints, &result) != 0 || result == nullptr)
        {
            WSACleanup();
            return;
        }

        std::memcpy (&server, result->ai_addr, sizeof (sockaddr_in));
        server.sin_port = htons ((u_short) port);
        freeaddrinfo (result);

        const SOCKET sock = ::socket (AF_INET, SOCK_DGRAM, IPPROTO_UDP);

        if (sock == INVALID_SOCKET)
        {
            WSACleanup();
            return;
        }

        udpSocket.store (sock);

        if (t.threadShouldExit())
        {
            closeUdpSocket();
            WSACleanup();
            return;
        }

        BOOL disable = FALSE;
        DWORD bytesReturned = 0;
        WSAIoctl (sock, SIO_UDP_CONNRESET, &disable, sizeof (disable), nullptr, 0, &bytesReturned, nullptr, nullptr);

        const int bufferBytes = 256 * 1024;
        setsockopt (sock, SOL_SOCKET, SO_RCVBUF, (const char*) &bufferBytes, sizeof (bufferBytes));
        setsockopt (sock, SOL_SOCKET, SO_SNDBUF, (const char*) &bufferBytes, sizeof (bufferBytes));

        sockaddr_in local {};
        local.sin_family = AF_INET;
        local.sin_addr.s_addr = htonl (INADDR_ANY);

        if (::bind (sock, (const sockaddr*) &local, sizeof (local)) != 0)
        {
            closeUdpSocket();
            WSACleanup();
            return;
        }

        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);

        {
            FnThread sender ("EnsayoProjectUdpSend", [this, server] (FnThread& st) { udpSendLoop (st, server); });
            sender.startThread (juce::Thread::Priority::highest);

            std::vector<uint8_t> incoming (2048);

            while (! t.threadShouldExit())
            {
                sockaddr_in from {};
                int fromLen = sizeof (from);
                const int n = recvfrom (sock, (char*) incoming.data(), (int) incoming.size(), 0, (sockaddr*) &from, &fromLen);

                if (n < 0)
                {
                    if (t.threadShouldExit())
                        break;

                    if (WSAGetLastError() != WSAEMSGSIZE)
                        juce::Thread::sleep (1);

                    continue;
                }

                AudioPacketView view;

                if (n == 0 || ! decodeAudioPacket (incoming.data(), (size_t) n, view))
                    continue;

                const auto* payload = view.voiceLen > 0 ? view.voice : view.instrument;
                const auto len = view.voiceLen > 0 ? view.voiceLen : view.instrumentLen;
                preview.push (view.seq, payload, len, juce::Time::getMillisecondCounterHiRes());
            }

            sender.signalThreadShouldExit();
            SetEvent ((HANDLE) sendWake);
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);

        closeUdpSocket();
        WSACleanup();
    }

    void udpSendLoop (FnThread& t, sockaddr_in server)
    {
        DWORD taskIndex = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW (L"Pro Audio", &taskIndex);
        std::vector<uint8_t> packet (maxPacketBytes);
        auto nextKeepalive = juce::Time::getMillisecondCounter();

        while (! t.threadShouldExit())
        {
            const SOCKET sock = udpSocket.load();
            const auto id = clientId.load (std::memory_order_relaxed);
            size_t len = 0;

            while (outgoing.pop (packet.data(), len))
            {
                if (sock == INVALID_SOCKET || len < audioHeaderBytes || packet[0] != packetMagic)
                    continue;

                for (int i = 0; i < 4; ++i)   // the client stamps its own id (u32 little-endian)
                    packet[(size_t) (clientIdOffset + i)] = (uint8_t) (id >> (8 * i));

                sendto (sock, (const char*) packet.data(), (int) len, 0, (const sockaddr*) &server, sizeof (server));
            }

            const auto now = juce::Time::getMillisecondCounter();

            if (listening.load() && sock != INVALID_SOCKET && id != 0 && now >= nextKeepalive)
            {
                const uint8_t keepalive[5] = { 0x00, (uint8_t) id, (uint8_t) (id >> 8), (uint8_t) (id >> 16), (uint8_t) (id >> 24) };
                sendto (sock, (const char*) keepalive, (int) sizeof (keepalive), 0, (const sockaddr*) &server, sizeof (server));
                nextKeepalive = now + (juce::uint32) keepaliveIntervalMs;
            }

            WaitForSingleObject ((HANDLE) sendWake, 100);
        }

        if (mmcss != nullptr)
            AvRevertMmThreadCharacteristics (mmcss);
    }
   #endif
};

//==============================================================================
ProjectClient::ProjectClient (ClientConfig c) : impl (std::make_unique<Impl> (*this, std::move (c))) {}
ProjectClient::~ProjectClient() = default;

void ProjectClient::connect (const SessionLink& link)   { impl->connect (link); }

void ProjectClient::disconnect()
{
    // Ops sent just before leaving (a finish, a rename) must reach the host: give the writer a moment to flush.
    if (impl->welcomed)
    {
        std::shared_ptr<LineWriter> w;

        {
            const std::lock_guard<std::mutex> scoped (impl->writerLock);
            w = impl->writer;
        }

        if (w != nullptr)
            w->drain (500);
    }

    ++(*impl->generation);   // queued callbacks of this connection are dropped
    impl->teardown();
}

bool ProjectClient::isConnected() const noexcept   { return impl->connected; }
bool ProjectClient::isHost() const noexcept        { return impl->hostFlag; }
const Id& ProjectClient::user() const noexcept     { return impl->config.user; }
std::shared_ptr<const Project> ProjectClient::project() const   { return impl->replica; }

TakeCache* ProjectClient::cache() noexcept
{
    const std::lock_guard<std::mutex> scoped (impl->lock);
    return impl->takeCache.get();
}

int ProjectClient::sendOp (const juce::var& op)
{
    if (! impl->welcomed)
    {
        if (onRejected != nullptr)
            onRejected (utf8 ("Sin conexi\xc3\xb3n con el anfitri\xc3\xb3n"));

        return 0;
    }

    return impl->sendOpInternal (op);
}

void ProjectClient::beginTake (const Id& take, int channels, const Id& track, int64_t timelineStart, int64_t takeOffset,
                               bool announce)
{
    bool sendAddTake = false;

    {
        const std::lock_guard<std::mutex> scoped (impl->lock);

        if (impl->projectId.isEmpty())
        {
            impl->postError ("No hay proyecto: no se puede grabar");
            return;
        }

        auto& p = impl->findOrCreateLocked (take);
        p.channels = channels;
        p.recTrack = track;
        p.recStart = timelineStart;
        p.recOffset = std::max<int64_t> (0, takeOffset);
        impl->writeMetaLocked (p);

        if (impl->welcomed && ! p.addTakeSent && ! p.hostReady)
        {
            p.addTakeSent = true;
            sendAddTake = true;
        }
    }

    if (sendAddTake)
        impl->sendOpInternal (Ops::addTake (take, channels));

    if (! announce)
        return;

    const std::lock_guard<std::mutex> scoped (impl->activeLock);
    impl->activeTake = take;
    impl->activeTrack = track;
    impl->activeStart = timelineStart;
    impl->activePeaks.clear();

    if (impl->welcomed)
        impl->sendLine (object ({ { "t", "rec_start" }, { "take", take }, { "track", track },
                                        { "start", (juce::int64) timelineStart } }));
}

void ProjectClient::sendPeaks (const Id& take, int64_t fromPeak, const std::vector<Peak>& peaks)
{
    if (fromPeak < 0)
        return;

    const std::lock_guard<std::mutex> scoped (impl->activeLock);

    if (take == impl->activeTake)
    {
        const auto end = (size_t) fromPeak + peaks.size();

        if (impl->activePeaks.size() < end)
            impl->activePeaks.resize (end);

        std::copy (peaks.begin(), peaks.end(), impl->activePeaks.begin() + (std::ptrdiff_t) fromPeak);
    }

    if (impl->canSend.load())
        impl->sendPeakLines (take, fromPeak, peaks.data(), peaks.size());
}

void ProjectClient::queueSegment (const Id& take, int index, const juce::MemoryBlock& flac)
{
    bool created = false;

    {
        const std::lock_guard<std::mutex> scoped (impl->lock);

        if (impl->projectId.isEmpty() || impl->takeCache == nullptr)
        {
            impl->postError ("No hay proyecto: no se puede guardar la toma");
            return;
        }

        created = impl->findPending (take) == nullptr;
        auto& p = impl->findOrCreateLocked (take);
        const auto where = " (segmento " + juce::String (index) + " de la toma " + take + ")";

        if (p.finish)
        {
            impl->postError (utf8 ("La toma ya est\xc3\xa1 terminada: se descarta un segmento") + where);
            return;
        }

        if (index < p.queued)
        {
            impl->postError ("Segmento repetido: se ignora" + where);
            return;
        }

        impl->takeDir (take).createDirectory();

        if (! impl->segmentFile (take, index).replaceWithData (flac.getData(), flac.getSize()))
        {
            impl->postError ("No se pudo guardar el segmento" + where);
            return;
        }

        if (index > p.queued)
        {
            // Kept on disk; it joins the take once the missing segments arrive.
            impl->postError ("Segmento fuera de orden: queda guardado hasta que lleguen los anteriores" + where);
            return;
        }

        if (! impl->absorbLocked (p, flac.getData(), flac.getSize()))
            impl->postError (utf8 ("No se pudo a\xc3\xb1" "adir a la cach\xc3\xa9 local; se sube igualmente") + where);

        if (p.channels == 0)
        {
            p.channels = impl->takeCache->channels (take);
            impl->writeMetaLocked (p);
        }
    }

    impl->uploadWake.signal();
    impl->postTransfers();

    if (created)   // queued without beginTake: add_take is sent by reconcile
        impl->post ([i = impl.get()] { i->reconcile(); });
}

void ProjectClient::finishTake (const Id& take, const Id& track, const Region& region)
{
    impl->clearActive (take);

    {
        const std::lock_guard<std::mutex> scoped (impl->lock);

        if (impl->projectId.isEmpty())
        {
            impl->postError ("No hay proyecto: no se puede terminar la toma");
            return;
        }

        auto& p = impl->findOrCreateLocked (take);
        p.finish = Impl::Finish { track, region };
        impl->writeMetaLocked (p);
        impl->maybeCompleteLocked (p);
    }

    impl->reconcile();
}

void ProjectClient::setListening (bool enabled)
{
    impl->listening.store (enabled);

    if (impl->welcomed)
        impl->sendLine (object ({ { "t", "listen" }, { "enabled", enabled } }));

   #if JUCE_WINDOWS
    if (impl->sendWake != nullptr)
        SetEvent ((HANDLE) impl->sendWake);
   #endif
}

void ProjectClient::sendPreview (const uint8_t* packet, size_t len) noexcept
{
    if (! impl->outgoing.push (packet, len))
        return;

   #if JUCE_WINDOWS
    if (impl->sendWake != nullptr)
        SetEvent ((HANDLE) impl->sendWake);
   #endif
}

std::vector<UnfinishedTake> ProjectClient::unfinishedTakes() const
{
    std::vector<UnfinishedTake> out;
    const std::lock_guard<std::mutex> scoped (impl->lock);

    for (const auto& p : impl->pendings)
    {
        if (p.finish || p.failed)
            continue;

        UnfinishedTake u;
        u.take = p.take;
        u.track = p.recTrack;
        u.start = p.recStart;
        u.offset = p.recOffset;
        u.channels = p.channels;
        u.samples = impl->takeCache != nullptr ? impl->takeCache->sampleCount (p.take) : 0;
        out.push_back (u);
    }

    return out;
}

void ProjectClient::discardTake (const Id& take)
{
    impl->clearActive (take);

    {
        const std::lock_guard<std::mutex> scoped (impl->lock);

        if (impl->projectId.isNotEmpty() && impl->findPending (take) != nullptr)
            impl->dropPendingLocked (take);
    }

    // Nobody may keep showing it as recording. The take itself (if add_take was applied) stays empty and incomplete,
    // with no region.
    if (impl->welcomed)
        impl->sendLine (object ({ { "t", "rec_stop" }, { "take", take } }));

    impl->postTransfers();
}

JitterBuffer* ProjectClient::previewBuffer() noexcept   { return &impl->preview; }
std::vector<Presence> ProjectClient::presence() const   { return impl->presenceList; }
std::vector<LiveRecording> ProjectClient::liveRecordings() const   { return impl->live; }

Transfers ProjectClient::transfers() const
{
    Transfers t;
    const std::lock_guard<std::mutex> scoped (impl->lock);

    for (const auto& p : impl->pendings)
    {
        if (! p.failed)
            t.pendingUploads += std::max (0, p.queued - p.nextIndex);

        ++t.pendingTakes;
    }

    if (impl->replica != nullptr && impl->takeCache != nullptr)
        for (const auto& take : impl->replica->takes)
            if (impl->takeCache->segmentCount (take.id) < take.segments)
                ++t.downloadingTakes;

    return t;
}

int ProjectClient::pingMs() const noexcept             { return impl->ping.load(); }
int64_t ProjectClient::bytesDownloaded() const noexcept { return impl->downloaded.load(); }

void ProjectClient::dropConnectionForTest()
{
   #if JUCE_WINDOWS
    for (auto* socket : { impl->controlSocket.get(), impl->dataSocket.get() })
        if (socket != nullptr && socket->getRawSocketHandle() >= 0)
            ::shutdown ((SOCKET) socket->getRawSocketHandle(), SD_BOTH);   // the readers see EOF: the unclean path
   #endif
}
}
