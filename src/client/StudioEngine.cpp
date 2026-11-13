#include "client/StudioEngine.h"
#include "core/MixMath.h"

#include <algorithm>
#include <cmath>
#include <bit>
#include <thread>

namespace ensayo
{
namespace
{
constexpr int maxChannels = 64;
constexpr int chatFrame = 128;                       // frameSamples (FrameSize::f128)
constexpr int chatQueueFrames = 64;                  // 170 ms of frames waiting for the chat thread

void storeMax (std::atomic<float>& target, float value) noexcept
{
    float current = target.load (std::memory_order_relaxed);

    while (value > current && ! target.compare_exchange_weak (current, value)) {}
}
}

//==============================================================================
void StudioDeviceManager::createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    if (auto* asio = juce::AudioIODeviceType::createAudioIODeviceType_ASIO())
        types.add (asio);
}

//==============================================================================
// Frames are aligned to the engine's chatClock (frame k = samples [128 k, 128 k + 128)), so the sequence number and
// sampleTime of a frame follow time even across the gaps where nothing is sent.
struct StudioEngine::ChatSender
{
    ChatSender (const OpusMode& mode, int numChannels, StreamKind streamKind)
        : kind (streamKind), channels (numChannels), frames ((size_t) chatQueueFrames), encoder (mode, numChannels)
    {
        for (int k = 0; k <= chatFrame; ++k)
            gateTable[(size_t) k] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * k / chatFrame));
    }

    struct Frame { uint32_t index = 0; std::array<float, chatFrame * 2> pcm {}; };   // interleaved

    // Audio thread: n samples starting at chatClock `clock`; b is the right channel (stereo only). While `wanted` the
    // gate opens over one frame, otherwise it closes over one frame; a frame is queued when its gate was open at any
    // of its samples, so the last one sent ends faded out. Returns true when a frame was queued.
    bool process (const float* a, const float* b, int n, int64_t clock, bool wanted) noexcept
    {
        bool queued = false;

        for (int i = 0; i < n; ++i)
        {
            const auto t = clock + i;
            const int pos = (int) (t % chatFrame);
            gate = juce::jlimit (0, chatFrame, gate + (wanted ? 1 : -1));
            const float g = gateTable[(size_t) gate];
            open = open || gate > 0;

            if (channels == 1)
            {
                building.pcm[(size_t) pos] = g * a[i];
            }
            else
            {
                building.pcm[(size_t) (2 * pos)] = g * a[i];
                building.pcm[(size_t) (2 * pos + 1)] = g * b[i];
            }

            if (pos != chatFrame - 1)
                continue;

            if (open)
            {
                building.index = (uint32_t) (t / chatFrame);
                int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
                fifo.prepareToWrite (1, start1, size1, start2, size2);

                if (size1 == 1)                      // else the chat thread is behind: the frame is dropped
                {
                    frames[(size_t) start1] = building;
                    fifo.finishedWrite (1);
                    queued = true;
                }
            }

            open = gate > 0;
        }

        return queued;
    }

    // Chat thread: encodes the next queued frame into `packet`. Returns the datagram's size, 0 when nothing is queued.
    size_t encodeNext() noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (1, start1, size1, start2, size2);

        if (size1 != 1)
            return 0;

        const auto& frame = frames[(size_t) start1];
        const int bytes = encoder.encode (frame.pcm.data(), coded.data(), (int) coded.size());
        AudioPacketView view;
        view.frameSize = FrameSize::f128;
        view.seq = (uint16_t) frame.index;
        view.clientId = 0;                           // the ProjectClient stamps its own id
        view.sampleTime = frame.index * (uint32_t) chatFrame;
        fifo.finishedRead (1);

        if (bytes <= 0)
            return encodeNext();

        if (channels == 1)
        {
            view.flags = AudioFlags::voice;
            view.voice = coded.data();
            view.voiceLen = bytes;
        }
        else
        {
            view.flags = AudioFlags::instrument;
            view.instrument = coded.data();
            view.instrumentLen = bytes;
        }

        return encodeAudioPacket (view, packet.data(), packet.size());
    }

    const StreamKind kind;
    const int channels;

    // Audio thread.
    std::array<float, chatFrame + 1> gateTable {};   // half-cosine 0 → 1
    int gate = 0;                                    // 0..chatFrame
    bool open = false;                               // the frame being built had its gate open at some sample
    Frame building;

    // Audio thread → chat thread (single producer, single consumer).
    juce::AbstractFifo fifo { chatQueueFrames };
    std::vector<Frame> frames;

    // Chat thread.
    OpusEncoder encoder;
    std::array<uint8_t, 255> coded {};               // payloads are at most 255 bytes
    std::array<uint8_t, maxPacketBytes> packet {};
};

struct StudioEngine::RemoteSlotMix
{
    explicit RemoteSlotMix (const OpusMode& mode) : mono (mode, 1), stereo (mode, 2) {}

    OpusDecoder mono, stereo;
    uint64_t state = 0;                              // the stream instance being decoded (its slot state), 0 = none
    int channels = 0;                                // of that stream
    bool primed = false;                             // a real frame was decoded: concealment may follow
    int lost = 0;                                    // consecutive concealed frames
    std::vector<float> L, R;                         // decoded and not played yet: [0, fill)
    int fill = 0;
    float gain = 1.0f;                               // participant gain reached at the end of the last block
    int tailCount = 0;                               // [0, tailCount) of L/R is a previous sender's faded tail...
    float tailGain = 1.0f;                           // ...played at that sender's gain, not ramped to the new one
    int counter = -1;                                // the stream's chatCountFrames entry, -1 = not counted
};

