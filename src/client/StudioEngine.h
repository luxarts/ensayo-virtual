#pragma once
#include "core/Render.h"
#include "client/Recorder.h"
#include "client/LatencyCalibrator.h"
#include "client/PluginSlot.h"
#include "client/FakeAudioDevice.h"
#include "core/RemoteStreams.h"
#include "core/OpusCodec.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <thread>
#include <vector>
namespace ensayo
{
class StudioDeviceManager : public juce::AudioDeviceManager
{
public:
    void createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>&) override;   // ASIO only
};

struct EngineMeters { float voice = 0, instrument = 0, output = 0; };
struct RecordingResult { Id take; int channels = 1; int segments = 0; int64_t samples = 0; Region region; };

class StudioEngine : private juce::AudioIODeviceCallback
{
public:
    StudioEngine();
    ~StudioEngine() override;
    juce::AudioDeviceManager& getDeviceManager() noexcept;
    juce::StringArray getAsioDeviceNames();
    juce::String openDevice (const juce::String& type, const juce::String& name, double rate, int bufferSize);   // "" ok
    void useFakeDevice (int blockSize, TestSignal, FakeLatency = {});
    FakeAudioDevice* getFakeDevice() const noexcept;
    PluginSlot& getPluginSlot() noexcept;
    juce::String takeDeviceError();

    void setChannels (int mic, int instrument, int outputPair) noexcept;
    void setMonitor (float voiceGain, float instrumentGain) noexcept;      // own inputs, 0 ms
    void setCalibration (int samples) noexcept;
    void setMetronome (bool click, bool countIn) noexcept;

    void setSampleSource (const SampleSource*) noexcept;                     // the project's TakeCache or nullptr
    // setSampleSource: message thread only (it waits for the audio thread). When it returns the callback no longer references the previous source (it may be destroyed).
    // While playing, the master fades out, the source is swapped in silence and the master fades back in; the
    // timeline keeps advancing meanwhile, so a running take stays aligned.
    void setProject (std::shared_ptr<const Project>);
    void setSolo (std::set<Id>);

    // Transport (message thread). Position in timeline samples. play(), stop() and setPosition() are no-ops while
    // recording: a take ends only with stopRecording(), which also stops the transport.
    void play();
    void stop();
    void setPosition (int64_t);
    int64_t getPosition() const noexcept;
    bool isPlaying() const noexcept;

    // Records the inputs that match `track.kind` (voice: mic channel, mono; instrument: plugin output, stereo) from the
    // current position. Starts playback beatsPerBar beats earlier when count-in is on. Segments, peaks and live audio
    // come out through the Recorder callbacks set by the caller on getRecorder() before calling this.
    juce::Result startRecording (const Track&, const Id& take, const juce::File& localWav);
    // Stops the take and playback; the result's region is aligned with recordingOffset and starts at the record point.
    std::optional<RecordingResult> stopRecording();
    bool isRecording() const noexcept;
    // The take's record point (timeline) and recording offset (count-in + latencies + calibration), set by
    // startRecording before the recorder starts; valid until the next startRecording.
    int64_t getRecordPoint() const noexcept;
    int64_t getRecordOffset() const noexcept;
    Recorder& getRecorder() noexcept;

    // Own inputs. Send gains (linear) scale the mic and the instrument (post plugin) before everything else uses them:
    // the recorder, the chat, the armed meter and the monitor. Ramped over rampSamples; 1.0 leaves the take bit-exact.
    void setSendGains (float voice, float instrument) noexcept;
    // The kind of the armed own track: the monitor plays only that input (voice: mic, centred; instrument: plugin
    // output, stereo), crossfaded over transportFadeSamples on every change. nullopt (or imported) = no monitor.
    void setArmed (std::optional<TrackKind>) noexcept;

    // Voice chat, send side. While enabled the mic (post send gain, mono) is encoded continuously into 128-sample Opus
    // packets; while a take records an instrument track the instrument (post send gain, stereo) goes out as a second
    // stream. Packets come out through onChatPacket on the engine's chat-encoder thread (clientId 0: the sender stamps
    // it). Set onChatPacket on the message thread before setChatEnabled (true) and never reassign it while enabled.
    // A start or a stop of a stream (enable, mute, an instrument take) is faded over one frame; the sequence number
    // and sampleTime count every frame of time, sent or not.
    std::function<void (StreamKind, const uint8_t* packet, size_t len)> onChatPacket;
    // Message thread. setChatEnabled (false) returns once onChatPacket is no longer running and will not be called.
    void setChatEnabled (bool);
    // Stops sending the voice (its last frame fades out). Neither the take nor the monitor change.
    void setMicMuted (bool) noexcept;

