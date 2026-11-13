#pragma once
#include "core/Render.h"
#include "client/Recorder.h"
#include "client/LatencyCalibrator.h"
#include "client/PluginSlot.h"
#include "client/FakeAudioDevice.h"
#include "core/JitterBuffer.h"
#include "core/OpusCodec.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <set>
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
    void setProject (std::shared_ptr<const Project>);
    void setSolo (std::set<Id>);

    // Transport (message thread). Position in timeline samples.
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
    Recorder& getRecorder() noexcept;

    void setPreviewBuffer (JitterBuffer*) noexcept;    // live listening; nullptr = off
    // Encodes the recorder's live audio into 128-sample Opus frames and passes datagrams here (any thread).
    std::function<void (const uint8_t* packet, size_t len)> onPreviewPacket;

    void startCalibration();
    bool isCalibrating() const noexcept;
    std::optional<int> calibrationRoundTrip() const;

    EngineMeters takeMeters() noexcept;
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
    // Encodes the live take on the Recorder's writer thread (128-sample frames).
    struct PreviewEncoder;

    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int,
                                           const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;

    void applyRequests() noexcept;                                                    // audio thread
    void startRunning() noexcept;                                                     // audio thread
    // Audio thread: plays the transport into L/R ([0, n)); returns the block index where capture starts, or -1.
    int runTransport (float* L, float* R, int n) noexcept;
    void addMetronome (int64_t position, int n, float* L, float* R) noexcept;        // audio thread
    void addPreview (float* L, float* R, int n) noexcept;                             // audio thread
    void encodePreview (const float* const* channels, int numChannels, int n);        // writer thread
    void disableCapture() noexcept;                                                   // message thread
    void restoreRecorderCallbacks();                                                  // message thread

    StudioDeviceManager deviceManager;
    PluginSlot plugin;
    Renderer renderer;
    Recorder recorder;
    LatencyCalibrator calibrator;
    OpusMode mode128 { FrameSize::f128 };
    OpusDecoder previewMono { mode128, 1 }, previewStereo { mode128, 2 };
    std::array<float, transportFadeSamples + 1> fadeTable {};   // half-cosine 0 → 1

    // Settings (message thread → audio thread).
    std::atomic<int> micChannel { 0 }, instrumentChannel { 1 }, outputPair { 0 };
    std::atomic<float> monitorVoiceGain { 1.0f }, monitorInstrumentGain { 1.0f };
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
    int fadeIndex = 0, fadeDirection = 0;                      // 0..transportFadeSamples; +1 in, -1 out, 0 idle
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
    std::function<void (const float* const*, int, int)> userLiveAudio;
    std::function<void (int, int64_t)> userFinished;
    int finishedSegments = 0;                                  // written on the writer thread before stop() returns
    int64_t finishedSamples = 0;
    std::unique_ptr<PreviewEncoder> previewEncoder;

    // Live listening (receive). The callback flags inPreview while it uses the buffer (same hand-off as capture).
    std::atomic<JitterBuffer*> previewBuffer { nullptr };
    std::atomic<bool> inPreview { false };
    JitterBuffer* lastPreviewBuffer = nullptr;                 // audio thread
    std::vector<float> previewL, previewR, previewInterleaved;
    std::vector<uint8_t> previewPacket;
    int previewFill = 0;

    // Calibration: requested by the message thread, started by the callback once the transport is stopped.
    std::atomic<bool> calibrationRequested { false };

    // Audio-thread scratch, sized in audioDeviceAboutToStart.
    juce::AudioBuffer<float> instrumentBuffer;                 // 2 x block
    std::vector<float> silence, mixL, mixR, scratchL, scratchR;
    int maxBlock = 0;

    std::atomic<uint32_t> callbackCount { 0 };
    std::atomic<float> peakVoice { 0.0f }, peakInstrument { 0.0f }, peakOutput { 0.0f };

    juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE (StudioEngine)
};
}