//==============================================================================
StudioEngine::StudioEngine()
{
    for (int k = 0; k <= transportFadeSamples; ++k)
        fadeTable[(size_t) k] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * k / transportFadeSamples));

    chatVoice = std::make_unique<ChatSender> (mode128, 1, StreamKind::voice);
    chatInstrument = std::make_unique<ChatSender> (mode128, 2, StreamKind::instrument);

    for (auto& m : remoteMix)
        m = std::make_unique<RemoteSlotMix> (mode128);

    chatThread = std::thread ([this] { chatLoop(); });
    deviceManager.addAudioCallback (this);
}

StudioEngine::~StudioEngine()
{
    if (recording.load())
    {
        disableCapture();
        recorder.stop();
        restoreRecorderCallbacks();
    }

    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();

    chatEnabled.store (false);
    chatQuit.store (true);
    chatSignal.fetch_add (1);
    chatSignal.notify_one();
    chatThread.join();
}

juce::AudioDeviceManager& StudioEngine::getDeviceManager() noexcept   { return deviceManager; }
PluginSlot& StudioEngine::getPluginSlot() noexcept                    { return plugin; }
Recorder& StudioEngine::getRecorder() noexcept                        { return recorder; }

FakeAudioDevice* StudioEngine::getFakeDevice() const noexcept
{
    return dynamic_cast<FakeAudioDevice*> (deviceManager.getCurrentAudioDevice());
}

juce::StringArray StudioEngine::getAsioDeviceNames()
{
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == "ASIO")
        {
            type->scanForDevices();
            return type->getDeviceNames (false);
        }
    }

    return {};
}

void StudioEngine::useFakeDevice (int blockSize, TestSignal signal, FakeLatency latency)
{
    deviceManager.closeAudioDevice();

    // Make sure the built-in (ASIO) types exist first: the manager only creates them while its list is empty.
    for (auto* type : deviceManager.getAvailableDeviceTypes())
    {
        if (type->getTypeName() == FakeAudioDeviceType::typeName())
        {
            deviceManager.removeAudioDeviceType (type);
            break;
        }
    }

    deviceManager.addAudioDeviceType (std::make_unique<FakeAudioDeviceType> (blockSize, signal, latency));

    const auto error = openDevice (FakeAudioDeviceType::typeName(), FakeAudioDeviceType::deviceName(), (double) sampleRate, blockSize);

    if (error.isNotEmpty())
        audioDeviceError (error);
}

juce::String StudioEngine::openDevice (const juce::String& deviceType, const juce::String& name, double rate, int bufferSize)
{
    bool typeFound = false;

    for (auto* type : deviceManager.getAvailableDeviceTypes())
        typeFound = typeFound || type->getTypeName() == deviceType;

    if (! typeFound)
        return "Tipo de dispositivo no disponible: " + deviceType;

    deviceManager.setCurrentAudioDeviceType (deviceType, true);

    juce::AudioDeviceManager::AudioDeviceSetup setup;
    setup.inputDeviceName = name;
    setup.outputDeviceName = name;
    setup.sampleRate = rate;
    setup.bufferSize = bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange (0, maxChannels, true);
    setup.outputChannels.setRange (0, maxChannels, true);

    return deviceManager.setAudioDeviceSetup (setup, true);
}

juce::String StudioEngine::takeDeviceError()
{
    const juce::ScopedLock sl (errorLock);
    return std::exchange (lastError, {});
}

//==============================================================================
void StudioEngine::setChannels (int mic, int instrument, int pair) noexcept
{
    micChannel.store (mic);
    instrumentChannel.store (instrument);
    outputPair.store (pair);
}

void StudioEngine::setMonitor (float voiceGain, float instrumentGain) noexcept
{
    monitorVoiceGain.store (voiceGain);
    monitorInstrumentGain.store (instrumentGain);
}

void StudioEngine::setSendGains (float voice, float instrument) noexcept
{
    const auto valid = [] (float g) { return std::isfinite (g) ? std::max (0.0f, g) : 0.0f; };
    sendVoiceGain.store (valid (voice));
    sendInstrumentGain.store (valid (instrument));
}

void StudioEngine::setArmed (std::optional<TrackKind> kind) noexcept
{
    armedKind.store (kind.has_value() && *kind != TrackKind::imported ? (int) *kind : -1);
}

void StudioEngine::setCalibration (int samples) noexcept     { calibrationSamples.store (samples); }

void StudioEngine::setMetronome (bool click, bool countIn) noexcept
{
    clickOn.store (click);
    countInOn.store (countIn);
}

void StudioEngine::setSampleSource (const SampleSource* s) noexcept
{
    if (source.load() == s)
        return;

    // Fade the master to silence first (the callback reports it; immediately when the transport is stopped). If no
    // callback answers (device stopped), swap anyway: the hand-off below is what makes the swap safe.
    holdReached.store (false);                       // only a block that saw this hold may report silence
    holdRequested.store (true);
    const auto deadline = juce::Time::getMillisecondCounter() + 250;

    while (! holdReached.load() && juce::Time::getMillisecondCounter() < deadline)
        std::this_thread::yield();

    source.store (s);

    while (inRender.load())                          // a block that loaded the previous source is still running
        std::this_thread::yield();

    holdRequested.store (false);
}