    // Voice chat, receive side: the streams of the other participants (ProjectClient::remoteStreams()), nullptr = none.
    // Message thread; when it returns the callback no longer touches the previous table (it may be destroyed). Every
    // stream that was sounding fades out over one concealed frame.
    void setRemoteStreams (RemoteStreamTable*) noexcept;
    // Personal level of one sender's streams (voice and instrument), linear; muted = 0. Ramped per block. Senders
    // without an entry play at 1.0 (64 entries; when full, one at 1.0 or one whose sender has no stream in the table
    // is reused). Message thread only.
    void setParticipantGain (uint32_t clientId, float gain, bool muted) noexcept;
    void removeParticipantGain (uint32_t clientId) noexcept;     // back to 1.0; message thread only
    void clearParticipantGains() noexcept;                       // every sender back to 1.0; message thread only
    float participantGain (uint32_t clientId) const noexcept;    // any thread; 1.0 without an entry

    void startCalibration();
    bool isCalibrating() const noexcept;
    std::optional<int> calibrationRoundTrip() const;

    EngineMeters takeMeters() noexcept;
    // Message thread. Peak of every track of the project since the last call (post volume, pan and mute, pre master),
    // then reset. A peak >= clipLevel is a clip.
    std::vector<std::pair<Id, float>> takeTrackMeters();
    // Peak of the armed kind's input (post send gain) since the last call, then reset; 0 when nothing is armed.
    float takeArmedInputPeak() noexcept;
    static constexpr float clipLevel = 1.0f;
    uint32_t getCallbackCount() const noexcept;

private:
    // What the message thread wants; the callback reconciles its transport with it at the start of every block.
    // Serials mark one-shot requests (seek, record) so none is lost when several arrive between two callbacks.
    struct Requests
    {
        bool play = false;
        uint32_t seekSerial = 0, recordSerial = 0;
        int64_t seekTarget = 0;
        int64_t recordStart = 0, recordPoint = 0;           // recordStart = recordPoint - count-in
        bool countIn = false;
    };
    // Linear gain ramp, advanced once per sample on the audio thread.
    struct GainRamp
    {
        float current = 1.0f, target = 1.0f, step = 0.0f;
        int remaining = 0;

        void setTarget (float value, int length) noexcept
        {
            if (value == target)
                return;

            target = value;
            remaining = length;
            step = (target - current) / (float) length;
        }

        float next() noexcept
        {
            const float g = current;

            if (remaining > 0)
                current = --remaining == 0 ? target : current + step;

            return g;
        }
    };
    // One outgoing chat stream (voice or instrument): frames built on the audio thread, encoded on the chat thread.
    struct ChatSender;
    // The receive state of one RemoteStreamTable slot: decoders, concealment, decoded samples and gain.
    struct RemoteSlotMix;

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;
    static juce::String restartedDuringTake();         // the error set when the device stops or restarts mid-take

    void applyRequests() noexcept;                                                    // audio thread
    void requestPlay (bool) noexcept;                                                 // message thread
    void startRunning() noexcept;                                                     // audio thread
    // Audio thread: plays the transport into L/R ([0, n)); returns the block index where capture starts, or -1.
    int runTransport (float* L, float* R, int n) noexcept;
    void addMetronome (int64_t position, int n, float* L, float* R) noexcept;        // audio thread
    void sendChat (const float* voice, const float* instrumentL, const float* instrumentR, int n) noexcept;   // audio
    void mixRemote (float* L, float* R, int n) noexcept;                              // audio thread
    void chatLoop();                                                                  // chat thread
    void disableCapture() noexcept;                                                   // message thread
    void restoreRecorderCallbacks();                                                  // message thread

    StudioDeviceManager deviceManager;
    PluginSlot plugin;
    Renderer renderer;
    Recorder recorder;
    LatencyCalibrator calibrator;
    OpusMode mode128 { FrameSize::f128 };
    std::array<float, transportFadeSamples + 1> fadeTable {};   // half-cosine 0 → 1

    // Settings (message thread → audio thread).
    std::atomic<int> micChannel { 0 }, instrumentChannel { 1 }, outputPair { 0 };
    std::atomic<float> monitorVoiceGain { 1.0f }, monitorInstrumentGain { 1.0f };
    std::atomic<float> sendVoiceGain { 1.0f }, sendInstrumentGain { 1.0f };
    std::atomic<int> armedKind { -1 };                         // (int) TrackKind of the armed track, -1 = none
    std::atomic<int> calibrationSamples { 0 };
    std::atomic<bool> clickOn { false }, countInOn { false };
    std::atomic<double> bpm { 120.0 };
    std::atomic<int> beatsPerBar { 4 };
    std::atomic<const SampleSource*> source { nullptr };
    std::atomic<bool> blockSupported { false };

