#include "client/TakeCache.h"

#include <algorithm>
#include <cstring>

namespace ensayo
{
// Audio-thread state: the chunk pointer tables and `samples`. Every chunk is segmentSamples long, so sample p
// lives in chunk p / segmentSamples; only the last segment of a take may fill its chunk partially.
// Appender state (`segments`, `closed`, `peaks`) is guarded by TakeCache::writeLock.
struct TakeCache::TakeData
{
    using Map = std::map<Id, std::shared_ptr<TakeData>>;

    explicit TakeData (int numChannelsIn) : numChannels (numChannelsIn)
    {
        for (int ch = 0; ch < numChannels; ++ch)
            chunks[ch] = std::make_unique<std::atomic<float*>[]> ((size_t) maxChunks);   // value-initialised: nullptr
    }

    ~TakeData()
    {
        for (auto& table : chunks)
            if (table != nullptr)
                for (int i = 0; i < maxChunks; ++i)
                    delete[] table[i].load (std::memory_order_relaxed);
    }

    const int numChannels;
    std::unique_ptr<std::atomic<float*>[]> chunks[2];
    std::atomic<int64_t> samples { 0 };   // published (release) after the chunk data and pointers

    int segments = 0;
    bool closed = false;                  // a short segment arrived: the take is complete
    std::vector<Peak> peaks;

    // Keeps the index map this take's insertion replaced alive. Readers may still hold that map; without this
    // their copy could be the last reference and the map would be freed on the audio thread.
    std::shared_ptr<const Map> replacedIndex;

