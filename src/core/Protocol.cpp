#include "Protocol.h"

#include <cstring>

namespace ensayo
{
namespace
{
    constexpr int maxPayloadBytes = 255;
    const char* const sessionIdAlphabet = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
    constexpr int sessionIdLength = 6;

    inline void writeU16 (uint8_t* p, uint16_t v) noexcept
    {
        p[0] = (uint8_t) (v & 0xFF);
        p[1] = (uint8_t) ((v >> 8) & 0xFF);
    }

    inline void writeU32 (uint8_t* p, uint32_t v) noexcept
    {
        p[0] = (uint8_t) (v & 0xFF);
        p[1] = (uint8_t) ((v >> 8) & 0xFF);
        p[2] = (uint8_t) ((v >> 16) & 0xFF);
        p[3] = (uint8_t) ((v >> 24) & 0xFF);
    }

    inline uint16_t readU16 (const uint8_t* p) noexcept
    {
        return (uint16_t) (p[0] | (p[1] << 8));
    }

    inline uint32_t readU32 (const uint8_t* p) noexcept
    {
        return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
    }
}

std::optional<FrameSize> frameSizeForSamples (int samples) noexcept
{
    if (samples == 64)  return FrameSize::f64;
    if (samples == 128) return FrameSize::f128;
    return std::nullopt;
}

juce::String roleToString (Role role)
{
    switch (role)
    {
        case Role::voice:      return "voice";
        case Role::instrument: return "instrument";
        case Role::both:       break;
    }

    return "both";
}

std::optional<Role> roleFromString (const juce::String& text)
{
    if (text == "voice")      return Role::voice;
    if (text == "instrument") return Role::instrument;
    if (text == "both")       return Role::both;
    return std::nullopt;
}

bool roleSendsVoice (Role role) noexcept
{
    return role == Role::voice || role == Role::both;
}

bool roleSendsInstrument (Role role) noexcept
{
    return role == Role::instrument || role == Role::both;
}

size_t encodeAudioPacket (const AudioPacketView& packet, uint8_t* out, size_t capacity) noexcept
{
    if (out == nullptr)
        return 0;

    if (packet.voiceLen < 0 || packet.voiceLen > maxPayloadBytes
        || packet.instrumentLen < 0 || packet.instrumentLen > maxPayloadBytes)
        return 0;

    if (packet.voiceLen > 0 && packet.voice == nullptr)
        return 0;

    if (packet.instrumentLen > 0 && packet.instrument == nullptr)
        return 0;

    const size_t total = audioHeaderBytes + (size_t) packet.voiceLen + (size_t) packet.instrumentLen;

    if (total > capacity)
        return 0;

    out[0] = packetMagic;
    out[1] = protocolVersion;
    out[2] = packet.flags;
    out[3] = (uint8_t) packet.frameSize;
    writeU16 (out + 4, packet.seq);
    writeU32 (out + 6, packet.clientId);
    writeU32 (out + 10, packet.sampleTime);
    writeU32 (out + 14, packet.trackPosition);
    out[18] = (uint8_t) packet.voiceLen;
    out[19] = (uint8_t) packet.instrumentLen;

    uint8_t* payload = out + audioHeaderBytes;

    if (packet.voiceLen > 0)
        std::memcpy (payload, packet.voice, (size_t) packet.voiceLen);

    if (packet.instrumentLen > 0)
        std::memcpy (payload + packet.voiceLen, packet.instrument, (size_t) packet.instrumentLen);

    return total;
}

bool decodeAudioPacket (const uint8_t* data, size_t len, AudioPacketView& out) noexcept
{
    if (data == nullptr || len < audioHeaderBytes)
        return false;

    if (data[0] != packetMagic || data[1] != protocolVersion)
        return false;

    const uint8_t frameCode = data[3];

    if (frameCode > (uint8_t) FrameSize::f128)
        return false;

    const int voiceLen = data[18];
    const int instrumentLen = data[19];

    if (audioHeaderBytes + (size_t) voiceLen + (size_t) instrumentLen != len)
        return false;

    out.flags = data[2];
    out.frameSize = (FrameSize) frameCode;
    out.seq = readU16 (data + 4);
    out.clientId = readU32 (data + 6);
    out.sampleTime = readU32 (data + 10);
    out.trackPosition = readU32 (data + 14);

    const uint8_t* payload = data + audioHeaderBytes;
    out.voiceLen = voiceLen;
    out.voice = voiceLen > 0 ? payload : nullptr;
    out.instrumentLen = instrumentLen;
    out.instrument = instrumentLen > 0 ? payload + voiceLen : nullptr;

    return true;
}

void encodeKeepalive (uint32_t clientId, const UdpSecret& secret, uint8_t* out) noexcept
{
    out[0] = keepaliveMarker;
    writeU32 (out + 1, clientId);
    std::memcpy (out + 5, secret.data(), secret.size());
}

bool decodeKeepalive (const uint8_t* data, size_t len, uint32_t& clientId, UdpSecret& secret) noexcept
{
    if (data == nullptr || len != keepaliveBytes || data[0] != keepaliveMarker)
        return false;

    clientId = readU32 (data + 1);
    std::memcpy (secret.data(), data + 5, secret.size());
    return true;
}

juce::String udpSecretToHex (const UdpSecret& secret)
{
    return juce::String::toHexString (secret.data(), (int) secret.size(), 0).toLowerCase();
}

std::optional<UdpSecret> udpSecretFromHex (const juce::String& text)
{
    if (text.length() != (int) (2 * std::tuple_size_v<UdpSecret>) || ! text.containsOnly ("0123456789abcdefABCDEF"))
        return std::nullopt;

    UdpSecret secret {};

    for (size_t i = 0; i < secret.size(); ++i)
        secret[i] = (uint8_t) text.substring ((int) (2 * i), (int) (2 * i + 2)).getHexValue32();

    return secret;
}

juce::String makeLine (const juce::var& object)
{
    return juce::JSON::toString (object, true) + "\n";
}

std::optional<juce::var> parseLine (const juce::String& line)
{
    juce::var parsed;

    if (juce::JSON::parse (line.trim(), parsed).failed())
        return std::nullopt;

    auto* object = parsed.getDynamicObject();

    if (object == nullptr)
        return std::nullopt;

    const auto& type = object->getProperty ("t");

    if (! type.isString())
        return std::nullopt;

    return parsed;
}

juce::var makeMessage (const juce::String& type)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("t", type);
    return juce::var (object);
}

