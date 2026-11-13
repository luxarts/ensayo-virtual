#pragma once
#include "core/Protocol.h"
#include <juce_audio_devices/juce_audio_devices.h>
#include <atomic>
#include <functional>
#include <vector>
namespace ensayo
{
// click: +1.0 impulse every 250 ms on both inputs. loopback: input = output delayed by the latencies below
// (input channel 0 = output L, channel 1 = output R; the first samples are 0).
enum class TestSignal { silence, sine440, click, loopback };
struct FakeLatency { int input = 0, output = 0;                 // reported AND applied in loopback mode
                     int hidden = 0; };                          // applied in loopback but not reported (what calibration finds)
struct ClickEvent { int64_t sampleIndex; double qpcMs; };   // qpcMs = juce::Time::getMillisecondCounterHiRes()

// A std::function that may be (re)assigned from any thread while the device thread invokes it.
// Assignment and invocation are serialised by a spin lock, so hooks can be installed after start().
template <typename Signature>
class LockedCallback
{
public:
    LockedCallback() = default;
    LockedCallback (std::function<Signature> f) : fn (std::move (f)) {}

    LockedCallback& operator= (std::function<Signature> f)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        fn = std::move (f);
        return *this;
    }

    LockedCallback& operator= (std::nullptr_t)
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        fn = nullptr;
        return *this;
    }

    explicit operator bool() const
    {
        const juce::SpinLock::ScopedLockType sl (lock);
        return fn != nullptr;
    }

    template <typename... Args>
    void operator() (Args&&... args) const
    {
        const juce::SpinLock::ScopedLockType sl (lock);

        if (fn != nullptr)
            fn (std::forward<Args> (args)...);
    }

private:
    mutable juce::SpinLock lock;
    std::function<Signature> fn;

    LockedCallback (const LockedCallback&) = delete;
    LockedCallback& operator= (const LockedCallback&) = delete;
};

// Two inputs, two outputs, thread-driven at exact block cadence (waitable-timer driven).
class FakeAudioDevice : public juce::AudioIODevice
{
public:
    FakeAudioDevice (int blockSize, TestSignal, FakeLatency = {});
    ~FakeAudioDevice() override;

    // Called on the device thread after every callback with the output block; used by the probe.
    LockedCallback<void (const float* L, const float* R, int n, int64_t firstSampleIndex, double qpcMs)> onOutputBlock;
    LockedCallback<void (ClickEvent)> onClickEmitted;    // when TestSignal::click fires

    static constexpr int clickPeriodSamples = sampleRate / 4;   // 250 ms

    // juce::AudioIODevice overrides: names "Fake In/Out L/R", sample rates {48000}, buffer sizes {64,128,256,512,1024},
    // latencies = the FakeLatency values. Loopback needs input + output + hidden latency >= the block size (a real round trip
    // always spans at least one block); open() fails otherwise.
    juce::StringArray getOutputChannelNames() override;
    juce::StringArray getInputChannelNames() override;
    juce::Array<double> getAvailableSampleRates() override;
    juce::Array<int> getAvailableBufferSizes() override;
    int getDefaultBufferSize() override;
    juce::String open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                       double sampleRate, int bufferSizeSamples) override;
    void close() override;
    bool isOpen() override;
    void start (juce::AudioIODeviceCallback*) override;
    void stop() override;
    bool isPlaying() override;
    juce::String getLastError() override;
    int getCurrentBufferSizeSamples() override;
    double getCurrentSampleRate() override;
    int getCurrentBitDepth() override;
    juce::BigInteger getActiveOutputChannels() const override;
    juce::BigInteger getActiveInputChannels() const override;
    int getOutputLatencyInSamples() override;
    int getInputLatencyInSamples() override;

private:
    class DeviceThread;

    void renderBlock (juce::AudioIODeviceCallback*) noexcept;   // device thread
    void fillInputs (double qpcMs) noexcept;

    const int requestedBlockSize;
    const TestSignal signal;
    const FakeLatency latency;
    int blockSize;
    juce::BigInteger activeInputs, activeOutputs;
    std::atomic<bool> opened { false }, playing { false };
    juce::String lastError;

    std::vector<float> inL, inR, outL, outR;
    std::vector<float> pastL, pastR;   // loopback: ring of past output, indexed by sampleIndex % size
    int64_t sampleIndex = 0;        // device thread
    double sinePhase = 0.0;         // device thread

    std::unique_ptr<DeviceThread> thread;
    juce::AudioIODeviceCallback* callback = nullptr;

    JUCE_DECLARE_NON_COPYABLE (FakeAudioDevice)
};

class FakeAudioDeviceType : public juce::AudioIODeviceType   // typeName "Fake"; one device "Fake Audio"
{
public:
    FakeAudioDeviceType (int blockSize, TestSignal, FakeLatency = {});

    static const char* deviceName() noexcept { return "Fake Audio"; }
    static const char* typeName() noexcept { return "Fake"; }

    void scanForDevices() override;
    juce::StringArray getDeviceNames (bool wantInputNames = false) const override;
    int getDefaultDeviceIndex (bool forInput) const override;
    int getIndexOfDevice (juce::AudioIODevice*, bool asInput) const override;
    bool hasSeparateInputsAndOutputs() const override;
    juce::AudioIODevice* createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName) override;

private:
    const int blockSize;
    const TestSignal signal;
    const FakeLatency latency;
};

// Detects clicks (sample > 0.5 after 20 ms of quiet) in a stream and reports arrival time.
class ClickProbe
{
public:
    std::function<void (ClickEvent)> onClickDetected;
    void process (const float* L, const float* R, int n, int64_t firstSampleIndex, double qpcMs) noexcept;

    static constexpr float threshold = 0.5f;
    static constexpr int quietSamples = sampleRate / 50;   // 20 ms

private:
    int64_t quietRun = quietSamples;   // starts "quiet" so the very first click counts
};
}
