#include "client/FakeAudioDevice.h"

#include <atomic>
#include <vector>

namespace ensayo
{
class FakeAudioDeviceTests : public juce::UnitTest
{
public:
    FakeAudioDeviceTests() : juce::UnitTest ("FakeAudioDevice", "EnsayoVirtual") {}

    struct CountingCallback : public juce::AudioIODeviceCallback
    {
        std::atomic<int> callbacks { 0 };
        std::atomic<int> lastNumInputs { 0 }, lastNumOutputs { 0 }, lastNumSamples { 0 };
        std::atomic<float> inputPeak { 0.0f };

        void audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                               float* const* outputs, int numOutputs, int numSamples,
                                               const juce::AudioIODeviceCallbackContext&) override
        {
            callbacks.fetch_add (1);
            lastNumInputs.store (numInputs);
            lastNumOutputs.store (numOutputs);
            lastNumSamples.store (numSamples);

            float peak = inputPeak.load();

            for (int c = 0; c < numInputs; ++c)
                for (int i = 0; i < numSamples; ++i)
                    peak = juce::jmax (peak, std::abs (inputs[c][i]));

            inputPeak.store (peak);

            for (int c = 0; c < numOutputs; ++c)
                for (int i = 0; i < numSamples; ++i)
                    outputs[c][i] = 0.25f;   // something recognisable for onOutputBlock
        }

        void audioDeviceAboutToStart (juce::AudioIODevice*) override {}
        void audioDeviceStopped() override {}
    };

