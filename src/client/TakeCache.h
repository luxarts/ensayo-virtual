#pragma once
#include "core/Project.h"
#include "core/Segments.h"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
namespace ensayo
{
// Read side used by the renderer (audio thread).
class SampleSource
{
public:
    virtual ~SampleSource() = default;
    virtual int channels (const Id& take) const noexcept = 0;                       // 0 = unknown take
    // Copies samples [offset, offset + n) of `channel` (clamped to the take's last channel) into dest.
    // Samples not received yet, or outside the take, are written as 0. Never blocks, never allocates.
    virtual void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept = 0;
};

class TakeCache : public SampleSource
{
public:
    static constexpr int maxChunks = 4 * 3600;   // 4 h of 1 s chunks per take (pointer table preallocated)

    explicit TakeCache (juce::File directory);  // <cache>/<project>/takes ; created if missing
    ~TakeCache() override;

    // Loads every take found on disk (segments n.flac in order, stops at the first gap). Message thread.
    void loadFromDisk();
    // Any non-audio thread, serialised internally. `index` must equal segmentCount(take) or the call is ignored
    // and returns false. Decodes, appends to RAM, writes <dir>/<take>/<index>.flac, extends peaks.
    bool appendSegment (const Id& take, int index, const void* flac, size_t size);
    int segmentCount (const Id& take) const;
    int64_t sampleCount (const Id& take) const;
    std::vector<Peak> peaks (const Id& take) const;
    juce::MemoryBlock segmentBytes (const Id& take, int index) const;   // from disk; empty if missing
    std::vector<Id> takes() const;

    int channels (const Id& take) const noexcept override;
    void read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept override;

    std::function<void (const Id& take)> onTakeChanged;   // after each append, on the appending thread

private:
    struct TakeData;                                       // chunk pointer table (atomic), channel count, sample count
    std::shared_ptr<TakeData> find (const Id&) const noexcept;
    juce::File dir;
    mutable std::mutex writeLock;                          // appenders only; readers never take it
    std::atomic<std::shared_ptr<const std::map<Id, std::shared_ptr<TakeData>>>> index;   // copy-on-write
};
}
