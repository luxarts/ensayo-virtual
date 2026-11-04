#include "AudioEngine.h"

namespace ensayo
{
namespace
{
constexpr int maxChannels = 64;

void storeMax (std::atomic<float>& target, float value) noexcept
{
    float current = target.load (std::memory_order_relaxed);

    while (value > current && ! target.compare_exchange_weak (current, value)) {}
}
}

//==============================================================================
void AsioDeviceManager::createAudioDeviceTypes (juce::OwnedArray<juce::AudioIODeviceType>& types)
{
    if (auto* asio = juce::AudioIODeviceType::createAudioIODeviceType_ASIO())
        types.add (asio);
}

//==============================================================================
AudioEngine::AudioEngine()
{
    deviceManager.addAudioCallback (this);
}

AudioEngine::~AudioEngine()
{
    deviceManager.removeAudioCallback (this);
    deviceManager.closeAudioDevice();
}

juce::AudioDeviceManager& AudioEngine::getDeviceManager() noexcept   { return deviceManager; }
PluginSlot& AudioEngine::getPluginSlot() noexcept                    { return plugin; }
juce::AudioIODevice* AudioEngine::getDevice() const noexcept         { return deviceManager.getCurrentAudioDevice(); }

FakeAudioDevice* AudioEngine::getFakeDevice() const noexcept
{
    return dynamic_cast<FakeAudioDevice*> (deviceManager.getCurrentAudioDevice());
}

juce::StringArray AudioEngine::getAsioDeviceNames()
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

void AudioEngine::useFakeDevice (int blockSize, TestSignal signal)
{
    deviceManager.closeAudioDevice();

    // Make sure the built-in (ASIO) types exist first: the manager only creates them while its list is empty.
    const auto& types = deviceManager.getAvailableDeviceTypes();

    for (auto* type : types)
    {
        if (type->getTypeName() == FakeAudioDeviceType::typeName())
        {
            deviceManager.removeAudioDeviceType (type);
            break;
        }
    }

    deviceManager.addAudioDeviceType (std::make_unique<FakeAudioDeviceType> (blockSize, signal));

    const auto error = openDevice (FakeAudioDeviceType::typeName(), FakeAudioDeviceType::deviceName(), (double) sampleRate, blockSize);

    if (error.isNotEmpty())
        audioDeviceError (error);
}

juce::String AudioEngine::openDevice (const juce::String& deviceType, const juce::String& name, double sampleRateToUse, int bufferSize)
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
    setup.sampleRate = sampleRateToUse;
    setup.bufferSize = bufferSize;
    setup.useDefaultInputChannels = false;
    setup.useDefaultOutputChannels = false;
    setup.inputChannels.setRange (0, maxChannels, true);
    setup.outputChannels.setRange (0, maxChannels, true);

    return deviceManager.setAudioDeviceSetup (setup, true);
}

//==============================================================================
void AudioEngine::setChannels (int mic, int instrument, int pair) noexcept
{
    micChannel.store (mic);
    instrumentChannel.store (instrument);
    outputPair.store (pair);
}

void AudioEngine::setRole (Role r) noexcept                { role.store (r); }
void AudioEngine::setReceiveGain (float db) noexcept       { receiveGain.store (dbToGain (db)); }
void AudioEngine::setTransport (AudioTransport* t) noexcept { transport.store (t); }
void AudioEngine::setFrameHook (FrameHook hook)            { frameHook = std::move (hook); }
void AudioEngine::setSimulatedLossPercent (int p) noexcept { lossPercent.store (juce::jlimit (0, 100, p)); }

void AudioEngine::setMonitor (const ChannelGain& gain) noexcept
{
    monitorVoiceGain.store (gain.voiceGain());
    monitorInstrumentGain.store (gain.instrumentGain());
}

EnginePeaks AudioEngine::takePeaks() noexcept
{
    EnginePeaks peaks;
    peaks.voice = peakVoice.exchange (0.0f);
    peaks.instrument = peakInstrument.exchange (0.0f);
    peaks.received = peakReceived.exchange (0.0f);
    peaks.output = peakOutput.exchange (0.0f);
    return peaks;
}