    void runTest() override
    {
        beginTest ("device reports its fixed capabilities");
        {
            FakeAudioDevice device (128, TestSignal::silence);
            expectEquals (device.getName(), juce::String ("Fake Audio"));
            expectEquals (device.getTypeName(), juce::String ("Fake"));
            expectEquals (device.getOutputChannelNames().size(), 2);
            expectEquals (device.getInputChannelNames().size(), 2);
            expectEquals (device.getInputChannelNames()[0], juce::String ("Fake In L"));
            expectEquals (device.getOutputChannelNames()[1], juce::String ("Fake Out R"));
            expect (device.getAvailableSampleRates().contains (48000.0));
            expect (device.getAvailableBufferSizes().contains (64));
            expect (device.getAvailableBufferSizes().contains (128));
            expect (device.getAvailableBufferSizes().contains (256));
            expectEquals (device.getDefaultBufferSize(), 128);
            expectEquals (device.getInputLatencyInSamples(), 0);
            expectEquals (device.getOutputLatencyInSamples(), 0);
            expect (! device.isOpen());
            expect (! device.isPlaying());
        }

        beginTest ("clicks every 250 ms at exact cadence for 600 ms");
        {
            FakeAudioDevice device (128, TestSignal::click);
            CountingCallback callback;

            std::atomic<int> clicks { 0 };
            std::atomic<int> outputBlocks { 0 };
            std::atomic<int64_t> firstClickSample { -1 }, secondClickSample { -1 };
            std::atomic<float> outputValue { 0.0f };
            std::atomic<int64_t> lastFirstSample { -1 };

            device.onClickEmitted = [&] (ClickEvent e)
            {
                const int n = clicks.fetch_add (1);

                if (n == 0) firstClickSample.store (e.sampleIndex);
                if (n == 1) secondClickSample.store (e.sampleIndex);
            };

            device.onOutputBlock = [&] (const float* L, const float*, int n, int64_t firstSampleIndex, double)
            {
                outputBlocks.fetch_add (1);
                outputValue.store (L[n - 1]);
                lastFirstSample.store (firstSampleIndex);
            };

            juce::BigInteger ins, outs;
            ins.setRange (0, 2, true);
            outs.setRange (0, 2, true);

            const auto error = device.open (ins, outs, 48000.0, 128);
            expectEquals (error, juce::String());
            expect (device.isOpen());
            expectEquals (device.getCurrentBufferSizeSamples(), 128);
            expectEquals (device.getCurrentSampleRate(), 48000.0);
            expectEquals (device.getActiveInputChannels().countNumberOfSetBits(), 2);
            expectEquals (device.getActiveOutputChannels().countNumberOfSetBits(), 2);

            device.start (&callback);
            expect (device.isPlaying());
            juce::Thread::sleep (600);
            device.stop();
            expect (! device.isPlaying());

            const int count = callback.callbacks.load();
            logMessage ("callbacks in 600 ms: " + juce::String (count) + ", clicks: " + juce::String (clicks.load()));
            expect (count >= 191 && count <= 259, "callback count out of range: " + juce::String (count));
            expect (clicks.load() >= 2, "expected at least two clicks");
            expectEquals (firstClickSample.load(), (int64_t) 0);
            expectEquals (secondClickSample.load(), (int64_t) 12000);
            expectEquals (callback.lastNumInputs.load(), 2);
            expectEquals (callback.lastNumOutputs.load(), 2);
            expectEquals (callback.lastNumSamples.load(), 128);
            expectEquals (callback.inputPeak.load(), 1.0f);
            expect (outputBlocks.load() == count, "onOutputBlock must be called once per callback");
            expectEquals (outputValue.load(), 0.25f);
            expectEquals (lastFirstSample.load(), (int64_t) (count - 1) * 128);

            device.close();
            expect (! device.isOpen());
        }

        beginTest ("sine signal feeds both inputs");
        {
            FakeAudioDevice device (64, TestSignal::sine440);
            CountingCallback callback;
            juce::BigInteger ins, outs;
            ins.setRange (0, 2, true);
            outs.setRange (0, 2, true);
            expectEquals (device.open (ins, outs, 48000.0, 64), juce::String());
            device.start (&callback);
            juce::Thread::sleep (100);
            device.stop();
            device.close();
            expect (callback.callbacks.load() > 30);
            expect (callback.inputPeak.load() > 0.4f && callback.inputPeak.load() <= 1.0f);
        }

        beginTest ("device type creates the device");
        {
            FakeAudioDeviceType type (128, TestSignal::silence);
            type.scanForDevices();
            expectEquals (type.getTypeName(), juce::String ("Fake"));
            expectEquals (type.getDeviceNames (false).size(), 1);
            expectEquals (type.getDeviceNames (false)[0], juce::String ("Fake Audio"));
            expectEquals (type.getDeviceNames (true)[0], juce::String ("Fake Audio"));
            expectEquals (type.getDefaultDeviceIndex (false), 0);
            expect (! type.hasSeparateInputsAndOutputs());

            std::unique_ptr<juce::AudioIODevice> created (type.createDevice ("Fake Audio", "Fake Audio"));
            expect (created != nullptr);
            expect (dynamic_cast<FakeAudioDevice*> (created.get()) != nullptr);
            expectEquals (type.getIndexOfDevice (created.get(), false), 0);
            expect (type.createDevice ("Other", "Other") == nullptr);
        }

        beginTest ("click probe detects a click once and ignores one 5 ms later");
        {
            ClickProbe probe;
            std::vector<ClickEvent> detected;
            probe.onClickDetected = [&] (ClickEvent e) { detected.push_back (e); };

            std::vector<float> L (128, 0.0f), R (128, 0.0f);

            // 30 ms of silence: 11.25 blocks of 128.
            for (int b = 0; b < 12; ++b)
                probe.process (L.data(), R.data(), 128, (int64_t) b * 128, 1000.0 + b * 2.6667);

            L[37] = 0.9f;
            probe.process (L.data(), R.data(), 128, 12 * 128, 1032.0);
            L[37] = 0.0f;

            expectEquals ((int) detected.size(), 1);
            expectEquals (detected[0].sampleIndex, (int64_t) (12 * 128 + 37));
            expectEquals (detected[0].qpcMs, 1032.0);

            // A second click 5 ms (240 samples) later must be ignored: 240 - (128 - 37) = 149 → block 14, index 21.
            probe.process (L.data(), R.data(), 128, 13 * 128, 1034.7);
            R[21] = 0.9f;
            probe.process (L.data(), R.data(), 128, 14 * 128, 1037.3);
            R[21] = 0.0f;
            expectEquals ((int) detected.size(), 1);

            // After 20 ms of quiet a new click is detected again.
            for (int b = 15; b < 25; ++b)
                probe.process (L.data(), R.data(), 128, (int64_t) b * 128, 1040.0 + b * 2.6667);

            R[5] = 0.7f;
            probe.process (L.data(), R.data(), 128, 25 * 128, 1110.0);
            expectEquals ((int) detected.size(), 2);
            expectEquals (detected[1].sampleIndex, (int64_t) (25 * 128 + 5));
        }

        beginTest ("click probe ignores small signals");
        {
            ClickProbe probe;
            int count = 0;
            probe.onClickDetected = [&] (ClickEvent) { ++count; };
            std::vector<float> L (128, 0.3f), R (128, -0.3f);

            for (int b = 0; b < 40; ++b)
                probe.process (L.data(), R.data(), 128, (int64_t) b * 128, b * 2.6667);

            expectEquals (count, 0);
        }
    }
};

static FakeAudioDeviceTests fakeAudioDeviceTests;
}
