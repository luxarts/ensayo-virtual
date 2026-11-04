#include "MixServer.h"

#include <algorithm>
#include <thread>

namespace ensayo
{
namespace
{
    constexpr double fadeInSeconds = 0.2;
}

struct MixServer::Participant
{
    Participant (const OpusMode& mode, uint32_t participantId, const juce::String& participantName, Role participantRole)
        : id (participantId), name (participantName), role (participantRole),
          voiceJitter (codedBytesMono (mode.frameSize()), 1000.0 * frameSamples (mode.frameSize()) / sampleRate),
          instrumentJitter (codedBytesMono (mode.frameSize()), 1000.0 * frameSamples (mode.frameSize()) / sampleRate),
          voiceDecoder (mode, 1), instrumentDecoder (mode, 1), encoder (mode, 2),
          voice ((size_t) frameSamples (mode.frameSize()), 0.0f),
          instrument ((size_t) frameSamples (mode.frameSize()), 0.0f),
          popBuffer ((size_t) juce::jmax (codedBytesMono (mode.frameSize()), 64), 0),
          fadeStep ((float) frameSamples (mode.frameSize()) / (float) (fadeInSeconds * sampleRate))
    {
    }

    const uint32_t id;
    const juce::String name;
    const Role role;

    JitterBuffer voiceJitter, instrumentJitter;
    OpusDecoder voiceDecoder, instrumentDecoder;
    OpusEncoder encoder;                                 // this client's stereo mix

    std::vector<float> voice, instrument;                // decoded mono blocks for this tick
    std::vector<uint8_t> popBuffer;
    std::map<uint32_t, ChannelGain> gains;               // what this client hears of each target (0 = track)

    std::atomic<float> voicePeak { 0.0f }, instrumentPeak { 0.0f };
    float fade = 0.0f;
    const float fadeStep;
    uint16_t outSeq = 0;
};

// RAII for a control-thread mutation: waits until the real-time paths are out, keeps them out while alive.
struct MixServer::ControlMutation
{
    explicit ControlMutation (MixServer& s) : server (s)
    {
        server.writerActive.store (true, std::memory_order_release);

        while (server.readers.load (std::memory_order_acquire) != 0)
            std::this_thread::yield();
    }

    ~ControlMutation()
    {
        server.writerActive.store (false, std::memory_order_release);
    }