void StudioEngine::setProject (std::shared_ptr<const Project> p)
{
    if (p != nullptr)
    {
        bpm.store (p->bpm);
        beatsPerBar.store (p->beatsPerBar);
    }

    renderer.setProject (std::move (p));
}

void StudioEngine::setSolo (std::set<Id> solo)    { renderer.setSolo (std::move (solo)); }

//==============================================================================
void StudioEngine::requestPlay (bool shouldPlay) noexcept
{
    const juce::SpinLock::ScopedLockType sl (requestLock);
    requests.play = shouldPlay;
    wantPlaying.store (shouldPlay);
}

void StudioEngine::play()
{
    if (! recording.load())                          // a take ends only with stopRecording()
        requestPlay (true);
}

void StudioEngine::stop()
{
    if (! recording.load())
        requestPlay (false);
}

void StudioEngine::setPosition (int64_t target)
{
    if (recording.load())
        return;                                      // the record point is fixed while a take is running

    const juce::SpinLock::ScopedLockType sl (requestLock);
    ++requests.seekSerial;
    requests.seekTarget = target;
}

int64_t StudioEngine::getPosition() const noexcept
{
    {
        const juce::SpinLock::ScopedLockType sl (requestLock);

        if (requests.seekSerial != appliedSeekSerial.load())
            return requests.seekTarget;              // not picked up by the callback yet
    }

    return publishedPosition.load();
}

bool StudioEngine::isPlaying() const noexcept    { return wantPlaying.load() || audioPlaying.load(); }

//==============================================================================
juce::Result StudioEngine::startRecording (const Track& track, const Id& take, const juce::File& localWav)
{
    if (recording.load())
        return juce::Result::fail ("Already recording");

    if (isCalibrating())
        return juce::Result::fail ("Cannot record while calibrating");

    if (track.kind == TrackKind::imported)
        return juce::Result::fail ("Imported tracks cannot be recorded");

    auto* device = deviceManager.getCurrentAudioDevice();

    if (device == nullptr || ! device->isPlaying())
        return juce::Result::fail ("No audio device is running");

    const int channels = track.kind == TrackKind::voice ? 1 : 2;
    const double tempo = bpm.load();
    const int64_t countIn = countInOn.load() && tempo > 0.0
                              ? (int64_t) std::llround (beatsPerBar.load() * 60.0 * sampleRate / tempo) : 0;
    const int64_t point = getPosition();

    // The record point and the offset are fixed before the writer starts, so its callbacks (and the caller, through
    // getRecordPoint / getRecordOffset) can rely on them for the whole take.
    recordPoint = point;
    recordOffset = recordingOffset (countIn, device->getInputLatencyInSamples(), device->getOutputLatencyInSamples(),
                                    calibrationSamples.load());
    finishedSegments = 0;
    finishedSamples = 0;
    userFinished = recorder.onFinished;

    recorder.onFinished = [this] (int segments, int64_t samples)
    {
        finishedSegments = segments;
        finishedSamples = samples;

        if (userFinished)
            userFinished (segments, samples);
    };

    const auto started = recorder.start (take, channels, localWav);

    if (started.failed())
    {
        restoreRecorderCallbacks();
        return started;
    }

    recordTake = take;
    recordChannels.store (channels);
    captureEnabled.store (true);
    recording.store (true);

    const juce::SpinLock::ScopedLockType sl (requestLock);
    ++requests.recordSerial;
    requests.recordStart = point - countIn;
    requests.recordPoint = point;
    requests.countIn = countIn > 0;
    requests.play = true;
    wantPlaying.store (true);
    return juce::Result::ok();
}

std::optional<RecordingResult> StudioEngine::stopRecording()
{
    if (! recording.load())
        return std::nullopt;

    disableCapture();                                // the callback no longer pushes: Recorder::stop may run
    requestPlay (false);
    recorder.stop();                                 // drains the FIFO; onFinished has fired when it returns
    restoreRecorderCallbacks();
    recording.store (false);

    const int64_t total = finishedSamples;

    if (total - recordOffset < 1)
    {
        if (recorder.onError)
            recorder.onError (juce::String (juce::CharPointer_UTF8 ("La toma es m\xc3\xa1s corta que la cuenta y la latencia; no se guard\xc3\xb3")));

        return std::nullopt;
    }

    RecordingResult result;
    result.take = recordTake;
    result.channels = recordChannels.load();
    result.segments = finishedSegments;
    result.samples = total;
    result.region = Region { newId(), recordTake, recordPoint, recordOffset, total - recordOffset };
    return result;
}

bool StudioEngine::isRecording() const noexcept    { return recording.load(); }
int64_t StudioEngine::getRecordPoint() const noexcept    { return recordPoint; }
int64_t StudioEngine::getRecordOffset() const noexcept   { return recordOffset; }

void StudioEngine::disableCapture() noexcept
{
    captureEnabled.store (false);

    while (inPush.load())
        std::this_thread::yield();
}

void StudioEngine::restoreRecorderCallbacks()
{
    recorder.onFinished = std::exchange (userFinished, nullptr);
}

//==============================================================================
void StudioEngine::setChatEnabled (bool enabled)
{
    chatEnabled.store (enabled);

    if (! enabled)
        while (inChatSink.load())                    // the chat thread may be inside onChatPacket right now
            std::this_thread::yield();
}

