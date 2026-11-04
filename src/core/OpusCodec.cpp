#include "OpusCodec.h"

#include <opus_custom.h>
#include <cstring>

namespace ensayo
{
OpusMode::OpusMode (FrameSize frameSizeToUse)
    : size (frameSizeToUse)
{
    int error = 0;
    mode = opus_custom_mode_create (sampleRate, frameSamples (size), &error);
    jassert (mode != nullptr && error == OPUS_OK);
}

OpusMode::~OpusMode()
{
    if (mode != nullptr)
        opus_custom_mode_destroy (mode);
}

OpusEncoder::OpusEncoder (const OpusMode& mode, int numChannels)
    : channels (numChannels == 2 ? 2 : 1),
      bytes (channels == 2 ? codedBytesStereo (mode.frameSize()) : codedBytesMono (mode.frameSize())),
      samples (frameSamples (mode.frameSize()))
{
    jassert (numChannels == 1 || numChannels == 2);

    if (mode.get() == nullptr)
        return;

    int error = 0;
    encoder = opus_custom_encoder_create (mode.get(), channels, &error);

    if (encoder == nullptr || error != OPUS_OK)
    {
        encoder = nullptr;
        return;
    }

    opus_custom_encoder_ctl (encoder, OPUS_SET_VBR (0));
    opus_custom_encoder_ctl (encoder, OPUS_SET_COMPLEXITY (2));
    opus_custom_encoder_ctl (encoder, OPUS_SET_PACKET_LOSS_PERC (20));
    opus_custom_encoder_ctl (encoder, OPUS_SET_BITRATE ((opus_int32) (bytes * 8 * sampleRate / samples)));
}

OpusEncoder::~OpusEncoder()
{
    if (encoder != nullptr)
        opus_custom_encoder_destroy (encoder);
}

int OpusEncoder::codedBytes() const noexcept
{
    return bytes;
}

int OpusEncoder::encode (const float* pcm, uint8_t* out, int capacity) noexcept
{
    if (encoder == nullptr || pcm == nullptr || out == nullptr || capacity < bytes)
        return 0;

    const int written = opus_custom_encode_float (encoder, pcm, samples, out, bytes);
    return written == bytes ? written : 0;
}

OpusDecoder::OpusDecoder (const OpusMode& mode, int numChannels)
    : channels (numChannels == 2 ? 2 : 1),
      samples (frameSamples (mode.frameSize()))
{
    jassert (numChannels == 1 || numChannels == 2);

    if (mode.get() == nullptr)
        return;

    int error = 0;
    decoder = opus_custom_decoder_create (mode.get(), channels, &error);

    if (decoder == nullptr || error != OPUS_OK)
        decoder = nullptr;
}

OpusDecoder::~OpusDecoder()
{
    if (decoder != nullptr)
        opus_custom_decoder_destroy (decoder);
}

void OpusDecoder::decode (const uint8_t* data, int len, float* pcm) noexcept
{
    if (pcm == nullptr)
        return;

    const int totalFloats = samples * channels;

    if (decoder == nullptr)
    {
        std::memset (pcm, 0, sizeof (float) * (size_t) totalFloats);
        return;
    }

    const bool conceal = data == nullptr || len <= 0;
    const int decoded = opus_custom_decode_float (decoder, conceal ? nullptr : data, conceal ? 0 : len, pcm, samples);

    if (decoded != samples)
        std::memset (pcm, 0, sizeof (float) * (size_t) totalFloats);
}
}
