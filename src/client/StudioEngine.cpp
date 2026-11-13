#include "client/StudioEngine.h"
#include "core/MixMath.h"

#include <algorithm>
#include <cmath>
#include <thread>

namespace ensayo
{
namespace
{
constexpr int maxChannels = 64;
constexpr int previewFrame = 128;                    // frameSamples (FrameSize::f128)

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
struct StudioEngine::PreviewEncoder
{
    PreviewEncoder (const OpusMode& mode, int numChannels)
        : encoder (mode, numChannels), channels (numChannels), frame ((size_t) (previewFrame * numChannels), 0.0f) {}

    OpusEncoder encoder;
    const int channels;
    std::vector<float> frame;                        // interleaved, previewFrame frames
    int fill = 0;
    uint16_t seq = 0;
    uint32_t sampleTime = 0;
    std::array<uint8_t, 255> coded {};             // payloads are at most 255 bytes
    std::array<uint8_t, maxPacketBytes> packet {};
};

//==============================================================================
StudioEngine::StudioEngine()
{
    for (int k = 0; k <= transportFadeSamples; ++k)
        fadeTable[(size_t) k] = (float) (0.5 - 0.5 * std::cos (juce::MathConstants<double>::pi * k / transportFadeSamples));

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
    previewEncoder = std::make_unique<PreviewEncoder> (mode128, channels);
    userLiveAudio = recorder.onLiveAudio;
    userFinished = recorder.onFinished;

    recorder.onLiveAudio = [this] (const float* const* ch, int numChannels, int n)
    {
        encodePreview (ch, numChannels, n);

        if (userLiveAudio)
            userLiveAudio (ch, numChannels, n);
    };

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
    recorder.onLiveAudio = std::exchange (userLiveAudio, nullptr);
    recorder.onFinished = std::exchange (userFinished, nullptr);
    previewEncoder.reset();
}

void StudioEngine::encodePreview (const float* const* channels, int numChannels, int n)
{
    auto* p = previewEncoder.get();

    if (p == nullptr || ! onPreviewPacket || numChannels < 1)
        return;

    for (int i = 0; i < n; ++i)
    {
        for (int c = 0; c < p->channels; ++c)
            p->frame[(size_t) (p->fill * p->channels + c)] = channels[std::min (c, numChannels - 1)][i];

        if (++p->fill < previewFrame)
            continue;

        p->fill = 0;
        const int bytes = p->encoder.encode (p->frame.data(), p->coded.data(), (int) p->coded.size());

        AudioPacketView view;
        view.frameSize = FrameSize::f128;
        view.seq = p->seq++;
        view.clientId = 0;                           // the ProjectClient stamps its own id
        view.sampleTime = p->sampleTime;
        p->sampleTime += (uint32_t) previewFrame;

        if (bytes <= 0)
            continue;

        if (p->channels == 1)
        {
            view.flags = AudioFlags::voice;
            view.voice = p->coded.data();
            view.voiceLen = bytes;
        }
        else
        {
            view.flags = AudioFlags::instrument;
            view.instrument = p->coded.data();
            view.instrumentLen = bytes;
        }

        const auto written = encodeAudioPacket (view, p->packet.data(), p->packet.size());

        if (written > 0)
            onPreviewPacket (p->packet.data(), written);
    }
}

//==============================================================================
void StudioEngine::setPreviewBuffer (JitterBuffer* buffer) noexcept
{
    previewBuffer.store (buffer);

    while (inPreview.load())                         // the callback may still be popping from the previous one
        std::this_thread::yield();
}

void StudioEngine::addPreview (float* L, float* R, int n) noexcept
{
    inPreview.store (true);
    auto* buffer = previewBuffer.load();

    // Half-cosine over one frame: the first frame after silence fades in, the last concealed frame fades out.
    const auto ramp = [] (float* dl, float* dr, bool in) noexcept
    {
        for (int i = 0; i < previewFrame; ++i)
        {
            const auto c = (float) std::cos (juce::MathConstants<double>::pi * (i + 1) / previewFrame);
            const auto g = in ? 0.5f - 0.5f * c : 0.5f + 0.5f * c;
            dl[i] *= g;
            dr[i] *= g;
        }
    };

    if (buffer != lastPreviewBuffer)
    {
        // Listening turned off (or switched) while a stream sounds: after the samples already decoded, one more frame
        // is concealed by our own decoder (the old buffer is never touched again) and faded out, so the cut never steps.
        const auto& state = previewKind == 1 ? monoState : stereoState;
        const bool sounding = lastPreviewBuffer != nullptr && previewKind != 0 && state.primed;
        lastPreviewBuffer = buffer;

        if (sounding && previewFill + previewFrame <= (int) previewL.size())
        {
            float* dl = previewL.data() + previewFill;
            float* dr = previewR.data() + previewFill;

            if (previewKind == 1)
            {
                previewMono.decode (nullptr, 0, dl);
                std::copy (dl, dl + previewFrame, dr);
            }
            else
            {
                previewStereo.decode (nullptr, 0, previewInterleaved.data());
                deinterleave (previewInterleaved.data(), dl, dr, previewFrame);
            }

            ramp (dl, dr, false);
            previewFill += previewFrame;
        }

        // Otherwise previewFill is kept too: what is left is already faded out (the tail of a stream switched off,
        // possibly being played out while the buffer was nullptr) or silence, so it plays to its end and anything
        // new is decoded after it - cutting it short would step.

        monoState = {};
        stereoState = {};
        previewKind = 0;
    }

    // One 128-sample frame: a real payload (48 B mono, 96 B stereo), else concealment, else silence.
    const auto decodeFrame = [&] (JitterBuffer& jb, float* dl, float* dr) noexcept
    {
        int len = 0;
        const bool popped = jb.pop (previewPacket.data(), len);
        const int kind = ! popped ? 0
                       : len == codedBytesMono (FrameSize::f128) ? 1
                       : len == codedBytesStereo (FrameSize::f128) ? 2 : 0;

        if (kind != 0)
        {
            if (kind != previewKind)                 // another decoder: it starts unprimed
            {
                monoState = {};
                stereoState = {};
                previewKind = kind;
            }

            auto& state = kind == 1 ? monoState : stereoState;

            if (kind == 1)
            {
                previewMono.decode (previewPacket.data(), len, dl);
                std::copy (dl, dl + previewFrame, dr);
            }
            else
            {
                previewStereo.decode (previewPacket.data(), len, previewInterleaved.data());
                deinterleave (previewInterleaved.data(), dl, dr, previewFrame);
            }

            if (! state.primed)
                ramp (dl, dr, true);

            state.primed = true;
            state.lost = 0;
            return;
        }

        auto& state = previewKind == 1 ? monoState : stereoState;

        if (previewKind != 0 && state.primed && state.lost < maxConcealedFrames)
        {
            if (previewKind == 1)
            {
                previewMono.decode (nullptr, 0, dl);
                std::copy (dl, dl + previewFrame, dr);
            }
            else
            {
                previewStereo.decode (nullptr, 0, previewInterleaved.data());
                deinterleave (previewInterleaved.data(), dl, dr, previewFrame);
            }

            if (++state.lost == maxConcealedFrames)  // a dead stream: fade the last concealed frame out
            {
                ramp (dl, dr, false);
                state = {};
                previewKind = 0;
            }

            return;
        }

        std::fill (dl, dl + previewFrame, 0.0f);
        std::fill (dr, dr + previewFrame, 0.0f);
    };

    if (buffer != nullptr && previewFill + n <= (int) previewL.size())   // always true: sized block + 2 frames
    {
        // Whole 128-sample frames are decoded until the block can be served; the remainder (< 128) waits.
        while (previewFill < n)
        {
            decodeFrame (*buffer, previewL.data() + previewFill, previewR.data() + previewFill);
            previewFill += previewFrame;
        }

        addStereo (L, R, previewL.data(), previewR.data(), 1.0f, n);
        previewFill -= n;
        std::copy (previewL.begin() + n, previewL.begin() + n + previewFill, previewL.begin());
        std::copy (previewR.begin() + n, previewR.begin() + n + previewFill, previewR.begin());
    }
    else if (buffer == nullptr && previewFill > 0)
    {
        // The faded tail of a stream that was switched off.
        const int m = std::min (n, previewFill);
        addStereo (L, R, previewL.data(), previewR.data(), 1.0f, m);
        previewFill -= m;
        std::copy (previewL.begin() + m, previewL.begin() + m + previewFill, previewL.begin());
        std::copy (previewR.begin() + m, previewR.begin() + m + previewFill, previewR.begin());
    }

    inPreview.store (false);
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

    for (auto* v : { &silence, &mixL, &mixR, &scratchL, &scratchR })
        v->assign ((size_t) blockSize, 0.0f);

    previewL.assign ((size_t) (blockSize + 2 * previewFrame), 0.0f);
    previewR.assign ((size_t) (blockSize + 2 * previewFrame), 0.0f);
    previewInterleaved.assign ((size_t) previewFrame * 2, 0.0f);
    previewPacket.assign (maxPacketBytes, 0);
    previewFill = 0;

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

    // 3. Own monitor, 0 ms.
    float* L = mixL.data();
    float* R = mixR.data();
    std::fill (L, L + n, 0.0f);
    std::fill (R, R + n, 0.0f);
    addMonoCentred (L, R, voice, monitorVoiceGain.load (std::memory_order_relaxed), n);
    addStereo (L, R, instrumentL, instrumentR, monitorInstrumentGain.load (std::memory_order_relaxed), n);

    int captureFrom = -1;

    if (calibrationRequested.load() && ! running && ! calibrator.isRunning())
    {
        calibrator.start();                          // running is published before the request is cleared
        calibrationRequested.store (false);
    }

    // inRender brackets every use of the sample source (hand-off with setSampleSource).
    inRender.store (true);
    blockHold = holdRequested.load();

    if (calibrator.isRunning())
    {
        // 4. Calibration owns the output: the click train only (transport requests wait).
        calibrator.process (voice, L, R, n);
    }
    else
    {
        // 5. Transport: playback + metronome under the master fade.
        applyRequests();
        captureFrom = runTransport (L, R, n);
    }

    holdReached.store (blockHold && (! running || fadeIndex == 0));   // silent under a hold this block saw
    inRender.store (false);

    // 6. Recording: the selected inputs, from the first sample of playback (count-in included).
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
            const float* channels[1] = { voice + captureFrom };
            recorder.push (channels, n - captureFrom);
        }
        else
        {
            const float* channels[2] = { instrumentL + captureFrom, instrumentR + captureFrom };
            recorder.push (channels, n - captureFrom);
        }
    }

    inPush.store (false);

    // 7. Live listening: added to the output only.
    addPreview (L, R, n);

    // 8. Meters.
    storeMax (peakVoice, peakOf (voice, n));
    storeMax (peakInstrument, juce::jmax (peakOf (instrumentL, n), peakOf (instrumentR, n)));
    storeMax (peakOutput, juce::jmax (peakOf (L, n), peakOf (R, n)));

    const int pair = outputPair.load (std::memory_order_relaxed);

    if (pair >= 0 && pair * 2 + 1 < numOutputs && outputs[pair * 2] != nullptr && outputs[pair * 2 + 1] != nullptr)
    {
        std::copy (L, L + n, outputs[pair * 2]);
        std::copy (R, R + n, outputs[pair * 2 + 1]);
    }

    // 9. Position (advanced by runTransport) and state for the message thread.
    publishedPosition.store (position);
    audioPlaying.store (running);
}
}
