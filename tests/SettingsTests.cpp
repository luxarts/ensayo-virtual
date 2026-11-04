#include "client/Settings.h"

namespace ensayo
{
class SettingsTests : public juce::UnitTest
{
public:
    SettingsTests() : juce::UnitTest ("Settings", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("save then load keeps every field");
        {
            AppSettings s;
            s.name = "Lucas";
            s.role = Role::instrument;
            s.deviceType = "Fake";
            s.deviceName = "MiniFuse ASIO Driver";
            s.sampleRate = 48000.0;
            s.bufferSize = 64;
            s.micChannel = 2;
            s.instrumentChannel = 3;
            s.outputPair = 1;
            s.pluginFile = "C:\\Program Files\\Common Files\\VST3\\NeuralAmpModeler.vst3";
            s.pluginState = "AAECAwQ=";
            s.pluginBypassed = true;
            s.monitorVoiceDb = -6.0f;
            s.monitorInstrumentDb = 3.5f;
            s.monitorVoiceMuted = true;
            s.monitorInstrumentMuted = false;
            s.receiveDb = -2.0f;
            s.trackDb = -12.0f;
            s.trackMuted = true;
            s.hostPort = 22150;
            s.lastLink = "ensayo://192.168.1.10:22140/ABC234";
            s.ytDlpPath = "C:\\tools\\yt-dlp.exe";

            const auto file = juce::File::createTempFile (".json");
            expect (saveSettings (s, file));
            expectSame (loadSettings (file), s);
            file.deleteFile();
        }

        beginTest ("missing file gives defaults");
        expectSame (loadSettings (juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ensayo-does-not-exist.json")),
                    AppSettings());

        beginTest ("invalid JSON gives defaults");
        expectSame (fromJson ("{ not json"), AppSettings());
        expectSame (fromJson ("[1, 2, 3]"), AppSettings());

        beginTest ("missing fields keep their defaults");
        {
            const auto s = fromJson (R"({ "micChannel": 3, "monitorInstrumentDb": -3.0 })");
            expectEquals (s.micChannel, 3);
            expectEquals (s.instrumentChannel, 1);
            expectEquals (s.monitorInstrumentDb, -3.0f);
            expectEquals (s.monitorVoiceDb, 0.0f);
            expectEquals (s.hostPort, defaultPort);
            expectEquals (s.deviceType, juce::String ("ASIO"));
            expect (s.role == Role::both);
        }

        beginTest ("unknown role falls back to both");
        {
            expect (fromJson (R"({ "role": "drums" })").role == Role::both);
            expect (fromJson (R"({ "role": "voice" })").role == Role::voice);
        }

        beginTest ("default file lives in AppData under EnsayoVirtual");
        expect (defaultSettingsFile().getFullPathName().endsWith ("EnsayoVirtual\\settings.json"));
    }

private:
    void expectSame (const AppSettings& a, const AppSettings& b)
    {
        expectEquals (a.name, b.name);
        expect (a.role == b.role);
        expectEquals (a.deviceType, b.deviceType);
        expectEquals (a.deviceName, b.deviceName);
        expectEquals (a.sampleRate, b.sampleRate);
        expectEquals (a.bufferSize, b.bufferSize);
        expectEquals (a.micChannel, b.micChannel);
        expectEquals (a.instrumentChannel, b.instrumentChannel);
        expectEquals (a.outputPair, b.outputPair);
        expectEquals (a.pluginFile, b.pluginFile);
        expectEquals (a.pluginState, b.pluginState);
        expect (a.pluginBypassed == b.pluginBypassed);
        expectEquals (a.monitorVoiceDb, b.monitorVoiceDb);
        expectEquals (a.monitorInstrumentDb, b.monitorInstrumentDb);
        expect (a.monitorVoiceMuted == b.monitorVoiceMuted);
        expect (a.monitorInstrumentMuted == b.monitorInstrumentMuted);
        expectEquals (a.receiveDb, b.receiveDb);
        expectEquals (a.trackDb, b.trackDb);
        expect (a.trackMuted == b.trackMuted);
        expectEquals (a.hostPort, b.hostPort);
        expectEquals (a.lastLink, b.lastLink);
        expectEquals (a.ytDlpPath, b.ytDlpPath);
    }
};

static SettingsTests settingsTests;
}
