#include "FakeAudioDevice.h"

#include <cmath>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

namespace ensayo
{
namespace
{
constexpr double fakeSampleRate = (double) sampleRate;

int64_t fileTimeNow() noexcept
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime (&ft);
    return (int64_t) (((uint64_t) ft.dwHighDateTime << 32) | ft.dwLowDateTime);
}
}

//==============================================================================
// Waits on a high-resolution waitable timer armed with absolute due times so the cadence never drifts.
class FakeAudioDevice::DeviceThread : public juce::Thread
{
public:
    DeviceThread (FakeAudioDevice& d, int blockSize)
        : juce::Thread ("FakeAudioDevice"), device (d),
          periodTicks ((double) blockSize * 1.0e7 / fakeSampleRate)
    {
        timer = CreateWaitableTimerExW (nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);

        if (timer == nullptr)
        {
            timer = CreateWaitableTimerExW (nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            timeBeginPeriod (1);
            usesTimePeriod = true;
        }

        wakeEvent = CreateEventW (nullptr, TRUE, FALSE, nullptr);
    }

    ~DeviceThread() override
    {
        shutdown();

        if (timer != nullptr)     CloseHandle (timer);
        if (wakeEvent != nullptr) CloseHandle (wakeEvent);
        if (usesTimePeriod)       timeEndPeriod (1);
    }

    void shutdown()
    {
        signalThreadShouldExit();

        if (wakeEvent != nullptr)
            SetEvent (wakeEvent);

        stopThread (2000);
    }

    void run() override
    {
        int64_t start = fileTimeNow();
        uint64_t tick = 0;

        while (! threadShouldExit())
        {
            device.renderBlock (device.callback);
            ++tick;

            int64_t due = start + (int64_t) std::llround ((double) tick * periodTicks);
            const int64_t now = fileTimeNow();

            if (due < now - 500000)   // more than 50 ms behind: resync instead of bursting
            {
                due = now;
                start = now;
                tick = 0;
            }

            if (timer == nullptr)
            {
                juce::Thread::sleep (1);
                continue;
            }

            LARGE_INTEGER dueTime;
            dueTime.QuadPart = due;

            if (! SetWaitableTimer (timer, &dueTime, 0, nullptr, nullptr, FALSE))
            {
                juce::Thread::sleep (1);
                continue;
            }

            HANDLE handles[2] = { timer, wakeEvent };
            WaitForMultipleObjects (2, handles, FALSE, 100);
        }
    }

private:
    FakeAudioDevice& device;
    const double periodTicks;
    HANDLE timer = nullptr, wakeEvent = nullptr;
    bool usesTimePeriod = false;
};

//==============================================================================
FakeAudioDevice::FakeAudioDevice (int blockSizeToUse, TestSignal signalToUse)
    : juce::AudioIODevice (FakeAudioDeviceType::deviceName(), FakeAudioDeviceType::typeName()),
      requestedBlockSize (blockSizeToUse > 0 ? blockSizeToUse : 128),
      signal (signalToUse),
      blockSize (requestedBlockSize)
{
}

FakeAudioDevice::~FakeAudioDevice()
{
    close();
}

juce::StringArray FakeAudioDevice::getOutputChannelNames()      { return { "Fake Out L", "Fake Out R" }; }
juce::StringArray FakeAudioDevice::getInputChannelNames()       { return { "Fake In L", "Fake In R" }; }
juce::Array<double> FakeAudioDevice::getAvailableSampleRates()  { return { fakeSampleRate }; }
juce::Array<int> FakeAudioDevice::getAvailableBufferSizes()     { return { 64, 128, 256 }; }
int FakeAudioDevice::getDefaultBufferSize()                     { return requestedBlockSize; }

juce::String FakeAudioDevice::open (const juce::BigInteger& inputChannels, const juce::BigInteger& outputChannels,
                                    double sampleRateToUse, int bufferSizeSamples)
{
    close();
    lastError.clear();

    if (sampleRateToUse > 0.0 && (int) std::lround (sampleRateToUse) != sampleRate)
    {
        lastError = "El dispositivo falso solo funciona a 48000 Hz";
        return lastError;
    }

    const int size = bufferSizeSamples > 0 ? bufferSizeSamples : requestedBlockSize;

    if (! getAvailableBufferSizes().contains (size))
    {
        lastError = "Tamano de bloque no soportado: " + juce::String (size);
        return lastError;
    }

    blockSize = size;
    activeInputs.clear();
    activeOutputs.clear();

    for (int c = 0; c < 2; ++c)   // only the two fake channels exist
    {
        activeInputs.setBit (c, inputChannels[c]);
        activeOutputs.setBit (c, outputChannels[c]);
    }

    inL.assign ((size_t) blockSize, 0.0f);
    inR.assign ((size_t) blockSize, 0.0f);
    outL.assign ((size_t) blockSize, 0.0f);
    outR.assign ((size_t) blockSize, 0.0f);
    sampleIndex = 0;
    sinePhase = 0.0;
    opened.store (true);
    return {};
}

void FakeAudioDevice::close()
{
    stop();
    opened.store (false);
}

bool FakeAudioDevice::isOpen()                           { return opened.load(); }
bool FakeAudioDevice::isPlaying()                        { return playing.load(); }
juce::String FakeAudioDevice::getLastError()             { return lastError; }
int FakeAudioDevice::getCurrentBufferSizeSamples()       { return blockSize; }
double FakeAudioDevice::getCurrentSampleRate()           { return fakeSampleRate; }
int FakeAudioDevice::getCurrentBitDepth()                { return 32; }
juce::BigInteger FakeAudioDevice::getActiveOutputChannels() const { return activeOutputs; }
juce::BigInteger FakeAudioDevice::getActiveInputChannels() const  { return activeInputs; }
int FakeAudioDevice::getOutputLatencyInSamples()         { return 0; }
int FakeAudioDevice::getInputLatencyInSamples()          { return 0; }

void FakeAudioDevice::start (juce::AudioIODeviceCallback* newCallback)
{
    if (! opened.load())
        return;

    stop();

    callback = newCallback;
    sampleIndex = 0;
    sinePhase = 0.0;

    if (callback != nullptr)
        callback->audioDeviceAboutToStart (this);

    thread = std::make_unique<DeviceThread> (*this, blockSize);
    thread->startThread (juce::Thread::Priority::highest);
    playing.store (true);
}

void FakeAudioDevice::stop()
{
    if (thread != nullptr)
    {
        thread->shutdown();
        thread.reset();
    }

    playing.store (false);

    if (auto* old = std::exchange (callback, nullptr))
        old->audioDeviceStopped();
}

void FakeAudioDevice::fillInputs (double qpcMs) noexcept
{
    const int n = blockSize;

    switch (signal)
    {
        case TestSignal::silence:
            juce::FloatVectorOperations::clear (inL.data(), n);
            juce::FloatVectorOperations::clear (inR.data(), n);
            break;

        case TestSignal::sine440:
        {
            const double increment = juce::MathConstants<double>::twoPi * 440.0 / fakeSampleRate;

            for (int i = 0; i < n; ++i)
            {
                const auto s = (float) (0.5 * std::sin (sinePhase));
                inL[(size_t) i] = s;
                inR[(size_t) i] = s;
                sinePhase += increment;

                if (sinePhase >= juce::MathConstants<double>::twoPi)
                    sinePhase -= juce::MathConstants<double>::twoPi;
            }

            break;
        }

        case TestSignal::click:
        {
            juce::FloatVectorOperations::clear (inL.data(), n);
            juce::FloatVectorOperations::clear (inR.data(), n);

            for (int i = 0; i < n; ++i)
            {
                const int64_t index = sampleIndex + i;

                if (index % clickPeriodSamples == 0)
                {
                    inL[(size_t) i] = 1.0f;
                    inR[(size_t) i] = 1.0f;
                    onClickEmitted (ClickEvent { index, qpcMs });
                }
            }

            break;
        }
    }
}

void FakeAudioDevice::renderBlock (juce::AudioIODeviceCallback* cb) noexcept
{
    const double qpcMs = juce::Time::getMillisecondCounterHiRes();
    fillInputs (qpcMs);

    const float* inputs[2] = { inL.data(), inR.data() };
    float* outputs[2] = { outL.data(), outR.data() };
    const float* activeIn[2] = { nullptr, nullptr };
    float* activeOut[2] = { nullptr, nullptr };
    int numIn = 0, numOut = 0;

    for (int c = 0; c < 2; ++c)
    {
        if (activeInputs[c])  activeIn[numIn++] = inputs[c];
        if (activeOutputs[c]) activeOut[numOut++] = outputs[c];
    }

    juce::FloatVectorOperations::clear (outL.data(), blockSize);
    juce::FloatVectorOperations::clear (outR.data(), blockSize);

    if (cb != nullptr)
    {
        const juce::AudioIODeviceCallbackContext context;
        cb->audioDeviceIOCallbackWithContext (activeIn, numIn, activeOut, numOut, blockSize, context);
    }

    onOutputBlock (outL.data(), outR.data(), blockSize, sampleIndex, qpcMs);
    sampleIndex += blockSize;
}

//==============================================================================
FakeAudioDeviceType::FakeAudioDeviceType (int blockSizeToUse, TestSignal signalToUse)
    : juce::AudioIODeviceType (typeName()), blockSize (blockSizeToUse), signal (signalToUse)
{
}

void FakeAudioDeviceType::scanForDevices() {}
juce::StringArray FakeAudioDeviceType::getDeviceNames (bool) const        { return { deviceName() }; }
int FakeAudioDeviceType::getDefaultDeviceIndex (bool) const               { return 0; }
bool FakeAudioDeviceType::hasSeparateInputsAndOutputs() const             { return false; }

int FakeAudioDeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool) const
{
    return device != nullptr && device->getName() == deviceName() ? 0 : -1;
}

juce::AudioIODevice* FakeAudioDeviceType::createDevice (const juce::String& outputDeviceName, const juce::String& inputDeviceName)
{
    if (outputDeviceName == deviceName() || inputDeviceName == deviceName())
        return new FakeAudioDevice (blockSize, signal);

    return nullptr;
}

//==============================================================================
void ClickProbe::process (const float* L, const float* R, int n, int64_t firstSampleIndex, double qpcMs) noexcept
{
    for (int i = 0; i < n; ++i)
    {
        const float l = L != nullptr ? std::abs (L[i]) : 0.0f;
        const float r = R != nullptr ? std::abs (R[i]) : 0.0f;

        if (juce::jmax (l, r) > threshold)
        {
            if (quietRun >= quietSamples && onClickDetected != nullptr)
                onClickDetected (ClickEvent { firstSampleIndex + i, qpcMs });

            quietRun = 0;
        }
        else
        {
            ++quietRun;
        }
    }
}
}