    // Requests: written by the message thread under the lock; the callback only try-locks and copies.
    mutable juce::SpinLock requestLock;
    Requests requests;
    std::atomic<uint32_t> appliedSeekSerial { 0 };
    std::atomic<bool> wantPlaying { false };

    // Transport state published by the callback.
    std::atomic<int64_t> publishedPosition { 0 };
    std::atomic<bool> audioPlaying { false };

    // Audio-thread transport state.
    uint32_t seenSeek = 0, seenRecord = 0;
    int64_t position = 0;
    bool running = false;
    int fadeIndex = 0;                                         // 0..transportFadeSamples (gain 0 → 1)
    bool stopPending = false, jumpPending = false, captureOnJump = false, countInOnJump = false;
    int64_t jumpTarget = 0;
    bool capturing = false;                                    // pushing to the recorder
    bool countInActive = false;
    int64_t recordPointAudio = 0, countInStartAudio = 0;

    // Capture hand-off (Dekker style: both sides use sequentially consistent stores and loads).
    std::atomic<bool> captureEnabled { false }, inPush { false };
    std::atomic<int> recordChannels { 1 };

    // Recording state (message thread; `recording` is also read by isRecording()).
    std::atomic<bool> recording { false };
    Id recordTake;
    int64_t recordPoint = 0, recordOffset = 0;
    std::function<void (int, int64_t)> userFinished;
    int finishedSegments = 0;                                  // written on the writer thread before stop() returns
    int64_t finishedSamples = 0;

    // Own inputs on the audio thread: send gains, monitor gains and the monitor's armed-kind fades
    // (0..transportFadeSamples, gain fadeTable[index]).
    GainRamp sendVoiceRamp, sendInstrumentRamp, monitorVoiceRamp, monitorInstrumentRamp;
    int monitorVoiceFade = 0, monitorInstrumentFade = 0;

    // Chat send. The audio thread builds 128-sample frames aligned to chatClock and queues them; the chat thread
    // encodes them and calls onChatPacket, flagging inChatSink around the call (Dekker with setChatEnabled (false)).
    std::atomic<bool> chatEnabled { false }, micMuted { false }, inChatSink { false }, chatQuit { false };
    std::atomic<uint32_t> chatSignal { 0 };                    // bumped (and notified) when frames are queued
    std::unique_ptr<ChatSender> chatVoice, chatInstrument;
    int64_t chatClock = 0;                                     // audio thread: samples since the device started
    std::thread chatThread;

    // Chat receive. The callback flags inRemote while it uses the table (same hand-off as capture).
    std::atomic<RemoteStreamTable*> remoteTable { nullptr };
    std::atomic<bool> inRemote { false };
    RemoteStreamTable* lastRemoteTable = nullptr;              // audio thread
    // Packet-loss concealment: only after a real frame, for at most maxConcealedFrames, then a faded cut.
    static constexpr int maxConcealedFrames = 18;              // 48 ms of 128-sample frames
    std::array<std::unique_ptr<RemoteSlotMix>, RemoteStreamTable::capacity> remoteMix;
    std::vector<uint8_t> remotePacket;
    std::vector<float> remoteInterleaved;
    // Per-sender gains: clientId << 32 | the gain's float bits; 0 = free entry.
    static constexpr int participantCapacity = 64;
    std::array<std::atomic<uint64_t>, participantCapacity> participantGains {};

    // Sample source swap: the message thread asks for silence (hold), the callback fades the master to 0 and reports
    // it, the pointer is swapped with the inRender hand-off, then the hold is released and the master fades back in.
    std::atomic<bool> holdRequested { false }, holdReached { false }, inRender { false };
    bool blockHold = false;                                    // audio thread: the hold as loaded for this block

    // Calibration: requested by the message thread, started by the callback once the transport is stopped.
    std::atomic<bool> calibrationRequested { false };

    // Audio-thread scratch, sized in audioDeviceAboutToStart.
    juce::AudioBuffer<float> instrumentBuffer;                 // 2 x block
    std::vector<float> silence, mixL, mixR, scratchL, scratchR, voiceSent, instrumentSentL, instrumentSentR;
    int maxBlock = 0;

    std::atomic<uint32_t> callbackCount { 0 };
    std::atomic<float> peakVoice { 0.0f }, peakInstrument { 0.0f }, peakOutput { 0.0f }, peakArmed { 0.0f };

    juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE (StudioEngine)
};
}
