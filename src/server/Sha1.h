#pragma once
#include <juce_core/juce_core.h>
#include <array>
#include <cstdint>
#include <cstring>
namespace ensayo
{
// Plain SHA-1 (FIPS 180-4). JUCE ships SHA-256 only, and uploads are verified with SHA-1 per the protocol.
class Sha1
{
public:
    Sha1() noexcept { reset(); }

    void reset() noexcept
    {
        state = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };
        totalBytes = 0;
        bufferUsed = 0;
    }

    void update (const void* data, size_t len) noexcept
    {
        const auto* bytes = static_cast<const uint8_t*> (data);
        totalBytes += len;

        while (len > 0)
        {
            const size_t take = juce::jmin (len, (size_t) 64 - bufferUsed);
            std::memcpy (buffer.data() + bufferUsed, bytes, take);
            bufferUsed += take;
            bytes += take;
            len -= take;

            if (bufferUsed == 64)
            {
                processBlock (buffer.data());
                bufferUsed = 0;
            }
        }
    }

    // Finishes the digest and returns 40 lowercase hex characters. The object is reset afterwards.
    juce::String finishHex() noexcept
    {
        const uint64_t bits = totalBytes * 8;
        const uint8_t pad = 0x80;
        update (&pad, 1);
        const uint8_t zero = 0;

        while (bufferUsed != 56)
            update (&zero, 1);

        uint8_t lengthBytes[8];

        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (uint8_t) (bits >> (56 - 8 * i));

        update (lengthBytes, 8);

        juce::String hex;

        for (auto word : state)
            hex += juce::String::toHexString ((int) word).paddedLeft ('0', 8);

        reset();
        return hex.toLowerCase();
    }

    static juce::String of (const void* data, size_t len) noexcept
    {
        Sha1 sha;
        sha.update (data, len);
        return sha.finishHex();
    }

    static juce::String of (juce::InputStream& stream)
    {
        Sha1 sha;
        uint8_t chunk[16384];

        for (;;)
        {
            const int n = stream.read (chunk, (int) sizeof (chunk));

            if (n <= 0)
                break;

            sha.update (chunk, (size_t) n);
        }

        return sha.finishHex();
    }

private:
    static uint32_t rotl (uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

    void processBlock (const uint8_t* block) noexcept
    {
        uint32_t w[80];

        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t) block[i * 4] << 24) | ((uint32_t) block[i * 4 + 1] << 16)
                 | ((uint32_t) block[i * 4 + 2] << 8) | (uint32_t) block[i * 4 + 3];

        for (int i = 16; i < 80; ++i)
            w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];

        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;

            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

            const uint32_t temp = rotl (a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = rotl (b, 30);
            b = a;
            a = temp;
        }

        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e;
    }

    std::array<uint32_t, 5> state {};
    std::array<uint8_t, 64> buffer {};
    uint64_t totalBytes = 0;
    size_t bufferUsed = 0;
};
}
