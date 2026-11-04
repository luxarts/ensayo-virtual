#pragma once
#include "core/JitterBuffer.h"
#include <cstddef>
#include <cstdint>
namespace ensayo
{
// What the engine needs from the network side. SessionClient (Task 7) implements it.
class AudioTransport
{
public:
    virtual ~AudioTransport() = default;
    virtual void sendAudio (const uint8_t* packet, size_t len) noexcept = 0;   // audio thread → queue
    virtual JitterBuffer& receiveBuffer() noexcept = 0;                        // filled by the network thread
    virtual uint32_t getClientId() const noexcept = 0;
    virtual uint32_t getTrackPosition() const noexcept = 0;
};
}