void StudioEngine::setMicMuted (bool muted) noexcept    { micMuted.store (muted); }

void StudioEngine::sendChat (const float* voice, const float* instrumentL, const float* instrumentR, int n) noexcept
{
    const bool enabled = chatEnabled.load (std::memory_order_relaxed);
    const bool wantVoice = enabled && ! micMuted.load (std::memory_order_relaxed);
    const bool wantInstrument = enabled && captureEnabled.load (std::memory_order_relaxed)
                             && recordChannels.load (std::memory_order_relaxed) == 2;

    bool queued = chatVoice->process (voice, nullptr, n, chatClock, wantVoice);
    queued = chatInstrument->process (instrumentL, instrumentR, n, chatClock, wantInstrument) || queued;
    chatClock += n;

    if (queued)
    {
        chatSignal.fetch_add (1, std::memory_order_release);
        chatSignal.notify_one();                     // WakeByAddress: never blocks
    }
}

void StudioEngine::chatLoop()
{
    for (;;)
    {
        // The signal is read before the quit flag: a quit stored (and signalled) after this load changes the value, so
        // the wait below returns at once instead of missing it.
        const auto seen = chatSignal.load (std::memory_order_acquire);

        if (chatQuit.load())
            break;

        for (auto* sender : { chatVoice.get(), chatInstrument.get() })
        {
            while (const auto len = sender->encodeNext())
            {
                inChatSink.store (true);

                if (chatEnabled.load() && onChatPacket)
                    onChatPacket (sender->kind, sender->packet.data(), len);

                inChatSink.store (false);
            }
        }

        chatSignal.wait (seen, std::memory_order_acquire);
    }
}

//==============================================================================
void StudioEngine::setRemoteStreams (RemoteStreamTable* table) noexcept
{
    remoteTable.store (table);

    while (inRemote.load())                          // the callback may still be popping from the previous table
        std::this_thread::yield();
}

void StudioEngine::setParticipantGain (uint32_t clientId, float gain, bool muted) noexcept
{
    if (clientId == 0)
        return;

    const float g = muted || ! std::isfinite (gain) ? 0.0f : std::max (0.0f, gain);
    const auto value = ((uint64_t) clientId << 32) | std::bit_cast<uint32_t> (g);
    const auto unity = std::bit_cast<uint32_t> (1.0f);
    int freeIndex = -1, unityIndex = -1;

    for (int i = 0; i < participantCapacity; ++i)
    {
        const auto current = participantGains[(size_t) i].load();
        const auto id = (uint32_t) (current >> 32);

        if (id == clientId)
        {
            participantGains[(size_t) i].store (value);
            return;
        }

        if (current == 0 && freeIndex < 0)
            freeIndex = i;

        if (id != 0 && (uint32_t) current == unity && unityIndex < 0)
            unityIndex = i;              // an entry at the default level can be given away
    }

    int index = freeIndex >= 0 ? freeIndex : unityIndex;

    // Full: give away the entry of a sender that has no stream in the table now (there are more entries than slots),
    // so a muted participant who is sounding is never unmuted by someone else's level.
    if (index < 0)
    {
        std::array<uint32_t, RemoteStreamTable::capacity> live {};

        if (auto* table = remoteTable.load())
            for (size_t s = 0; s < live.size(); ++s)
                if (const auto key = table->slots()[s].key(); key != RemoteStreamTable::freeKey)
                    live[s] = RemoteStreamTable::clientOf (key);

        for (int i = 0; i < participantCapacity && index < 0; ++i)
        {
            const auto id = (uint32_t) (participantGains[(size_t) i].load() >> 32);

            if (std::find (live.begin(), live.end(), id) == live.end())
                index = i;
        }

        if (index < 0)
            index = (int) (clientId % participantCapacity);   // unreachable: 64 entries, at most 16 live senders
    }

    participantGains[(size_t) index].store (value);
}

void StudioEngine::removeParticipantGain (uint32_t clientId) noexcept
{
    if (clientId == 0)
        return;

    for (auto& entry : participantGains)
        if ((uint32_t) (entry.load() >> 32) == clientId)
            entry.store (0);
}

void StudioEngine::clearParticipantGains() noexcept
{
    for (auto& entry : participantGains)
        entry.store (0);
}

float StudioEngine::participantGain (uint32_t clientId) const noexcept
{
    for (const auto& entry : participantGains)
    {
        const auto value = entry.load (std::memory_order_relaxed);

        if ((uint32_t) (value >> 32) == clientId)
            return std::bit_cast<float> ((uint32_t) value);
    }

    return 1.0f;
}

int StudioEngine::chatCountIndex (uint32_t key) noexcept
{
    for (int i = 0; i < chatCountCapacity; ++i)
    {
        auto& entry = chatCountKeys[(size_t) i];
        const auto k = entry.load (std::memory_order_relaxed);   // only this thread writes the keys

        if (k == key)
            return i;

        if (k == RemoteStreamTable::freeKey)
        {
            entry.store (key, std::memory_order_release);
            return i;
        }
    }

    return -1;
}

