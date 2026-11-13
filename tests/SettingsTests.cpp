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
            s.sendVoiceDb = -4.5f;
            s.sendInstrumentDb = 2.0f;
            s.micMuted = true;
            s.participants["0123456789abcdef0123456789abcdef"] = { -12.5f, false };
            s.participants["fedcba9876543210fedcba9876543210"] = { 6.0f, true };
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

        beginTest ("a corrupt settings file keeps its userId when the id can still be read");
        {
            const auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ensayo-settings-recover");
            dir.deleteRecursively();
            dir.createDirectory();
            const auto file = dir.getChildFile ("settings.json");
            const Id id = newId();

            file.replaceWithText ("{\n  \"name\": \"Ana\",\n  \"userId\" :  \"" + id + "\",\n  \"bufferSize\": 2");   // cut short
            const auto recovered = loadSettings (file);
            expectEquals (recovered.userId, id);
            expectEquals (fromJson (file.loadFileAsString()).userId, id, "the recovered id is saved with the defaults");

            // Held open (cannot be moved aside): the id is still recovered in memory.
            file.replaceWithText ("garbage \"userId\":\"" + id + "\" garbage");
            {
                const auto handle = CreateFileW (file.getFullPathName().toWideCharPointer(), GENERIC_READ, FILE_SHARE_READ,
                                                 nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                expect (handle != INVALID_HANDLE_VALUE);
                bool keepFile = false;
                expectEquals (loadSettings (file, nullptr, &keepFile).userId, id);
                expect (keepFile);
                CloseHandle (handle);
            }

            // No readable id (wrong length, upper case): a new one.
            file.replaceWithText ("{ \"userId\": \"" + id.toUpperCase() + "\", broken");
            const auto fresh = loadSettings (file);
            expect (isValidId (fresh.userId) && fresh.userId != id);
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

        beginTest ("an old file without the voice chat fields loads, its monitor mutes become the fader minimum");
        {
            const auto s = fromJson (R"({ "monitorVoiceDb": -6.0, "monitorInstrumentDb": 3.0,
                                          "monitorVoiceMuted": true, "monitorInstrumentMuted": false })");
            expectEquals (s.monitorVoiceDb, minFaderDb);
            expectEquals (s.monitorInstrumentDb, 3.0f);
            expectEquals (s.sendVoiceDb, 0.0f);
            expectEquals (s.sendInstrumentDb, 0.0f);
            expect (! s.micMuted);
            expect (s.participants.empty());
            expect (! toJson (s).contains ("monitorVoiceMuted"), "the superseded mute fields are no longer written");
        }

        beginTest ("participant entries with a bad id or a non-object value are skipped, gains are clamped");
        {
            const auto s = fromJson (R"({ "participants": { "0123456789abcdef0123456789abcdef": { "gainDb": 40.0, "muted": true },
                                                            "not-an-id": { "gainDb": -3.0 },
                                                            "fedcba9876543210fedcba9876543210": 5,
                                                            "00000000000000000000000000000001": { "gainDb": -99.0 } },
                                          "sendVoiceDb": 20.0 })");
            expectEquals ((int) s.participants.size(), 2);
            expectEquals (s.participants.at ("0123456789abcdef0123456789abcdef").gainDb, maxFaderDb);
            expect (s.participants.at ("0123456789abcdef0123456789abcdef").muted);
            expectEquals (s.participants.at ("00000000000000000000000000000001").gainDb, minFaderDb);
            expectEquals (s.sendVoiceDb, 6.0f);
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
        expectEquals (a.sendVoiceDb, b.sendVoiceDb);
        expectEquals (a.sendInstrumentDb, b.sendInstrumentDb);
        expect (a.micMuted == b.micMuted);
        expectEquals ((int) a.participants.size(), (int) b.participants.size());

        for (const auto& [user, mix] : a.participants)
        {
            const auto it = b.participants.find (user);
            expect (it != b.participants.end(), "missing participant " + user);

            if (it != b.participants.end())
            {
                expectEquals (it->second.gainDb, mix.gainDb);
                expect (it->second.muted == mix.muted);
            }
        }
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
