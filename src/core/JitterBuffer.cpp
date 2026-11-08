#include "JitterBuffer.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace ensayo
{
namespace
{
    constexpr double evaluationPeriodMs = 1000.0;
    constexpr double ewmaAlpha = 0.05;
    constexpr int downMovesRequired = 3;
}

int JitterBuffer::clampBlocks (int blocks) const noexcept
{
    return std::clamp (blocks, minBlocks, maxBlocks);
}

JitterBuffer::JitterBuffer (int blockBytesToUse, double blockDurationMsToUse)
    : blockBytes (std::max (1, blockBytesToUse)),
      blockDurationMs (blockDurationMsToUse > 0.0 ? blockDurationMsToUse : 1.0),
      maxBlocks (std::clamp ((int) std::ceil (maxBufferMs / blockDurationMs), minBlocks + 1, numSlots / 2)),
      storage ((size_t) blockBytes * (size_t) numSlots, 0)
{
    for (auto& tag : tags)
        tag.store (0, std::memory_order_relaxed);

    for (auto& length : lengths)
        length.store (0, std::memory_order_relaxed);
}

void JitterBuffer::setAutoSize (bool enabled) noexcept
{
    autoSize.store (enabled, std::memory_order_relaxed);
}

void JitterBuffer::setSizeBlocks (int blocks) noexcept
{
    sizeBlocks.store (clampBlocks (blocks), std::memory_order_relaxed);
    windowResetRequest.store (true, std::memory_order_relaxed);
}

int JitterBuffer::getSizeBlocks() const noexcept
{
    return sizeBlocks.load (std::memory_order_relaxed);
}

void JitterBuffer::clearSlots() noexcept
{
    for (auto& tag : tags)
        tag.store (0, std::memory_order_release);
}

void JitterBuffer::resetEstimator() noexcept
{
    estimator = {};
    jitterMs.store (0.0, std::memory_order_relaxed);
}

void JitterBuffer::reset() noexcept
{
    clearSlots();
    hasWindow.store (false, std::memory_order_relaxed);
    pendingResync.store (0, std::memory_order_relaxed);
    highestSeq.store (0, std::memory_order_relaxed);
    primed.store (false, std::memory_order_relaxed);
    underruns.store (0, std::memory_order_relaxed);
    late.store (0, std::memory_order_relaxed);
    resyncs.store (0, std::memory_order_relaxed);
    jitterMs.store (0.0, std::memory_order_relaxed);   // the producer restarts its estimator on the next push
}

void JitterBuffer::push (uint16_t seq, const uint8_t* data, int len, double arrivalMs) noexcept
{
    if (data == nullptr || len <= 0 || len > blockBytes)
        return;

    if (windowResetRequest.exchange (false, std::memory_order_relaxed))
    {
        estimator.windowStarted = false;
        estimator.windowMax = 0.0;
        estimator.downCount = 0;
    }

    if (! hasWindow.load (std::memory_order_relaxed))
    {
        // First packet after construction or reset(): it defines the window.
        hasWindow.store (true, std::memory_order_relaxed);
        pendingResync.store (taggedSeq (seq), std::memory_order_release);
        highestSeq.store (0, std::memory_order_relaxed);
        resetEstimator();
    }
    else
    {
        const uint32_t pending = pendingResync.load (std::memory_order_relaxed);
        const uint16_t base = pending != 0 ? seqOf (pending) : readSeq.load (std::memory_order_acquire);
        const int distance = seqDistance (base, seq);

        if (distance < 0)
        {
            late.fetch_add (1, std::memory_order_relaxed);

            // A single late packet is reordering; a run of them means the consumer ran ahead of the stream
            // (the producer stalled for longer than the buffer) and would otherwise drop every packet from
            // now on. Re-anchor the window on the current packet.
            if (++estimator.consecutiveLate < lateResyncThreshold)
                return;

            resyncs.fetch_add (1, std::memory_order_relaxed);
            clearSlots();
            pendingResync.store (taggedSeq (seq), std::memory_order_release);
            highestSeq.store (0, std::memory_order_relaxed);
        }
        else if (distance >= maxBlocks)
        {
            resyncs.fetch_add (1, std::memory_order_relaxed);
            clearSlots();
            pendingResync.store (taggedSeq (seq), std::memory_order_release);
            highestSeq.store (0, std::memory_order_relaxed);
        }

        estimator.consecutiveLate = 0;
    }

    const size_t slot = slotOf (seq);
    tags[slot].store (0, std::memory_order_release);
    std::memcpy (storage.data() + slot * (size_t) blockBytes, data, (size_t) len);
    lengths[slot].store (len, std::memory_order_relaxed);
    tags[slot].store (taggedSeq (seq), std::memory_order_release);

    const uint32_t highest = highestSeq.load (std::memory_order_relaxed);

    if (highest == 0 || seqDistance (seqOf (highest), seq) > 0)
        highestSeq.store (taggedSeq (seq), std::memory_order_release);

    // Jitter estimator: deviation of the arrival interval from the nominal one, normalised by the
    // number of blocks the sequence advanced.
    auto& e = estimator;

    if (! e.windowStarted)
    {
        e.windowStarted = true;
        e.windowStartMs = arrivalMs;
        e.windowMax = 0.0;
    }

    if (e.hasLast)
    {
        const int advanced = seqDistance (e.lastSeq, seq);

        if (advanced > 0)
        {
            const double expected = advanced * blockDurationMs;
            const double deviation = std::abs ((arrivalMs - e.lastArrivalMs) - expected);
            const double delta = deviation - e.mean;
            e.mean += ewmaAlpha * delta;
            e.variance += ewmaAlpha * (delta * delta - e.variance);
            e.windowMax = std::max (e.windowMax, deviation);
            jitterMs.store (e.mean, std::memory_order_relaxed);
        }
    }

    e.hasLast = true;
    e.lastArrivalMs = arrivalMs;
    e.lastSeq = seq;

    if (arrivalMs - e.windowStartMs >= evaluationPeriodMs)
    {
        if (autoSize.load (std::memory_order_relaxed))
            evaluateTarget();

        e.windowStartMs = arrivalMs;
        e.windowMax = 0.0;
    }
}

void JitterBuffer::evaluateTarget() noexcept
{
    auto& e = estimator;
    const double stddev = std::sqrt (std::max (0.0, e.variance));
    const double neededMs = e.mean + 3.0 * stddev + e.windowMax;
    const int target = clampBlocks ((int) std::ceil (neededMs / blockDurationMs) + 1);
    const int current = sizeBlocks.load (std::memory_order_relaxed);

    if (target > current)
    {
        sizeBlocks.store (target, std::memory_order_relaxed);
        e.downCount = 0;
    }
    else if (target < current)
    {
        if (++e.downCount >= downMovesRequired)
        {
            sizeBlocks.store (target, std::memory_order_relaxed);
            e.downCount = 0;
        }
    }
    else
    {
        e.downCount = 0;
    }
}

bool JitterBuffer::pop (uint8_t* out, int& len) noexcept
{
    len = 0;

    if (out == nullptr)
        return false;

    const uint32_t pending = pendingResync.exchange (0, std::memory_order_acq_rel);

    if (pending != 0)
    {
        readSeq.store (seqOf (pending), std::memory_order_release);
        primed.store (false, std::memory_order_relaxed);
        appliedSize = sizeBlocks.load (std::memory_order_relaxed);
    }

    if (! hasWindow.load (std::memory_order_relaxed))
        return false;

    const int size = sizeBlocks.load (std::memory_order_relaxed);
    uint16_t seq = readSeq.load (std::memory_order_relaxed);

    if (! primed.load (std::memory_order_relaxed))
    {
        const uint32_t highest = highestSeq.load (std::memory_order_acquire);

        if (highest == 0 || seqDistance (seq, seqOf (highest)) + 1 < size)
            return false;

        primed.store (true, std::memory_order_relaxed);
        appliedSize = size;
    }
    else if (size > appliedSize)
    {
        // Deeper buffer requested: hold the read position for one block per extra slot (re-buffering).
        ++appliedSize;
        return false;
    }
    else if (size < appliedSize)
    {
        // Shallower buffer requested: skip ahead so the depth matches immediately.
        seq = (uint16_t) (seq + (uint16_t) (appliedSize - size));
        appliedSize = size;
    }

    const size_t slot = slotOf (seq);
    const uint32_t expected = taggedSeq (seq);
    bool ok = false;

    if (tags[slot].load (std::memory_order_acquire) == expected)
    {
        const int length = std::clamp (lengths[slot].load (std::memory_order_relaxed), 0, blockBytes);
        std::memcpy (out, storage.data() + slot * (size_t) blockBytes, (size_t) length);

        // The producer only rewrites this slot during a resync; a tag change means the copy may be torn.
        if (tags[slot].load (std::memory_order_acquire) == expected)
        {
            tags[slot].store (0, std::memory_order_release);
            len = length;
            ok = true;
        }
    }

    if (! ok)
        underruns.fetch_add (1, std::memory_order_relaxed);

    readSeq.store ((uint16_t) (seq + 1), std::memory_order_release);
    return ok;
}

JitterBuffer::Stats JitterBuffer::getStats() const noexcept
{
    Stats stats;
    stats.sizeBlocks = sizeBlocks.load (std::memory_order_relaxed);
    stats.underruns = underruns.load (std::memory_order_relaxed);
    stats.late = late.load (std::memory_order_relaxed);
    stats.resyncs = resyncs.load (std::memory_order_relaxed);
    stats.jitterMs = jitterMs.load (std::memory_order_relaxed);
    stats.primed = primed.load (std::memory_order_relaxed);
    return stats;
}
}