std::vector<StudioEngine::ChatReceived> StudioEngine::chatReceivedFrames() const
{
    std::vector<ChatReceived> result;

    for (int i = 0; i < chatCountCapacity; ++i)
    {
        const auto key = chatCountKeys[(size_t) i].load (std::memory_order_acquire);

        if (key == RemoteStreamTable::freeKey)   // claimed in order: nothing after a free entry
            break;

        result.push_back ({ RemoteStreamTable::clientOf (key), RemoteStreamTable::kindOf (key),
                            chatCountFrames[(size_t) i].load (std::memory_order_relaxed) });
    }

    return result;
}

void StudioEngine::mixRemote (float* L, float* R, int n) noexcept
{
    inRemote.store (true);
    auto* table = remoteTable.load();

    // Half-cosine over one frame: the first frame of a stream fades in, the last one (end of concealment, a stream
    // that stops or changes) fades out.
    const auto ramp = [] (float* dl, float* dr, bool in) noexcept
    {
        for (int i = 0; i < chatFrame; ++i)
        {
            const auto c = (float) std::cos (juce::MathConstants<double>::pi * (i + 1) / chatFrame);
            const auto g = in ? 0.5f - 0.5f * c : 0.5f + 0.5f * c;
            dl[i] *= g;
            dr[i] *= g;
        }
    };

    // One frame of the slot's stream (data == nullptr: concealment) into dl / dr.
    const auto decodeInto = [this] (RemoteSlotMix& m, const uint8_t* data, int len, float* dl, float* dr) noexcept
    {
        if (m.channels == 1)
        {
            m.mono.decode (data, len, dl);
            std::copy (dl, dl + chatFrame, dr);
        }
        else
        {
            m.stereo.decode (data, len, remoteInterleaved.data());
            deinterleave (remoteInterleaved.data(), dl, dr, chatFrame);
        }
    };

    const auto fits = [] (const RemoteSlotMix& m) noexcept { return m.fill + chatFrame <= (int) m.L.size(); };

    // The stream stops (released, replaced, table gone): a sounding one gets one more concealed frame, faded out,
    // after what is already decoded; the slot's own state is never touched again.
    const auto stopStream = [&] (RemoteSlotMix& m) noexcept
    {
        if (m.state != 0 && m.primed && fits (m))
        {
            float* dl = m.L.data() + m.fill;
            float* dr = m.R.data() + m.fill;
            decodeInto (m, nullptr, 0, dl, dr);
            ramp (dl, dr, false);
            m.fill += chatFrame;
        }

        m.state = 0;
        m.channels = 0;
        m.primed = false;
        m.lost = 0;
    };

    if (table != lastRemoteTable)
    {
        for (auto& m : remoteMix)
            stopStream (*m);

        lastRemoteTable = table;
    }

    for (int index = 0; index < RemoteStreamTable::capacity; ++index)
    {
        auto& m = *remoteMix[(size_t) index];
        float target = m.gain;

        if (table != nullptr)
        {
            auto& slot = table->slots()[(size_t) index];
            const auto state = slot.state.load (std::memory_order_acquire);
            const auto key = RemoteStreamTable::Slot::keyOf (state);

            if (key == RemoteStreamTable::freeKey)
            {
                if (m.state != 0)
                    stopStream (m);
            }
            else
            {
                target = participantGain (RemoteStreamTable::clientOf (key));

                if (state != m.state)
                {
                    stopStream (m);
                    m.state = state;
                    m.channels = RemoteStreamTable::kindOf (key) == StreamKind::voice ? 1 : 2;
                    m.mono.reset();
                    m.stereo.reset();
                    m.counter = chatCountIndex (key);

                    // What is still decoded belongs to the previous sender: it keeps its gain while the new stream
                    // (faded in) starts at its own.
                    m.tailCount = m.fill;
                    m.tailGain = m.gain;
                    m.gain = target;
                }

                const int expected = m.channels == 1 ? codedBytesMono (FrameSize::f128) : codedBytesStereo (FrameSize::f128);

                // Whole frames until the block can be served; the remainder (< 128) waits for the next block.
                while (m.fill < n && fits (m))
                {
                    float* dl = m.L.data() + m.fill;
                    float* dr = m.R.data() + m.fill;
                    int len = 0;

                    if (slot.pop (state, remotePacket.data(), len) && len == expected)
                    {
                        decodeInto (m, remotePacket.data(), len, dl, dr);

                        if (m.counter >= 0)
                            chatCountFrames[(size_t) m.counter].fetch_add (1, std::memory_order_relaxed);

                        if (! m.primed)
                            ramp (dl, dr, true);

                        m.primed = true;
                        m.lost = 0;
                    }
                    else if (m.primed && m.lost < maxConcealedFrames)
                    {
                        decodeInto (m, nullptr, 0, dl, dr);

                        if (++m.lost == maxConcealedFrames)   // a dead stream: the last concealed frame fades out
                        {
                            ramp (dl, dr, false);
                            m.primed = false;
                        }
                    }
                    else
                    {
                        std::fill (dl, dl + chatFrame, 0.0f);
                        std::fill (dr, dr + chatFrame, 0.0f);
                    }

                    m.fill += chatFrame;
                }
            }
        }

        // What is decoded plays (a stopped stream's faded tail too), with the participant gain ramped across the block:
        // a full-scale change takes rampSamples.
        const int count = std::min (n, m.fill);

        if (count <= 0)
        {
            m.gain = target;
            continue;
        }

        const float g0 = m.gain;
        const float limit = (float) n / (float) rampSamples * std::max ({ 1.0f, g0, target });
        const float g1 = g0 + juce::jlimit (-limit, limit, target - g0);

        for (int i = 0; i < count; ++i)
        {
            const float g = i < m.tailCount ? m.tailGain : g0 + (g1 - g0) * (float) (i + 1) / (float) n;
            L[i] += g * m.L[(size_t) i];
            R[i] += g * m.R[(size_t) i];
        }

        m.gain = g1;
        m.tailCount = std::max (0, m.tailCount - count);
        m.fill -= count;
        std::copy (m.L.begin() + count, m.L.begin() + count + m.fill, m.L.begin());
        std::copy (m.R.begin() + count, m.R.begin() + count + m.fill, m.R.begin());
    }

    inRemote.store (false);
}