    JUCE_DECLARE_NON_COPYABLE (TakeData)
};

namespace
{
juce::File segmentFile (const juce::File& dir, const Id& take, int index)
{
    return dir.getChildFile (take).getChildFile (juce::String (index) + ".flac");
}

// Copies `audio` into a new chunk, appends its peaks and publishes it. Caller holds writeLock and has checked
// that the take is open, the channel count matches and the chunk table has room.
template <typename Data>
void appendDecoded (Data& take, const juce::AudioBuffer<float>& audio)
{
    const int n = audio.getNumSamples();
    const int64_t before = take.samples.load (std::memory_order_relaxed);
    const int chunk = take.segments;

    for (int ch = 0; ch < take.numChannels; ++ch)
    {
        auto* data = new float[(size_t) segmentSamples];
        std::memcpy (data, audio.getReadPointer (ch), (size_t) n * sizeof (float));
        std::fill (data + n, data + segmentSamples, 0.0f);
        take.chunks[ch][chunk].store (data, std::memory_order_release);
    }

    // Peaks cover fixed peakBlock windows of the whole take, so first top up a partial last peak.
    const int partial = (int) (before % peakBlock);
    const int topUp = partial == 0 ? 0 : std::min (n, peakBlock - partial);

    if (topUp > 0 && ! take.peaks.empty())
    {
        auto& last = take.peaks.back();

        for (int ch = 0; ch < take.numChannels; ++ch)
            for (int i = 0; i < topUp; ++i)
            {
                last.min = std::min (last.min, audio.getSample (ch, i));
                last.max = std::max (last.max, audio.getSample (ch, i));
            }
    }

    if (n > topUp)
    {
        const juce::AudioBuffer<float> rest (const_cast<float* const*> (audio.getArrayOfReadPointers()),
                                             audio.getNumChannels(), topUp, n - topUp);
        appendPeaks (rest, n - topUp, take.peaks);
    }

    ++take.segments;
    take.closed = n < segmentSamples;
    take.samples.store (before + n, std::memory_order_release);
}
}

TakeCache::TakeCache (juce::File directory) : dir (std::move (directory))
{
    dir.createDirectory();
    index.store (std::make_shared<const TakeData::Map>(), std::memory_order_release);
}

TakeCache::~TakeCache()
{
    // Unwinds the chain of replaced maps; every TakeData (and its chunks) is freed here.
    index.store (nullptr);
}

std::shared_ptr<TakeCache::TakeData> TakeCache::find (const Id& take) const noexcept
{
    const auto map = index.load (std::memory_order_acquire);

    if (map == nullptr)
        return {};

    const auto it = map->find (take);
    return it == map->end() ? nullptr : it->second;
}

void TakeCache::loadFromDisk()
{
    std::lock_guard<std::mutex> lock (writeLock);
    const auto current = index.load (std::memory_order_acquire);
    auto next = std::make_shared<TakeData::Map> (*current);
    std::shared_ptr<TakeData> first;

    for (const auto& takeDir : dir.findChildFiles (juce::File::findDirectories, false))
    {
        const Id take = takeDir.getFileName();

        if (! isValidId (take) || next->count (take) != 0)
            continue;

        std::shared_ptr<TakeData> data;

        for (int i = 0; i < maxChunks; ++i)
        {
            juce::MemoryBlock bytes;
            juce::AudioBuffer<float> audio;

            if (! segmentFile (dir, take, i).loadFileAsData (bytes)
                || ! decodeSegment (bytes.getData(), bytes.getSize(), audio)
                || audio.getNumSamples() == 0)
                break;

            if (data == nullptr)
                data = std::make_shared<TakeData> (audio.getNumChannels());
            else if (audio.getNumChannels() != data->numChannels || data->closed)
                break;

            appendDecoded (*data, audio);
        }

        if (data == nullptr)
            continue;

        if (first == nullptr)
            first = data;

        next->emplace (take, std::move (data));
    }

    if (first == nullptr)
        return;

    first->replacedIndex = current;
    index.store (std::move (next), std::memory_order_release);
}

bool TakeCache::appendSegment (const Id& take, int segmentIndex, const void* flac, size_t size)
{
    if (! isValidId (take) || segmentIndex < 0 || segmentIndex >= maxChunks)
        return false;

    juce::AudioBuffer<float> audio;

    if (! decodeSegment (flac, size, audio) || audio.getNumSamples() == 0)
        return false;

    {
        std::lock_guard<std::mutex> lock (writeLock);
        auto data = find (take);
        const bool isNew = data == nullptr;

        if (isNew ? segmentIndex != 0
                  : (segmentIndex != data->segments || data->closed || audio.getNumChannels() != data->numChannels))
            return false;

        const auto file = segmentFile (dir, take, segmentIndex);

        if (! file.getParentDirectory().createDirectory() || ! file.replaceWithData (flac, size))
            return false;

        if (isNew)
            data = std::make_shared<TakeData> (audio.getNumChannels());

        appendDecoded (*data, audio);

        if (isNew)
        {
            const auto current = index.load (std::memory_order_acquire);
            auto next = std::make_shared<TakeData::Map> (*current);
            next->emplace (take, data);
            data->replacedIndex = current;
            index.store (std::move (next), std::memory_order_release);
        }
    }

    if (onTakeChanged)
        onTakeChanged (take);

    return true;
}

int TakeCache::segmentCount (const Id& take) const
{
    std::lock_guard<std::mutex> lock (writeLock);
    const auto data = find (take);
    return data == nullptr ? 0 : data->segments;
}

int64_t TakeCache::sampleCount (const Id& take) const
{
    const auto data = find (take);
    return data == nullptr ? 0 : data->samples.load (std::memory_order_acquire);
}

std::vector<Peak> TakeCache::peaks (const Id& take) const
{
    std::lock_guard<std::mutex> lock (writeLock);
    const auto data = find (take);
    return data == nullptr ? std::vector<Peak>() : data->peaks;
}

juce::MemoryBlock TakeCache::segmentBytes (const Id& take, int segmentIndex) const
{
    // Only segments that are part of the take: a stray file after a gap on disk is never served.
    if (segmentIndex < 0 || segmentIndex >= segmentCount (take))
        return {};

    juce::MemoryBlock bytes;

    if (! segmentFile (dir, take, segmentIndex).loadFileAsData (bytes))
        return {};

    return bytes;
}

std::vector<Id> TakeCache::takes() const
{
    std::vector<Id> result;

    if (const auto map = index.load (std::memory_order_acquire))
        for (const auto& [take, data] : *map)
            result.push_back (take);

    return result;
}

int TakeCache::channels (const Id& take) const noexcept
{
    const auto data = find (take);
    return data == nullptr ? 0 : data->numChannels;
}

void TakeCache::read (const Id& take, int channel, int64_t offset, int n, float* dest) const noexcept
{
    if (dest == nullptr || n <= 0)
        return;

    const auto data = find (take);

    if (data == nullptr)
    {
        std::fill (dest, dest + n, 0.0f);
        return;
    }

    const auto& table = data->chunks[std::clamp (channel, 0, data->numChannels - 1)];
    const int64_t available = data->samples.load (std::memory_order_acquire);
    int done = 0;

    // Leading samples before the take start.
    if (offset < 0)
    {
        const int zeros = offset <= -(int64_t) n ? n : (int) -offset;
        std::fill (dest, dest + zeros, 0.0f);
        done = zeros;
    }

    while (done < n)
    {
        const int64_t pos = offset + done;   // offset + done >= 0 here, and cannot overflow below `available`

        if (pos >= available)
            break;

        const int chunk = (int) (pos / segmentSamples);
        const int within = (int) (pos % segmentSamples);
        const int count = (int) std::min<int64_t> ({ (int64_t) (n - done), (int64_t) (segmentSamples - within),
                                                     available - pos });
        const float* src = table[chunk].load (std::memory_order_acquire);

        if (src != nullptr)
            std::memcpy (dest + done, src + within, (size_t) count * sizeof (float));
        else
            std::fill (dest + done, dest + done + count, 0.0f);   // unreachable: pointers precede `samples`

        done += count;
    }

    std::fill (dest + done, dest + n, 0.0f);
}
}
