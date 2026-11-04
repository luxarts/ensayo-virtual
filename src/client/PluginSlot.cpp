#include "PluginSlot.h"

namespace ensayo
{
namespace
{
struct ByFileName
{
    static int compareElements (const juce::File& a, const juce::File& b)
    {
        return a.getFileName().compareIgnoreCase (b.getFileName());
    }
};

bool isInsideBundle (const juce::File& f, const juce::File& root)
{
    for (auto parent = f.getParentDirectory(); parent.isAChildOf (root); parent = parent.getParentDirectory())
        if (parent.hasFileExtension ("vst3"))
            return true;

    return false;
}
}

juce::Array<juce::File> findVst3Plugins (const juce::File& root)
{
    juce::Array<juce::File> results;

    if (! root.isDirectory())
        return results;

    for (const auto& entry : juce::RangedDirectoryIterator (root, true, "*.vst3", juce::File::findFilesAndDirectories))
    {
        const auto f = entry.getFile();

        if (! isInsideBundle (f, root))
            results.add (f);
    }

    ByFileName comparator;
    results.sort (comparator);
    return results;
}

void PluginSlot::configureBuses (juce::AudioPluginInstance& plugin)
{
    auto layout = plugin.getBusesLayout();

    if (layout.inputBuses.isEmpty() || layout.outputBuses.isEmpty())
        return;

    layout.inputBuses.getReference (0) = juce::AudioChannelSet::mono();
    layout.outputBuses.getReference (0) = juce::AudioChannelSet::stereo();

    if (plugin.setBusesLayout (layout))
        return;

    layout.inputBuses.getReference (0) = juce::AudioChannelSet::stereo();
    plugin.setBusesLayout (layout);   // if this also fails the plugin keeps its default layout
}

int PluginSlot::workChannelsFor (const juce::AudioPluginInstance& plugin)
{
    return std::max ({ 2, plugin.getTotalNumInputChannels(), plugin.getTotalNumOutputChannels() });
}

void PluginSlot::applyStateBase64 (juce::AudioPluginInstance& plugin, const juce::String& state)
{
    if (state.isEmpty())
        return;

    juce::MemoryBlock block;

    if (block.fromBase64Encoding (state))
        plugin.setStateInformation (block.getData(), (int) block.getSize());
}

juce::Result PluginSlot::load (const juce::File& pluginFile, double sampleRate, int blockSize,
                               const juce::String& initialState)
{
    if (sampleRate <= 0.0) sampleRate = 48000.0;
    if (blockSize <= 0)    blockSize = 512;

    juce::OwnedArray<juce::PluginDescription> types;

    if (pluginFile.exists())
        format.findAllTypesForFile (types, pluginFile.getFullPathName());

    if (types.isEmpty())
        return juce::Result::fail ("No VST3 plugin in " + pluginFile.getFileName());

    const juce::PluginDescription* chosen = types.getFirst();

    for (auto* description : types)
    {
        if (! description->isInstrument)
        {
            chosen = description;
            break;
        }
    }

    juce::String error;
    auto created = format.createInstanceFromDescription (*chosen, sampleRate, blockSize, error);

    if (created == nullptr)
        return juce::Result::fail (error.isNotEmpty() ? error : juce::String ("Could not create the plugin"));

    configureBuses (*created);
    applyStateBase64 (*created, initialState);
    created->prepareToPlay (sampleRate, blockSize);
    juce::AudioBuffer<float> newWork (workChannelsFor (*created), blockSize);

    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::swap (instance, created);
        std::swap (work, newWork);
        midi.ensureSize (2048);
        file = pluginFile;
    }

    // `created` now holds the previous plugin (if any); it is destroyed here, on the message thread.
    if (created != nullptr)
        created->releaseResources();

    return juce::Result::ok();
}

void PluginSlot::unload()
{
    std::unique_ptr<juce::AudioPluginInstance> old;

    {
        const juce::SpinLock::ScopedLockType sl (lock);
        std::swap (old, instance);
        file = juce::File();
    }

    if (old != nullptr)
        old->releaseResources();
}

void PluginSlot::prepare (double sampleRate, int blockSize)
{
    const juce::SpinLock::ScopedLockType sl (lock);
    midi.ensureSize (2048);

    if (instance != nullptr)
    {
        instance->prepareToPlay (sampleRate, blockSize);
        work.setSize (workChannelsFor (*instance), blockSize);
    }
    else
    {
        work.setSize (2, blockSize);
    }
}

void PluginSlot::release()
{
    const juce::SpinLock::ScopedLockType sl (lock);

    if (instance != nullptr)
        instance->releaseResources();
}

void PluginSlot::process (juce::AudioBuffer<float>& stereoInstrument, int numSamples) noexcept
{
    stereoInstrument.copyFrom (1, 0, stereoInstrument, 0, 0, numSamples);

    if (bypassed.load())
        return;

    const juce::SpinLock::ScopedTryLockType tryLock (lock);

    if (! tryLock.isLocked() || instance == nullptr || numSamples > work.getNumSamples())
        return;

    if (std::max (instance->getTotalNumInputChannels(), instance->getTotalNumOutputChannels()) > work.getNumChannels())
        return;

    const int mainInputs = instance->getMainBusNumInputChannels();
    const int outputs = instance->getTotalNumOutputChannels();

    for (int channel = 0; channel < work.getNumChannels(); ++channel)
    {
        if (channel < mainInputs)
            work.copyFrom (channel, 0, stereoInstrument, 0, 0, numSamples);
        else
            work.clear (channel, 0, numSamples);
    }

    juce::AudioBuffer<float> block (work.getArrayOfWritePointers(), work.getNumChannels(), numSamples);
    midi.clear();
    instance->processBlock (block, midi);

    if (outputs >= 2)
    {
        stereoInstrument.copyFrom (0, 0, block, 0, 0, numSamples);
        stereoInstrument.copyFrom (1, 0, block, 1, 0, numSamples);
    }
    else if (outputs == 1)
    {
        stereoInstrument.copyFrom (0, 0, block, 0, 0, numSamples);
        stereoInstrument.copyFrom (1, 0, block, 0, 0, numSamples);
    }
}

juce::String PluginSlot::getStateBase64() const
{
    if (instance == nullptr)
        return {};

    juce::MemoryBlock state;
    instance->getStateInformation (state);
    return state.toBase64Encoding();
}

void PluginSlot::setStateBase64 (const juce::String& state)
{
    if (instance != nullptr)
        applyStateBase64 (*instance, state);
}
}