//==============================================================================
void StudioEngine::startCalibration()
{
    if (recording.load() || isCalibrating())
        return;

    requestPlay (false);
    calibrationRequested.store (true);
}

bool StudioEngine::isCalibrating() const noexcept
{
    return calibrationRequested.load() || calibrator.isRunning();
}

std::optional<int> StudioEngine::calibrationRoundTrip() const
{
    if (isCalibrating())
        return std::nullopt;

    return calibrator.roundTrip();
}

//==============================================================================
EngineMeters StudioEngine::takeMeters() noexcept
{
    EngineMeters m;
    m.voice = peakVoice.exchange (0.0f);
    m.instrument = peakInstrument.exchange (0.0f);
    m.output = peakOutput.exchange (0.0f);
    return m;
}

std::vector<std::pair<Id, float>> StudioEngine::takeTrackMeters()    { return renderer.takeTrackPeaks(); }
float StudioEngine::takeArmedInputPeak() noexcept                    { return peakArmed.exchange (0.0f); }

uint32_t StudioEngine::getCallbackCount() const noexcept    { return callbackCount.load (std::memory_order_relaxed); }

//==============================================================================
void StudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const int blockSize = juce::jmax (1, device->getCurrentBufferSizeSamples());
    const double rate = device->getCurrentSampleRate();
    const bool supported = (int) std::lround (rate) == sampleRate;

    if (! supported)
    {
        audioDeviceError ("El dispositivo debe correr a 48000 Hz (actual: " + juce::String (rate, 0) + " Hz)");
    }
    else if (recording.load())
    {
        // The device came back in the middle of a take (driver reset, buffer change): samples were lost and the
        // timeline is no longer aligned. The owner stops the take on this error and keeps what was recorded; nothing
        // more is captured (no callback runs here either).
        captureEnabled.store (false);
        audioDeviceError (restartedDuringTake());
    }
    else
    {
        const juce::ScopedLock sl (errorLock);
        lastError.clear();
    }

    maxBlock = blockSize;
    instrumentBuffer.setSize (2, blockSize);
    instrumentBuffer.clear();

    for (auto* v : { &silence, &mixL, &mixR, &scratchL, &scratchR, &voiceSent, &instrumentSentL, &instrumentSentR })
        v->assign ((size_t) blockSize, 0.0f);

    // No callback runs here: the gains start where they are asked to be, the monitor fades in from silence.
    sendVoiceRamp = GainRamp { sendVoiceGain.load(), sendVoiceGain.load() };
    sendInstrumentRamp = GainRamp { sendInstrumentGain.load(), sendInstrumentGain.load() };
    monitorVoiceRamp = GainRamp { monitorVoiceGain.load(), monitorVoiceGain.load() };
    monitorInstrumentRamp = GainRamp { monitorInstrumentGain.load(), monitorInstrumentGain.load() };
    monitorVoiceFade = monitorInstrumentFade = 0;

    // Received streams: a block of decoded audio plus a frame being played out, a concealed fade-out frame and one more.
    for (auto& m : remoteMix)
    {
        m->L.assign ((size_t) (blockSize + 3 * chatFrame), 0.0f);
        m->R.assign ((size_t) (blockSize + 3 * chatFrame), 0.0f);
        m->fill = 0;
        m->state = 0;
        m->channels = 0;
        m->primed = false;
        m->lost = 0;
        m->tailCount = 0;
    }

    // Chat send: the frame being built when the device stopped is dropped (the clock moves on to the next frame) and
    // both streams fade in again from a closed gate.
    chatClock = (chatClock + chatFrame - 1) / chatFrame * chatFrame;

    for (auto* sender : { chatVoice.get(), chatInstrument.get() })
    {
        sender->gate = 0;
        sender->open = false;
    }

    remoteInterleaved.assign ((size_t) chatFrame * 2, 0.0f);
    remotePacket.assign (maxPacketBytes, 0);
    lastRemoteTable = nullptr;

    renderer.prepare (blockSize);
    plugin.prepare (rate, blockSize);
    blockSupported.store (supported);
}

void StudioEngine::audioDeviceStopped()
{
    blockSupported.store (false);
    plugin.release();

    if (recording.load())
    {
        // No callback runs now, so no push is in flight: the take ends at the last aligned sample instead of going on
        // after a restart, shifted by whatever the new latencies are, until the owner stops it.
        captureEnabled.store (false);
        audioDeviceError (restartedDuringTake());
    }
}

juce::String StudioEngine::restartedDuringTake()
{
    return juce::String (juce::CharPointer_UTF8 ("el dispositivo de audio se detuvo o se reinici\xc3\xb3 durante la toma"));
}

void StudioEngine::audioDeviceError (const juce::String& message)
{
    const juce::ScopedLock sl (errorLock);
    lastError = message;
}

