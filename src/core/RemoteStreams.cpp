#include "core/RemoteStreams.h"

namespace ensayo
{
bool RemoteStreamTable::Slot::pop (uint32_t expectedGeneration, uint8_t* out, int& len) noexcept
{
    len = 0;

    if (generation.load (std::memory_order_acquire) != expectedGeneration
        || key.load (std::memory_order_acquire) == freeKey)
        return false;

    const bool popped = buffer.pop (out, len);

    // A block pushed for a newer claim is published after its generation bump, so seeing that block implies seeing
    // the new generation here.
    if (generation.load (std::memory_order_acquire) != expectedGeneration
        || key.load (std::memory_order_acquire) == freeKey)
    {
        len = 0;
        return false;
    }

    return popped;
}

int RemoteStreamTable::push (uint32_t clientId, StreamKind kind, uint16_t seq, const uint8_t* payload, int len,
                             double nowMs) noexcept
{
    if (clientId == 0 || clientId > 0x7fffffffu)
        return -1;

    const auto wanted = keyFor (clientId, kind);
    int index = -1, firstFree = -1;

    for (int i = 0; i < capacity; ++i)
    {
        const auto key = table[(size_t) i].key.load (std::memory_order_relaxed);   // only this thread writes it

        if (key == wanted)
        {
            index = i;
            break;
        }

        if (key == freeKey && firstFree < 0)
            firstFree = i;
    }

    if (index < 0)
    {
        if (firstFree < 0)
            return -1;

        index = firstFree;
        auto& slot = table[(size_t) index];
        slot.buffer.reset();
        slot.releaseKey.store (freeKey, std::memory_order_relaxed);
        slot.generation.fetch_add (1, std::memory_order_acq_rel);
        slot.key.store (wanted, std::memory_order_release);
    }

    auto& slot = table[(size_t) index];
    slot.lastPacketMs = nowMs;
    slot.buffer.push (seq, payload, len, nowMs);
    return index;
}

void RemoteStreamTable::sweep (double nowMs) noexcept
{
    for (auto& slot : table)
    {
        const auto key = slot.key.load (std::memory_order_relaxed);
        const auto requested = slot.releaseKey.exchange (freeKey, std::memory_order_acq_rel);

        if (key == freeKey)
            continue;

        if (requested == key || nowMs - slot.lastPacketMs >= idleReleaseMs)
            release (slot);
    }
}

void RemoteStreamTable::releaseClient (uint32_t clientId) noexcept
{
    for (auto& slot : table)
    {
        const auto key = slot.key.load (std::memory_order_acquire);

        if (key != freeKey && clientOf (key) == clientId)
            slot.releaseKey.store (key, std::memory_order_release);
    }
}

void RemoteStreamTable::clear() noexcept
{
    for (auto& slot : table)
    {
        slot.releaseKey.store (freeKey, std::memory_order_relaxed);
        release (slot);
    }
}

int RemoteStreamTable::activeCount() const noexcept
{
    int count = 0;

    for (const auto& slot : table)
        if (slot.key.load (std::memory_order_acquire) != freeKey)
            ++count;

    return count;
}

void RemoteStreamTable::release (Slot& slot) noexcept
{
    slot.key.store (freeKey, std::memory_order_release);
}
}