uint32_t AudioEngine::getCallbackCount() const noexcept    { return callbackCount.load (std::memory_order_relaxed); }
FrameSize AudioEngine::getFrameSize() const noexcept       { return frameSize.load(); }

juce::String AudioEngine::takeDeviceError()
{
    const juce::ScopedLock sl (errorLock);
    return std::exchange (lastError, {});
}

//==============================================================================
void AudioEngine::audioDeviceAboutToStart (juce::AudioIODevice* device)
{
    const int blockSize = device->getCurrentBufferSizeSamples();
    const double rate = device->getCurrentSampleRate();

    FrameSize size = FrameSize::f128;
    bool supported = (int) std::lround (rate) == sampleRate;

    if (blockSize == 64)
        size = FrameSize::f64;
    else if (blockSize != 128 && blockSize != 256)
        supported = false;

    if (! supported)
        audioDeviceError ("El dispositivo debe correr a 48000 Hz con bloques de 64, 128 o 256 muestras (actual: "
                          + juce::String (rate, 0) + " Hz, " + juce::String (blockSize) + ")");

    const auto& mode = size == FrameSize::f64 ? mode64 : mode128;
    frameLength = frameSamples (size);
    voiceEncoder = std::make_unique<OpusEncoder> (mode, 1);
    instrumentEncoder = std::make_unique<OpusEncoder> (mode, 1);
    mixDecoder = std::make_unique<OpusDecoder> (mode, 2);

    instrumentBuffer.setSize (2, juce::jmax (blockSize, 1));
    instrumentBuffer.clear();
    silence.assign ((size_t) juce::jmax (blockSize, frameLength), 0.0f);
    monoInstrument.assign ((size_t) frameLength, 0.0f);
    decodedInterleaved.assign ((size_t) frameLength * 2, 0.0f);
    decodedL.assign ((size_t) frameLength, 0.0f);
    decodedR.assign ((size_t) frameLength, 0.0f);

    plugin.prepare (rate, blockSize);
    seq = 0;
    sampleTime = 0;
    decoderPrimed = false;
    consecutiveLost = 0;
    frameSize.store (size);
    blockSupported.store (supported);
}

void AudioEngine::audioDeviceStopped()
{
    blockSupported.store (false);
    plugin.release();
}

void AudioEngine::audioDeviceError (const juce::String& message)
{
    const juce::ScopedLock sl (errorLock);
    lastError = message;
}

void AudioEngine::audioDeviceIOCallbackWithContext (const float* const* inputs, int numInputs,
                                                    float* const* outputs, int numOutputs, int numSamples,
                                                    const juce::AudioIODeviceCallbackContext&)
{
    callbackCount.fetch_add (1, std::memory_order_relaxed);

    for (int channel = 0; channel < numOutputs; ++channel)
        if (outputs[channel] != nullptr)
            juce::FloatVectorOperations::clear (outputs[channel], numSamples);

    if (! blockSupported.load (std::memory_order_relaxed)
         || numSamples > instrumentBuffer.getNumSamples()
         || numSamples % frameLength != 0)
        return;

    const auto inputAt = [&] (int channel) -> const float*
    {
        return juce::isPositiveAndBelow (channel, numInputs) ? inputs[channel] : nullptr;
    };

    const float* voice = inputAt (micChannel.load (std::memory_order_relaxed));
    const float* instrument = inputAt (instrumentChannel.load (std::memory_order_relaxed));

    if (instrument != nullptr)
        instrumentBuffer.copyFrom (0, 0, instrument, numSamples);
    else
        instrumentBuffer.clear (0, 0, numSamples);

    plugin.process (instrumentBuffer, numSamples);

    const float* instrumentL = instrumentBuffer.getReadPointer (0);
    const float* instrumentR = instrumentBuffer.getReadPointer (1);

    const int pair = outputPair.load (std::memory_order_relaxed);
    float* outL = nullptr;
    float* outR = nullptr;

    if (pair >= 0 && pair * 2 + 1 < numOutputs)
    {
        outL = outputs[pair * 2];
        outR = outputs[pair * 2 + 1];
    }

    if (voice == nullptr)
        voice = silence.data();

    storeMax (peakVoice, peakOf (voice, numSamples));
    storeMax (peakInstrument, juce::jmax (peakOf (instrumentL, numSamples), peakOf (instrumentR, numSamples)));

    if (outL != nullptr && outR != nullptr)
    {
        addMonoCentred (outL, outR, voice, monitorVoiceGain.load (std::memory_order_relaxed), numSamples);
        addStereo (outL, outR, instrumentL, instrumentR, monitorInstrumentGain.load (std::memory_order_relaxed), numSamples);
    }

    for (int offset = 0; offset < numSamples; offset += frameLength)
        processFrame (voice + offset, instrumentL + offset, instrumentR + offset,
                      outL != nullptr ? outL + offset : nullptr,
                      outR != nullptr ? outR + offset : nullptr, frameLength);

    if (outL != nullptr && outR != nullptr)
        storeMax (peakOutput, juce::jmax (peakOf (outL, numSamples), peakOf (outR, numSamples)));
}

