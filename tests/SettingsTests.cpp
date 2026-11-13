#include "client/Settings.h"
#include "core/Project.h"

#ifndef NOMINMAX
 #define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

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
            s.userId = newId();
            s.calibration = { { calibrationKey ("MiniFuse ASIO Driver", 256), 37 }, { calibrationKey ("Fake Audio", 64), -12 } };
            s.lastProject = "C:\\projects\\abc";

            const auto file = juce::File::createTempFile (".json");
            expect (saveSettings (s, file));
            expectSame (loadSettings (file), s);
            file.deleteFile();
        }

        beginTest ("missing file gives defaults plus a new user id, saved at once");
        {
            const auto file = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ensayo-does-not-exist.json");
            file.deleteFile();
            auto loaded = loadSettings (file);
            expect (isValidId (loaded.userId), "no user id: " + loaded.userId);
            expect (file.existsAsFile(), "the new user id was not written back");
            expectEquals (loadSettings (file).userId, loaded.userId);
            loaded.userId.clear();
            expectSame (loaded, AppSettings());
            file.deleteFile();
        }

        beginTest ("userId is generated once and stable across loads; an old file keeps its other fields");
        {
            const auto file = juce::File::createTempFile (".json");
            file.replaceWithText (R"({ "name": "Ana", "bufferSize": 128, "hostPort": 22150 })");   // pre-userId settings file
            const auto first = loadSettings (file);
            expect (isValidId (first.userId));
            expectEquals (first.name, juce::String ("Ana"));
            expectEquals (first.bufferSize, 128);
            expectEquals (first.hostPort, 22150);
            expect (first.calibration.empty() && first.lastProject.isEmpty());

            for (int i = 0; i < 3; ++i)
                expectEquals (loadSettings (file).userId, first.userId);

            auto changed = first;
            changed.name = "Ana B";
            expect (saveSettings (changed, file));
            expectEquals (loadSettings (file).userId, first.userId);
            file.deleteFile();
        }

        beginTest ("a settings file that does not parse is kept as .bad, never overwritten silently");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ensayo-settings-bad");
            dir.deleteRecursively();
            dir.createDirectory();
            const auto file = dir.getChildFile ("settings.json");
            file.replaceWithText ("{ \"name\": \"Ana\", truncated");
            juce::String warning;
            const auto loaded = loadSettings (file, &warning);
            expect (isValidId (loaded.userId));
            expectEquals (loaded.name, juce::String());
            expectEquals (dir.getChildFile ("settings.json.bad").loadFileAsString(), juce::String ("{ \"name\": \"Ana\", truncated"));
            expect (warning.contains ("settings.json.bad"), warning);
            expect (fromJson (file.loadFileAsString()).userId == loaded.userId, "new defaults written with the id");

            juce::String none;
            loadSettings (file, &none);
            expect (none.isEmpty(), "a valid file gives no warning");

            // An older .bad is never deleted: the next one goes to .bad.1.
            file.replaceWithText ("second broken file");
            loadSettings (file);
            expectEquals (dir.getChildFile ("settings.json.bad").loadFileAsString(), juce::String ("{ \"name\": \"Ana\", truncated"));
            expectEquals (dir.getChildFile ("settings.json.bad.1").loadFileAsString(), juce::String ("second broken file"));

            // A corrupt file that cannot be moved aside (held open) is left untouched; defaults stay in memory.
            file.replaceWithText ("third broken file");
            bool keepFile = false;
            juce::String warning3;
            {
                // Held open without FILE_SHARE_DELETE: it can be read but not renamed.
                const auto handle = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                expect (handle != INVALID_HANDLE_VALUE);
                const auto inMemory = loadSettings (file, &warning3, &keepFile);
                expect (isValidId (inMemory.userId));
                CloseHandle (handle);
            }

            expect (keepFile, "a corrupt file that cannot be moved must be kept");
            expectEquals (file.loadFileAsString(), juce::String ("third broken file"));
            expect (! dir.getChildFile ("settings.json.bad.2").exists());
            expect (warning3.contains ("no se guardar"), warning3);

            dir.deleteRecursively();
        }

        beginTest ("calibration map round trip and key format");
        {
            expectEquals (calibrationKey ("MiniFuse ASIO Driver", 256), juce::String ("MiniFuse ASIO Driver@256"));
            AppSettings s;
            s.userId = newId();
            s.calibration[calibrationKey ("A", 64)] = 0;
            s.calibration[calibrationKey ("A", 256)] = 41;
            s.calibration[calibrationKey ("B@x", 128)] = -7;
            const auto back = fromJson (toJson (s));
            expect (back.calibration == s.calibration);
            expect (fromJson (R"({ "calibration": { "A@64": "bad", "A@128": 5 } })").calibration
                    == std::map<juce::String, int> { { "A@128", 5 } });
        }

        beginTest ("defaults: buffer 256, an invalid userId is dropped");
        {
            expectEquals (AppSettings().bufferSize, 256);
            expect (fromJson (R"({ "userId": "not-an-id" })").userId.isEmpty());
        }

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
        expectEquals (a.userId, b.userId);
        expect (a.calibration == b.calibration);
        expectEquals (a.lastProject, b.lastProject);
    }
};

static SettingsTests settingsTests;
}
