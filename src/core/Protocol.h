#pragma once
#include <juce_core/juce_core.h>
#include <cstdint>
#include <optional>
#include <vector>

namespace ensayo
{
constexpr int sampleRate = 48000;
constexpr uint8_t packetMagic = 0xEA;
constexpr uint8_t protocolVersion = 1;
constexpr size_t maxPacketBytes = 512;
constexpr int defaultPort = 22140;
constexpr float minFaderDb = -60.0f, maxFaderDb = 12.0f;

enum class FrameSize : uint8_t { f64 = 0, f128 = 1 };
constexpr int frameSamples (FrameSize f) noexcept           { return f == FrameSize::f64 ? 64 : 128; }
constexpr int codedBytesMono (FrameSize f) noexcept         { return f == FrameSize::f64 ? 24 : 48; }
constexpr int codedBytesStereo (FrameSize f) noexcept       { return f == FrameSize::f64 ? 48 : 96; }
std::optional<FrameSize> frameSizeForSamples (int samples) noexcept;   // 64 / 128 only

enum class Role { voice, instrument, both };
juce::String roleToString (Role);                 // "voice" | "instrument" | "both"
std::optional<Role> roleFromString (const juce::String&);
bool roleSendsVoice (Role) noexcept;
bool roleSendsInstrument (Role) noexcept;

namespace AudioFlags
{
    constexpr uint8_t voice = 1, instrument = 2, mix = 4;
}

// Wire layout (little-endian): magic u8, version u8, flags u8, frameCode u8, seq u16,
// clientId u32, sampleTime u32, trackPosition u32, lenVoice u8, lenInstrument u8, payloads.
// (Payload lengths are at most 255 so one byte each keeps the header at exactly 20 bytes.)
constexpr size_t audioHeaderBytes = 20;

struct AudioPacketView
{
    uint8_t flags = 0;
    FrameSize frameSize = FrameSize::f128;
    uint16_t seq = 0;
    uint32_t clientId = 0;
    uint32_t sampleTime = 0;
    uint32_t trackPosition = 0;
    const uint8_t* voice = nullptr;      int voiceLen = 0;        // for mix packets: the stereo payload
    const uint8_t* instrument = nullptr; int instrumentLen = 0;
};

// Real-time safe. Returns bytes written, 0 if `out` is too small or lengths exceed 255.
size_t encodeAudioPacket (const AudioPacketView&, uint8_t* out, size_t capacity) noexcept;
// Real-time safe. Pointers in the result point into `data`. False on any malformed field.
bool decodeAudioPacket (const uint8_t* data, size_t len, AudioPacketView& out) noexcept;

// Control channel: one JSON object per line, field "t" = type.
juce::String makeLine (const juce::var& object);               // JSON + '\n', single line
std::optional<juce::var> parseLine (const juce::String& line); // nullopt unless an object with string "t"
juce::var makeMessage (const juce::String& type);              // {"t": type}
juce::String generateSessionId (juce::Random&);                // 6 chars of "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"
bool isValidSessionId (const juce::String&);

struct TransportState
{
    bool loaded = false, playing = false, loop = false;
    int64_t position = 0, length = 0;   // samples @ 48 k
    juce::String name;
};
juce::var transportToVar (const TransportState&);
TransportState transportFromVar (const juce::var&);
}