    MixServer& server;
};

bool MixServer::tryEnterRealtime() noexcept
{
    if (writerActive.load (std::memory_order_acquire))
        return false;

    readers.fetch_add (1, std::memory_order_acq_rel);

    if (writerActive.load (std::memory_order_acquire))
    {
        readers.fetch_sub (1, std::memory_order_acq_rel);
        return false;
    }

    return true;
}

void MixServer::leaveRealtime() noexcept
{
    readers.fetch_sub (1, std::memory_order_acq_rel);
}

MixServer::MixServer (FrameSize frameSizeToUse)
    : size (frameSizeToUse), frame (frameSamples (frameSizeToUse)), mode (frameSizeToUse),
      trackL ((size_t) frame, 0.0f), trackR ((size_t) frame, 0.0f),
      mixL ((size_t) frame, 0.0f), mixR ((size_t) frame, 0.0f), interleaved ((size_t) frame * 2, 0.0f),
      coded ((size_t) codedBytesStereo (frameSizeToUse) + 64, 0), packet (maxPacketBytes, 0)
{
}

MixServer::~MixServer() = default;

FrameSize MixServer::frameSize() const noexcept
{
    return size;
}

MixServer::Participant* MixServer::findUnlocked (uint32_t id) const noexcept
{
    for (const auto& p : participants)
        if (p->id == id)
            return p.get();

    return nullptr;
}

uint32_t MixServer::addParticipant (const juce::String& name, Role role)
{
    const std::scoped_lock control (controlMutex);
    auto participant = std::make_unique<Participant> (mode, nextId, name, role);
    const auto id = nextId++;

    const ControlMutation mutation (*this);
    participants.push_back (std::move (participant));
    return id;
}

void MixServer::removeParticipant (uint32_t id)
{
    std::unique_ptr<Participant> removed;
    const std::scoped_lock control (controlMutex);

    {
        const ControlMutation mutation (*this);
        const auto it = std::find_if (participants.begin(), participants.end(),
                                      [id] (const auto& p) { return p->id == id; });

        if (it == participants.end())
            return;

        removed = std::move (*it);
        participants.erase (it);
    }
    // `removed` is destroyed here, after the real-time paths may run again.
}

std::vector<ParticipantInfo> MixServer::getParticipants() const
{
    const std::scoped_lock control (controlMutex);
    std::vector<ParticipantInfo> result;
    result.reserve (participants.size());

    for (const auto& p : participants)
        result.push_back ({ p->id, p->name, p->role });

    return result;
}

bool MixServer::isEmpty() const
{
    const std::scoped_lock control (controlMutex);
    return participants.empty();
}

void MixServer::setGain (uint32_t client, uint32_t target, const ChannelGain& gain)
{
    const std::scoped_lock control (controlMutex);
    auto* participant = findUnlocked (client);

    if (participant == nullptr)
        return;

    if (target != 0 && findUnlocked (target) == nullptr)
        return;

    const ControlMutation mutation (*this);
    participant->gains[target] = gain;
}

BackingTrack& MixServer::track() noexcept
{
    return backingTrack;
}

void MixServer::setNextParticipantId (uint32_t id)
{
    const std::scoped_lock control (controlMutex);
    nextId = juce::jmax (nextId, id);
}

void MixServer::pushAudio (const AudioPacketView& view, double arrivalMs) noexcept
{
    if (view.frameSize != size || (view.flags & AudioFlags::mix) != 0)
        return;

    if (! tryEnterRealtime())
        return;

    if (auto* p = findUnlocked (view.clientId))
    {
        if ((view.flags & AudioFlags::voice) != 0 && view.voice != nullptr && view.voiceLen > 0)
            p->voiceJitter.push (view.seq, view.voice, view.voiceLen, arrivalMs);

        if ((view.flags & AudioFlags::instrument) != 0 && view.instrument != nullptr && view.instrumentLen > 0)
            p->instrumentJitter.push (view.seq, view.instrument, view.instrumentLen, arrivalMs);
    }

    leaveRealtime();
}

int MixServer::tick (const Sender& send) noexcept
{
    if (! tryEnterRealtime())
        return 0;

    // 1. Decode one block per participant (PLC on gaps), apply the fade-in and measure peaks.
    for (auto& ptr : participants)
    {
        auto& p = *ptr;

        if (roleSendsVoice (p.role))
        {
            int len = 0;
            const bool got = p.voiceJitter.pop (p.popBuffer.data(), len);
            p.voiceDecoder.decode (got ? p.popBuffer.data() : nullptr, got ? len : 0, p.voice.data());
        }
        else
        {
            std::fill (p.voice.begin(), p.voice.end(), 0.0f);
        }

        if (roleSendsInstrument (p.role))
        {
            int len = 0;
            const bool got = p.instrumentJitter.pop (p.popBuffer.data(), len);
            p.instrumentDecoder.decode (got ? p.popBuffer.data() : nullptr, got ? len : 0, p.instrument.data());
        }
        else
        {
            std::fill (p.instrument.begin(), p.instrument.end(), 0.0f);
        }

        if (p.fade < 1.0f)
        {
            // Linear ramp across the block so the fade-in has no steps.
            const float start = p.fade;
            const float end = juce::jmin (1.0f, p.fade + p.fadeStep);
            const float step = (end - start) / (float) frame;

            for (int i = 0; i < frame; ++i)
            {
                const float g = start + (float) i * step;
                p.voice[(size_t) i] *= g;
                p.instrument[(size_t) i] *= g;
            }

            p.fade = end;
        }

        p.voicePeak.store (peakOf (p.voice.data(), frame), std::memory_order_relaxed);
        p.instrumentPeak.store (peakOf (p.instrument.data(), frame), std::memory_order_relaxed);
    }

    // 2. Backing track block.
    std::fill (trackL.begin(), trackL.end(), 0.0f);
    std::fill (trackR.begin(), trackR.end(), 0.0f);
    backingTrack.render (trackL.data(), trackR.data(), frame);
    const auto trackPos = backingTrack.getPosition();
    trackPosition.store ((uint32_t) trackPos, std::memory_order_relaxed);

    // 3. One personalised stereo mix per client.
    int sent = 0;

    for (auto& ptr : participants)
    {
        auto& client = *ptr;
        std::fill (mixL.begin(), mixL.end(), 0.0f);
        std::fill (mixR.begin(), mixR.end(), 0.0f);

        for (const auto& other : participants)
        {
            if (other.get() == &client)
                continue;

            const auto it = client.gains.find (other->id);
            const ChannelGain gain = it != client.gains.end() ? it->second : ChannelGain {};

            if (gain.muted)
                continue;

            if (roleSendsVoice (other->role))
                addMonoCentred (mixL.data(), mixR.data(), other->voice.data(), gain.voiceGain(), frame);

            if (roleSendsInstrument (other->role))
                addMonoCentred (mixL.data(), mixR.data(), other->instrument.data(), gain.instrumentGain(), frame);
        }

        {
            const auto it = client.gains.find (0);
            const ChannelGain gain = it != client.gains.end() ? it->second : ChannelGain {};

            if (! gain.muted)
                addStereo (mixL.data(), mixR.data(), trackL.data(), trackR.data(), gain.voiceGain(), frame);
        }

        interleave (mixL.data(), mixR.data(), interleaved.data(), frame);
        const int codedLen = client.encoder.encode (interleaved.data(), coded.data(), (int) coded.size());

        if (codedLen <= 0)
            continue;

        AudioPacketView view;
        view.flags = AudioFlags::mix;
        view.frameSize = size;
        view.seq = client.outSeq++;
        view.clientId = 0;
        view.sampleTime = sampleTime;
        view.trackPosition = (uint32_t) trackPos;
        view.voice = coded.data();
        view.voiceLen = codedLen;

        const auto len = encodeAudioPacket (view, packet.data(), packet.size());

        if (len > 0 && send)
        {
            send (client.id, packet.data(), len);
            ++sent;
        }
    }

    sampleTime += (uint32_t) frame;
    leaveRealtime();
    return sent;
}

std::vector<ParticipantStats> MixServer::getStats() const
{
    const std::scoped_lock control (controlMutex);
    std::vector<ParticipantStats> result;
    result.reserve (participants.size());

    for (const auto& p : participants)
    {
        ParticipantStats stats;
        stats.id = p->id;
        const auto voice = p->voiceJitter.getStats();
        const auto instrument = p->instrumentJitter.getStats();
        const bool useVoice = roleSendsVoice (p->role);
        const bool useInstrument = roleSendsInstrument (p->role);
        stats.jitterBlocks = juce::jmax (useVoice ? voice.sizeBlocks : 0, useInstrument ? instrument.sizeBlocks : 0);
        stats.underruns = (useVoice ? voice.underruns : 0) + (useInstrument ? instrument.underruns : 0);
        stats.late = (useVoice ? voice.late : 0) + (useInstrument ? instrument.late : 0);
        stats.voicePeak = p->voicePeak.load (std::memory_order_relaxed);
        stats.instrumentPeak = p->instrumentPeak.load (std::memory_order_relaxed);
        result.push_back (stats);
    }

    return result;
}

uint32_t MixServer::getTrackPosition() const noexcept
{
    return trackPosition.load (std::memory_order_relaxed);
}
}
