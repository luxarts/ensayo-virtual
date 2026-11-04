#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

namespace ensayo
{
// .vst3 files and bundles under root (recursive), without descending into bundles. Sorted by file name.
juce::Array<juce::File> findVst3Plugins (const juce::File& root);

// Hosts a single VST3 on the instrument. Swaps are guarded by a spin lock that the
// audio thread only try-locks, so loading never blocks audio (dry instrument meanwhile).
class PluginSlot
{
public:
    // Message thread. Loads the first effect found in the file. On failure the current plugin stays.
    juce::Result load (const juce::File& pluginFile, double sampleRate, int blockSize,
                       const juce::String& initialState = {});
    void unload();

    // Call while audio is stopped (device about to start / stopped).
    void prepare (double sampleRate, int blockSize);
    void release();

    // Audio thread. Channel 0 holds the mono instrument on entry; channels 0/1 hold stereo instrument on exit.
    void process (juce::AudioBuffer<float>& stereoInstrument, int numSamples) noexcept;

    void setBypassed (bool shouldBypass) noexcept     { bypassed.store (shouldBypass); }
    bool isBypassed() const noexcept                  { return bypassed.load(); }

    bool isLoaded() const noexcept                    { return instance != nullptr; }
    juce::AudioPluginInstance* getInstance() const noexcept { return instance.get(); }
    juce::File getFile() const                        { return file; }
    juce::String getName() const                      { return instance != nullptr ? instance->getName() : juce::String(); }

    juce::String getStateBase64() const;
    void setStateBase64 (const juce::String& state);

private:
    static void configureBuses (juce::AudioPluginInstance&);
    static int workChannelsFor (const juce::AudioPluginInstance&);
    static void applyStateBase64 (juce::AudioPluginInstance&, const juce::String& state);

    juce::VST3PluginFormat format;
    std::unique_ptr<juce::AudioPluginInstance> instance;
    juce::File file;
    juce::AudioBuffer<float> work;
    juce::MidiBuffer midi;
    juce::SpinLock lock;
    std::atomic<bool> bypassed { false };
};
}
