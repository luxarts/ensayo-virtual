#pragma once
#include "core/Protocol.h"
#include <map>
namespace ensayo
{
// Personal level of another participant's voice chat and live instrument (not part of the project).
struct ParticipantSetting { float gainDb = 0.0f; bool muted = false; };

struct AppSettings
{
    juce::String name; Role role = Role::both;
    juce::String userId;                 // this installation's user id (newId()), created by loadSettings when missing
    juce::String deviceType = "ASIO";   // "ASIO" | "Fake"
    juce::String deviceName; double sampleRate = 48000.0; int bufferSize = 256;
    int micChannel = 0, instrumentChannel = 1, outputPair = 0;
    juce::String pluginFile, pluginState; bool pluginBypassed = false;
    // "Mi audio": monitor and send levels of my own inputs, and the voice chat mute. A file from before these fields
    // loads with its monitorVoiceMuted / monitorInstrumentMuted mapped to the fader minimum (silence).
    float monitorVoiceDb = 0, monitorInstrumentDb = 0, sendVoiceDb = 0, sendInstrumentDb = 0; bool micMuted = false;
    std::map<juce::String, ParticipantSetting> participants;   // userId → personal level ("Participantes")
    float receiveDb = 0; float trackDb = 0; bool trackMuted = false;
    int hostPort = defaultPort; juce::String lastLink; juce::String ytDlpPath;
    std::map<juce::String, int> calibration;   // "<device>@<buffer>" → samples measured by "Calibrar latencia"
    juce::String lastProject;                  // directory of the last project hosted
};
juce::String calibrationKey (const juce::String& deviceName, int bufferSize);   // "<device>@<buffer>"
juce::String toJson (const AppSettings&);  AppSettings fromJson (const juce::String&);   // invalid JSON → defaults
juce::File defaultSettingsFile();           // %APPDATA%\EnsayoVirtual\settings.json
// Missing file or keys → defaults. A file without a valid userId gets a new one, written back to the file at once so
// the id is stable across loads. A file that is not valid JSON is moved to <name>.bad (or .bad.N) first and `warning`
// receives a Spanish message for the user. If it cannot be moved, it is left untouched, the defaults live in memory
// only and *keepFile is set: the caller must not save over it.
AppSettings loadSettings (const juce::File&, juce::String* warning = nullptr, bool* keepFile = nullptr);
bool saveSettings (const AppSettings&, const juce::File&);
}