void AudioEngine::processFrame (const float* voice, const float* instrumentL, const float* instrumentR,
                                float* outL, float* outR, int frame) noexcept
{
    if (auto* t = transport.load (std::memory_order_acquire))
    {
        const Role r = role.load (std::memory_order_relaxed);
        AudioPacketView view;
        view.frameSize = frameSize.load (std::memory_order_relaxed);
        view.seq = seq;
        view.clientId = t->getClientId();
        view.sampleTime = sampleTime;
        view.trackPosition = t->getTrackPosition();

        if (roleSendsVoice (r))
        {
            const int n = voiceEncoder->encode (voice, voiceCoded.data(), (int) voiceCoded.size());

            if (n > 0)
            {
                view.flags |= AudioFlags::voice;
                view.voice = voiceCoded.data();
                view.voiceLen = n;
            }
        }

        if (roleSendsInstrument (r))
        {
            for (int i = 0; i < frame; ++i)
                monoInstrument[(size_t) i] = 0.5f * (instrumentL[i] + instrumentR[i]);

            const int n = instrumentEncoder->encode (monoInstrument.data(), instrumentCoded.data(), (int) instrumentCoded.size());

            if (n > 0)
            {
                view.flags |= AudioFlags::instrument;
                view.instrument = instrumentCoded.data();
                view.instrumentLen = n;
            }
        }

        if (view.flags != 0)
        {
            uint8_t packet[maxPacketBytes];
            const auto written = encodeAudioPacket (view, packet, sizeof (packet));

            if (written > 0)
                t->sendAudio (packet, written);
        }

        ++seq;

        // Receive: one block per frame, PLC when the jitter buffer has nothing (or the simulated loss drops it).
        int len = 0;
        bool have = t->receiveBuffer().pop (receivedPacket.data(), len);

        const int loss = lossPercent.load (std::memory_order_relaxed);

        if (have && loss > 0)
        {
            lossRandomState = lossRandomState * 1664525u + 1013904223u;

            if ((int) ((lossRandomState >> 8) % 100u) < loss)
                have = false;
        }

        // PLC only conceals gaps inside a stream: nothing before the first real block (a fresh CELT decoder
        // would emit comfort noise) and silence once a run of losses is long enough to be a dead stream.
        if (have)
        {
            mixDecoder->decode (receivedPacket.data(), len, decodedInterleaved.data());
            decoderPrimed = true;
            consecutiveLost = 0;
        }
        else if (decoderPrimed && consecutiveLost < maxConcealedFrames)
        {
            mixDecoder->decode (nullptr, 0, decodedInterleaved.data());
            ++consecutiveLost;
        }
        else
        {
            juce::FloatVectorOperations::clear (decodedInterleaved.data(), frame * 2);
        }

        deinterleave (decodedInterleaved.data(), decodedL.data(), decodedR.data(), frame);
        storeMax (peakReceived, juce::jmax (peakOf (decodedL.data(), frame), peakOf (decodedR.data(), frame)));

        if (outL != nullptr && outR != nullptr)
            addStereo (outL, outR, decodedL.data(), decodedR.data(), receiveGain.load (std::memory_order_relaxed), frame);
    }

    if (frameHook)
        frameHook();

    sampleTime += (uint32_t) frame;
}
}
