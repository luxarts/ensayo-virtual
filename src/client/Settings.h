#pragma once
#include "core/Protocol.h"
namespace ensayo
{
struct AppSettings
{
    juce::String name; Role role = Role::both;
    juce::String deviceType = "ASIO";   // "ASIO" | "Fake"
    juce::String deviceName; double sampleRate = 48000.0; int bufferSize = 128;
    int micChannel = 0, instrumentChannel = 1, outputPair = 0;
    juce::String pluginFile, pluginState; bool pluginBypassed = false;
    float monitorVoiceDb = 0, monitorInstrumentDb = 0; bool monitorVoiceMuted = false, monitorInstrumentMuted = false;
    float receiveDb = 0; float trackDb = 0; bool trackMuted = false;
    int hostPort = defaultPort; juce::String lastLink; juce::String ytDlpPath;
};
juce::String toJson (const AppSettings&);  AppSettings fromJson (const juce::String&);   // invalid JSON → defaults
juce::File defaultSettingsFile();           // %APPDATA%\EnsayoVirtual\settings.json
AppSettings loadSettings (const juce::File&); bool saveSettings (const AppSettings&, const juce::File&);
}
