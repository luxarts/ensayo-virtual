#pragma once
#include "core/Protocol.h"
#include "core/OpusCodec.h"
#include "core/JitterBuffer.h"
#include "core/SpscQueue.h"
#include "core/MixMath.h"
#include "PluginSlot.h"
#include "FakeAudioDevice.h"
#include "AudioTransportInterface.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
namespace ensayo
{
// Called once per frame from the audio thread when this client hosts the embedded server (ServerHost::tick).
using FrameHook = std::function<void()>;

struct EnginePeaks { float voice = 0, instrument = 0, received = 0, output = 0; };

// AudioDeviceManager that only offers ASIO; the fake device type is added on demand by useFakeDevice.
class AsioDeviceManager : public juce::AudioDeviceManager
{
public:
    void createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>& types) override;
};

class AudioEngine : private juce::AudioIODeviceCallback
{
public:
    AudioEngine();
    ~AudioEngine() override;
    juce::AudioDeviceManager& getDeviceManager() noexcept;
    juce::StringArray getAsioDeviceNames();
    void useFakeDevice (int blockSize, TestSignal);        // registers FakeAudioDeviceType and opens it
    FakeAudioDevice* getFakeDevice() const noexcept;
    juce::String openDevice (const juce::String& deviceType, const juce::String& name, double sampleRate, int bufferSize); // "" on success
    juce::AudioIODevice* getDevice() const noexcept;
    PluginSlot& getPluginSlot() noexcept;

    void setChannels (int mic, int instrument, int outputPair) noexcept;
    void setRole (Role) noexcept;
    void setMonitor (const ChannelGain&) noexcept;         // own voice/instrument
    void setReceiveGain (float db) noexcept;
    void setTransport (AudioTransport*) noexcept;         // nullptr = offline (monitor only)
    void setFrameHook (FrameHook) ;                        // set while the device is stopped
    void setSimulatedLossPercent (int) noexcept;          // drops that % of received blocks before decode (tests)

    EnginePeaks takePeaks() noexcept;
    uint32_t getCallbackCount() const noexcept;
    FrameSize getFrameSize() const noexcept;               // from the device block size (256 → f128 ×2)
    juce::String takeDeviceError();
private:
    void audioDeviceIOCallbackWithContext (const float* const*, int, float* const*, int, int, const juce::AudioIODeviceCallbackContext&) override;
    void audioDeviceAboutToStart (juce::AudioIODevice*) override;
    void audioDeviceStopped() override;
    void audioDeviceError (const juce::String&) override;
    // per frame: voice/instr → plugin → monitor; encode voice+instr(L+R)/2 into AudioPacketView; encodeAudioPacket into
    // a stack buffer; transport->sendAudio; receiveBuffer().pop → OpusDecoder stereo (PLC) → addStereo(receiveGain);
    // frameHook() when set; sampleTime counter; seq counter.
    void processFrame (const float* voice, const float* instrumentL, const float* instrumentR,
                       float* outL, float* outR, int frame) noexcept;

    AsioDeviceManager deviceManager;
    PluginSlot plugin;

    // Codec state, (re)built on the message thread in audioDeviceAboutToStart.
    OpusMode mode64 { FrameSize::f64 }, mode128 { FrameSize::f128 };
    std::unique_ptr<OpusEncoder> voiceEncoder, instrumentEncoder;
    std::unique_ptr<OpusDecoder> mixDecoder;
    std::atomic<FrameSize> frameSize { FrameSize::f128 };
    std::atomic<bool> blockSupported { false };
    int frameLength = 128;                        // samples per frame while running

    // Audio-thread scratch, sized in audioDeviceAboutToStart.
    juce::AudioBuffer<float> instrumentBuffer;    // 2 x block
    std::vector<float> silence, monoInstrument, decodedInterleaved, decodedL, decodedR;
    std::array<uint8_t, maxPacketBytes> voiceCoded {}, instrumentCoded {}, receivedPacket {};

    FrameHook frameHook;
    std::atomic<AudioTransport*> transport { nullptr };
    std::atomic<int> micChannel { 0 }, instrumentChannel { 1 }, outputPair { 0 };
    std::atomic<Role> role { Role::both };
    std::atomic<float> monitorVoiceGain { 1.0f }, monitorInstrumentGain { 1.0f }, receiveGain { 1.0f };
    std::atomic<int> lossPercent { 0 };
    std::atomic<uint32_t> callbackCount { 0 };
    std::atomic<float> peakVoice { 0.0f }, peakInstrument { 0.0f }, peakReceived { 0.0f }, peakOutput { 0.0f };

    static constexpr int maxConcealedFrames = 20;   // ≈ 53 ms at 128: longer gaps are a dead stream → silence
    uint16_t seq = 0;                 // audio thread only
    uint32_t sampleTime = 0;          // audio thread only
    uint32_t lossRandomState = 0x9E3779B9u;
    bool decoderPrimed = false;       // a real block has been decoded since the device started
    int consecutiveLost = 0;

    juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE (AudioEngine)
};
}
