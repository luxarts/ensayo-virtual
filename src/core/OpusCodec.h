#pragma once
#include "Protocol.h"
#include <memory>
struct OpusCustomMode; struct OpusCustomEncoder; struct OpusCustomDecoder;
namespace ensayo
{
class OpusMode            // one per FrameSize, shared by all encoders/decoders of that size; outlives them
{
public:
    explicit OpusMode (FrameSize);
    ~OpusMode();
    OpusCustomMode* get() const noexcept { return mode; }
    FrameSize frameSize() const noexcept { return size; }
private:
    OpusCustomMode* mode = nullptr; FrameSize size;

    OpusMode (const OpusMode&) = delete;
    OpusMode& operator= (const OpusMode&) = delete;
};

class OpusEncoder         // CBR; encode() always produces exactly codedBytes()
{
public:
    OpusEncoder (const OpusMode&, int channels);   // channels 1 or 2
    ~OpusEncoder();
    int codedBytes() const noexcept;               // codedBytesMono/Stereo (frameSize)
    // pcm interleaved, frameSamples (frameSize) frames. Returns codedBytes() or 0 on error. Real-time safe.
    int encode (const float* pcm, uint8_t* out, int capacity) noexcept;
private:
    OpusCustomEncoder* encoder = nullptr; int channels, bytes, samples;

    OpusEncoder (const OpusEncoder&) = delete;
    OpusEncoder& operator= (const OpusEncoder&) = delete;
};

class OpusDecoder
{
public:
    OpusDecoder (const OpusMode&, int channels);
    ~OpusDecoder();
    // data == nullptr → packet-loss concealment. Writes frameSamples*channels interleaved floats. Real-time safe.
    void decode (const uint8_t* data, int len, float* pcm) noexcept;
private:
    OpusCustomDecoder* decoder = nullptr; int channels, samples;

    OpusDecoder (const OpusDecoder&) = delete;
    OpusDecoder& operator= (const OpusDecoder&) = delete;
};
}