juce::String generateSessionId (juce::Random& random)
{
    const int alphabetSize = (int) std::strlen (sessionIdAlphabet);
    juce::String id;
    id.preallocateBytes (sessionIdLength);

    for (int i = 0; i < sessionIdLength; ++i)
        id << juce::String::charToString ((juce::juce_wchar) sessionIdAlphabet[random.nextInt (alphabetSize)]);

    return id;
}

bool isValidSessionId (const juce::String& id)
{
    if (id.length() != sessionIdLength)
        return false;

    const juce::String allowed (sessionIdAlphabet);

    for (auto c : id)
        if (! allowed.containsChar (c))
            return false;

    return true;
}

juce::var transportToVar (const TransportState& state)
{
    auto* object = new juce::DynamicObject();
    object->setProperty ("loaded", state.loaded);
    object->setProperty ("playing", state.playing);
    object->setProperty ("loop", state.loop);
    object->setProperty ("position", (juce::int64) state.position);
    object->setProperty ("lengthSamples", (juce::int64) state.length);
    object->setProperty ("name", state.name);
    return juce::var (object);
}

TransportState transportFromVar (const juce::var& v)
{
    TransportState state;
    auto* object = v.getDynamicObject();

    if (object == nullptr)
        return state;

    state.loaded = (bool) object->getProperty ("loaded");
    state.playing = (bool) object->getProperty ("playing");
    state.loop = (bool) object->getProperty ("loop");
    state.position = (int64_t) (juce::int64) object->getProperty ("position");
    state.length = (int64_t) (juce::int64) object->getProperty ("lengthSamples");
    state.name = object->getProperty ("name").toString();
    return state;
}
}
