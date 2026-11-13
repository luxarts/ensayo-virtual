#include "Settings.h"
#include "core/Project.h"

namespace ensayo
{
namespace
{
float readFloat (const juce::var& v, const char* name, float fallback)
{
    return static_cast<float> (static_cast<double> (v.getProperty (name, fallback)));
}

int readInt (const juce::var& v, const char* name, int fallback)
{
    return static_cast<int> (v.getProperty (name, fallback));
}

bool readBool (const juce::var& v, const char* name, bool fallback)
{
    return static_cast<bool> (v.getProperty (name, fallback));
}

juce::String readString (const juce::var& v, const char* name, const juce::String& fallback)
{
    return v.getProperty (name, fallback).toString();
}
}

juce::String calibrationKey (const juce::String& deviceName, int bufferSize)
{
    return deviceName + "@" + juce::String (bufferSize);
}

juce::String toJson (const AppSettings& s)
{
    auto* root = new juce::DynamicObject();
    root->setProperty ("name", s.name);
    root->setProperty ("userId", s.userId);
    root->setProperty ("role", roleToString (s.role));
    root->setProperty ("deviceType", s.deviceType);
    root->setProperty ("deviceName", s.deviceName);
    root->setProperty ("sampleRate", s.sampleRate);
    root->setProperty ("bufferSize", s.bufferSize);
    root->setProperty ("micChannel", s.micChannel);
    root->setProperty ("instrumentChannel", s.instrumentChannel);
    root->setProperty ("outputPair", s.outputPair);
    root->setProperty ("pluginFile", s.pluginFile);
    root->setProperty ("pluginState", s.pluginState);
    root->setProperty ("pluginBypassed", s.pluginBypassed);
    root->setProperty ("monitorVoiceDb", s.monitorVoiceDb);
    root->setProperty ("monitorInstrumentDb", s.monitorInstrumentDb);
    root->setProperty ("monitorVoiceMuted", s.monitorVoiceMuted);
    root->setProperty ("monitorInstrumentMuted", s.monitorInstrumentMuted);
    root->setProperty ("receiveDb", s.receiveDb);
    root->setProperty ("trackDb", s.trackDb);
    root->setProperty ("trackMuted", s.trackMuted);
    root->setProperty ("hostPort", s.hostPort);
    root->setProperty ("lastLink", s.lastLink);
    root->setProperty ("ytDlpPath", s.ytDlpPath);

    auto* calibration = new juce::DynamicObject();

    for (const auto& [key, samples] : s.calibration)
        calibration->setProperty (key, samples);

    root->setProperty ("calibration", juce::var (calibration));
    root->setProperty ("lastProject", s.lastProject);
    return juce::JSON::toString (juce::var (root));
}

AppSettings fromJson (const juce::String& json)
{
    AppSettings s;
    juce::var root;

    if (juce::JSON::parse (json, root).failed() || ! root.isObject())
        return s;

    s.name = readString (root, "name", s.name);
    s.userId = readString (root, "userId", s.userId);

    if (! isValidId (s.userId))
        s.userId.clear();

    s.role = roleFromString (readString (root, "role", roleToString (s.role))).value_or (Role::both);
    s.deviceType = readString (root, "deviceType", s.deviceType);
    s.deviceName = readString (root, "deviceName", s.deviceName);
    s.sampleRate = static_cast<double> (root.getProperty ("sampleRate", s.sampleRate));
    s.bufferSize = readInt (root, "bufferSize", s.bufferSize);
    s.micChannel = readInt (root, "micChannel", s.micChannel);
    s.instrumentChannel = readInt (root, "instrumentChannel", s.instrumentChannel);
    s.outputPair = readInt (root, "outputPair", s.outputPair);
    s.pluginFile = readString (root, "pluginFile", s.pluginFile);
    s.pluginState = readString (root, "pluginState", s.pluginState);
    s.pluginBypassed = readBool (root, "pluginBypassed", s.pluginBypassed);
    s.monitorVoiceDb = readFloat (root, "monitorVoiceDb", s.monitorVoiceDb);
    s.monitorInstrumentDb = readFloat (root, "monitorInstrumentDb", s.monitorInstrumentDb);
    s.monitorVoiceMuted = readBool (root, "monitorVoiceMuted", s.monitorVoiceMuted);
    s.monitorInstrumentMuted = readBool (root, "monitorInstrumentMuted", s.monitorInstrumentMuted);
    s.receiveDb = readFloat (root, "receiveDb", s.receiveDb);
    s.trackDb = readFloat (root, "trackDb", s.trackDb);
    s.trackMuted = readBool (root, "trackMuted", s.trackMuted);
    s.hostPort = readInt (root, "hostPort", s.hostPort);
    s.lastLink = readString (root, "lastLink", s.lastLink);
    s.ytDlpPath = readString (root, "ytDlpPath", s.ytDlpPath);
    s.lastProject = readString (root, "lastProject", s.lastProject);

    if (auto* calibration = root["calibration"].getDynamicObject())
        for (const auto& property : calibration->getProperties())
            if (property.value.isInt() || property.value.isInt64() || property.value.isDouble())
                s.calibration[property.name.toString()] = (int) property.value;

    return s;
}

juce::File defaultSettingsFile()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
               .getChildFile ("EnsayoVirtual")
               .getChildFile ("settings.json");
}

AppSettings loadSettings (const juce::File& file)
{
    auto s = file.existsAsFile() ? fromJson (file.loadFileAsString()) : AppSettings();

    if (s.userId.isEmpty())
    {
        s.userId = newId();
        saveSettings (s, file);   // best effort: the id must survive the next load
    }

    return s;
}

bool saveSettings (const AppSettings& settings, const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    return file.replaceWithText (toJson (settings));
}
}