//==============================================================================
void StudioEngine::startRunning() noexcept
{
    running = true;
    renderer.snapRamps();
    fadeIndex = 0;                                   // fades in from silence
    stopPending = false;
}

void StudioEngine::applyRequests() noexcept
{
    Requests r;

    {
        const juce::SpinLock::ScopedTryLockType tl (requestLock);

        if (! tl.isLocked())
            return;                                  // picked up next block

        r = requests;
    }

    if (r.recordSerial != seenRecord)
    {
        seenRecord = r.recordSerial;
        seenSeek = r.seekSerial;                     // the take's start supersedes earlier seeks
        recordPointAudio = r.recordPoint;
        countInStartAudio = r.recordStart;
        capturing = false;                           // nothing of a previous take may leak into this one
        countInActive = false;

        if (! running)
        {
            position = r.recordStart;
            startRunning();
            capturing = true;
            countInActive = r.countIn;
        }
        else
        {
            jumpPending = true;
            jumpTarget = r.recordStart;
            captureOnJump = true;
            countInOnJump = r.countIn;
            stopPending = false;
        }
    }
    else if (r.seekSerial != seenSeek)
    {
        seenSeek = r.seekSerial;

        if (! running)
        {
            position = r.seekTarget;
        }
        else
        {
            jumpPending = true;
            jumpTarget = r.seekTarget;
            captureOnJump = false;
        }
    }

    appliedSeekSerial.store (seenSeek);

    if (r.play)
    {
        if (! running)
            startRunning();
        else
            stopPending = false;                     // a stop fade turns back into a fade-in
    }
    else if (running && ! stopPending)
    {
        stopPending = true;
        captureOnJump = false;
    }
}

void StudioEngine::addMetronome (int64_t pos, int n, float* L, float* R) noexcept
{
    const double tempo = bpm.load (std::memory_order_relaxed);
    const int beats = beatsPerBar.load (std::memory_order_relaxed);
    int split = 0;

    // The count-in is one bar of clicks counted from its own start, so it always sounds complete.
    if (countInActive)
    {
        split = (int) juce::jlimit<int64_t> (0, n, recordPointAudio - pos);

        if (split > 0)
            renderMetronome (tempo, beats, pos - countInStartAudio, split, L, R);
    }

    if (clickOn.load (std::memory_order_relaxed) && split < n)
        renderMetronome (tempo, beats, pos + split, n - split, L + split, R + split);
}

int StudioEngine::runTransport (float* L, float* R, int n) noexcept
{
    int captureFrom = capturing ? 0 : -1;
    int done = 0;
    const auto* src = source.load();                 // read under inRender: see setSampleSource
    const bool hold = blockHold;

    while (running && done < n)
    {
        // The master fades out for a stop, a jump or a source swap (hold), and in otherwise.
        const bool cut = stopPending || jumpPending;
        const int direction = cut || hold ? -1 : 1;
        int len = n - done;

        if (cut)
            len = std::min (len, fadeIndex);         // the stop or the jump happens exactly where the fade reaches 0

        if (len > 0)
        {
            float* sl = scratchL.data() + done;
            float* sr = scratchR.data() + done;

            if (src != nullptr)
            {
                renderer.render (*src, position, len, sl, sr);
            }
            else
            {
                std::fill (sl, sl + len, 0.0f);
                std::fill (sr, sr + len, 0.0f);
            }

            addMetronome (position, len, sl, sr);

            for (int i = 0; i < len; ++i)
            {
                const float g = fadeTable[(size_t) fadeIndex];
                L[done + i] += g * sl[i];
                R[done + i] += g * sr[i];
                fadeIndex = juce::jlimit (0, transportFadeSamples, fadeIndex + direction);
            }

            position += len;                         // a hold keeps the timeline (and a take) running in silence
            done += len;
        }

        if (cut && fadeIndex == 0)
        {
            if (jumpPending)
            {
                jumpPending = false;
                position = jumpTarget;
                renderer.snapRamps();

                if (captureOnJump)
                {
                    captureOnJump = false;
                    capturing = true;
                    countInActive = countInOnJump;
                    captureFrom = done;
                }
            }

            if (stopPending)
            {
                stopPending = false;
                running = false;
            }
        }
    }

    return captureFrom;
}

void StudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                     float* const* outputs, int numOutputs, int numSamples,
                                                     const juce::AudioIODeviceCallbackContext&)
{
    callbackCount.fetch_add (1, std::memory_order_relaxed);

    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear (outputs[channel], numSamples);

    const int n = numSamples;

    if (! blockSupported.load (std::memory_order_relaxed) || n <= 0 || n > maxBlock)
        return;

    // 1. Inputs.
    const auto inputAt = [&] (int channel) -> const float*
    {
        return juce::isPositiveAndBelow (channel, numInputs) ? inputs[channel] : nullptr;
    };

    const float* voice = inputAt (micChannel.load (std::memory_order_relaxed));
    const float* instrument = inputAt (instrumentChannel.load (std::memory_order_relaxed));

    if (voice == nullptr)
        voice = silence.data();

    // 2. Plugin on the instrument (mono in, stereo out).
    if (instrument != nullptr)
        instrumentBuffer.copyFrom (0, 0, instrument, n);
    else
        instrumentBuffer.clear (0, 0, n);

    plugin.process (instrumentBuffer, n);
    const float* instrumentL = instrumentBuffer.getReadPointer (0);
    const float* instrumentR = instrumentBuffer.getReadPointer (1);

    // 3. Send gains: what the take, the chat, the meters and the monitor get. A gain of 1 keeps the samples exact.
    float* sentVoice = voiceSent.data();
    float* sentL = instrumentSentL.data();
    float* sentR = instrumentSentR.data();
    sendVoiceRamp.setTarget (sendVoiceGain.load (std::memory_order_relaxed), rampSamples);
    sendInstrumentRamp.setTarget (sendInstrumentGain.load (std::memory_order_relaxed), rampSamples);

    for (int i = 0; i < n; ++i)
    {
        sentVoice[i] = sendVoiceRamp.next() * voice[i];
        const float g = sendInstrumentRamp.next();
        sentL[i] = g * instrumentL[i];
        sentR[i] = g * instrumentR[i];
    }

    // 4. Own monitor, 0 ms: only the input of the armed kind (crossfaded when the armed kind changes).
    float* L = mixL.data();
    float* R = mixR.data();
    const int armed = armedKind.load (std::memory_order_relaxed);
    const int voiceDirection = armed == (int) TrackKind::voice ? 1 : -1;
    const int instrumentDirection = armed == (int) TrackKind::instrument ? 1 : -1;
    monitorVoiceRamp.setTarget (monitorVoiceGain.load (std::memory_order_relaxed), rampSamples);
    monitorInstrumentRamp.setTarget (monitorInstrumentGain.load (std::memory_order_relaxed), rampSamples);

    for (int i = 0; i < n; ++i)
    {
        monitorVoiceFade = juce::jlimit (0, transportFadeSamples, monitorVoiceFade + voiceDirection);
        monitorInstrumentFade = juce::jlimit (0, transportFadeSamples, monitorInstrumentFade + instrumentDirection);
        const float gv = monitorVoiceRamp.next() * fadeTable[(size_t) monitorVoiceFade];
        const float gi = monitorInstrumentRamp.next() * fadeTable[(size_t) monitorInstrumentFade];
        L[i] = gv * sentVoice[i] + gi * sentL[i];
        R[i] = gv * sentVoice[i] + gi * sentR[i];
    }

    int captureFrom = -1;

    if (calibrationRequested.load() && ! running && ! calibrator.isRunning())
    {
        calibrator.start();                          // running is published before the request is cleared
        calibrationRequested.store (false);
    }

    // inRender brackets every use of the sample source (hand-off with setSampleSource).
    inRender.store (true);
    blockHold = holdRequested.load();

    const bool calibratingBlock = calibrator.isRunning();

    if (calibratingBlock)
    {
        // 5. Calibration owns the output: the click train only (transport requests wait).
        calibrator.process (voice, L, R, n);
    }
    else
    {
        // 6. Transport: playback + metronome under the master fade.
        applyRequests();
        captureFrom = runTransport (L, R, n);
    }

    holdReached.store (blockHold && (! running || fadeIndex == 0));   // silent under a hold this block saw
    inRender.store (false);

    // 7. Recording: the selected inputs (post send gain), from the first sample of playback (count-in included).
    inPush.store (true);

    if (! captureEnabled.load())
    {
        capturing = false;
        countInActive = false;
    }
    else if (captureFrom >= 0 && captureFrom < n)
    {
        if (recordChannels.load (std::memory_order_relaxed) == 1)
        {
            const float* channels[1] = { sentVoice + captureFrom };
            recorder.push (channels, n - captureFrom);
        }
        else
        {
            const float* channels[2] = { sentL + captureFrom, sentR + captureFrom };
            recorder.push (channels, n - captureFrom);
        }
    }

    inPush.store (false);

    // 8. Voice chat: the own inputs out, the other participants' streams in (output only, never recorded).
    sendChat (sentVoice, sentL, sentR, n);
    // While calibrating the output is the click train only: the others' streams still run (buffers drain, fades
    // follow) but into scratch, so they cannot leak into the loopback and spoil the measured round trip.
    if (calibratingBlock)
    {
        std::fill (scratchL.begin(), scratchL.begin() + n, 0.0f);
        std::fill (scratchR.begin(), scratchR.begin() + n, 0.0f);
        mixRemote (scratchL.data(), scratchR.data(), n);
    }
    else
    {
        mixRemote (L, R, n);
    }

    // 9. Meters.
    const float voicePeak = peakOf (sentVoice, n);
    const float instrumentPeak = juce::jmax (peakOf (sentL, n), peakOf (sentR, n));
    storeMax (peakVoice, voicePeak);
    storeMax (peakInstrument, instrumentPeak);
    storeMax (peakOutput, juce::jmax (peakOf (L, n), peakOf (R, n)));

    if (armed == (int) TrackKind::voice)
        storeMax (peakArmed, voicePeak);
    else if (armed == (int) TrackKind::instrument)
        storeMax (peakArmed, instrumentPeak);

    const int pair = outputPair.load (std::memory_order_relaxed);

    if (pair >= 0 && pair * 2 + 1 < numOutputs && outputs[pair * 2] != nullptr && outputs[pair * 2 + 1] != nullptr)
    {
        std::copy (L, L + n, outputs[pair * 2]);
        std::copy (R, R + n, outputs[pair * 2 + 1]);
    }

    // 10. Position (advanced by runTransport) and state for the message thread.
    publishedPosition.store (position);
    audioPlaying.store (running);
}
}
