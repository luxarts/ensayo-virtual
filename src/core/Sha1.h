#pragma once
// Minimal SHA-1 (RFC 3174) for the track upload checksum. JUCE ships MD5/SHA-256 only and the
// control protocol names the field "sha1", so both ends share this header.
#include <juce_core/juce_core.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

namespace ensayo
{
class Sha1
{
public:
    Sha1() = default;

    void update (const void* data, size_t len) noexcept
    {
        const auto* bytes = static_cast<const uint8_t*> (data);
        totalBits += (uint64_t) len * 8;

        while (len > 0)
        {
            const size_t take = std::min (len, (size_t) 64 - blockLen);
            std::memcpy (block.data() + blockLen, bytes, take);
            blockLen += take;
            bytes += take;
            len -= take;

            if (blockLen == 64)
            {
                processBlock();
                blockLen = 0;
            }
        }
    }

    std::array<uint8_t, 20> finish() noexcept
    {
        const uint64_t bits = totalBits;
        const uint8_t one = 0x80;
        update (&one, 1);
        totalBits = bits;   // padding does not count

        const uint8_t zero = 0;

        while (blockLen != 56)
        {
            update (&zero, 1);
            totalBits = bits;
        }

        uint8_t lengthBytes[8];

        for (int i = 0; i < 8; ++i)
            lengthBytes[i] = (uint8_t) (bits >> (56 - 8 * i));

        update (lengthBytes, 8);

        std::array<uint8_t, 20> digest {};

        for (int i = 0; i < 5; ++i)
        {
            digest[(size_t) i * 4 + 0] = (uint8_t) (h[i] >> 24);
            digest[(size_t) i * 4 + 1] = (uint8_t) (h[i] >> 16);
            digest[(size_t) i * 4 + 2] = (uint8_t) (h[i] >> 8);
            digest[(size_t) i * 4 + 3] = (uint8_t) (h[i]);
        }

        return digest;
    }

    // Lower-case hex of the whole buffer.
    static juce::String hexOf (const void* data, size_t len)
    {
        Sha1 sha;
        sha.update (data, len);
        return toHex (sha.finish());
    }

    static juce::String of (const void* data, size_t len)   { return hexOf (data, len); }

    // Lower-case hex of everything left in the stream.
    static juce::String of (juce::InputStream& in)
    {
        Sha1 sha;
        std::array<uint8_t, 65536> chunk {};

        for (;;)
        {
            const int n = in.read (chunk.data(), (int) chunk.size());

            if (n <= 0)
                break;

            sha.update (chunk.data(), (size_t) n);
        }

        return toHex (sha.finish());
    }

    // Lower-case hex of a file's contents; empty string when the file cannot be read.
    static juce::String hexOfFile (const juce::File& file)
    {
        juce::FileInputStream in (file);
        return in.openedOk() ? of (in) : juce::String();
    }

    static juce::String toHex (const std::array<uint8_t, 20>& digest)
    {
        return juce::String::toHexString (digest.data(), (int) digest.size(), 0).toLowerCase();
    }

private:
    static uint32_t rotl (uint32_t x, int n) noexcept { return (x << n) | (x >> (32 - n)); }

    void processBlock() noexcept
    {
        uint32_t w[80];

        for (int i = 0; i < 16; ++i)
            w[i] = ((uint32_t) block[(size_t) i * 4] << 24) | ((uint32_t) block[(size_t) i * 4 + 1] << 16)
                 | ((uint32_t) block[(size_t) i * 4 + 2] << 8) | (uint32_t) block[(size_t) i * 4 + 3];

        for (int i = 16; i < 80; ++i)
            w[i] = rotl (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];

        for (int i = 0; i < 80; ++i)
        {
            uint32_t f, k;

            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6; }

            const uint32_t temp = rotl (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rotl (b, 30); b = a; a = temp;
        }

        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    uint32_t h[5] { 0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0 };
    std::array<uint8_t, 64> block {};
    size_t blockLen = 0;
    uint64_t totalBits = 0;
};
}
