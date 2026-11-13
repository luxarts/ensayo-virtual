#pragma once
#include "ui/LobbyView.h"
#include "ui/DawView.h"
#include "client/SessionController.h"
#include "client/Settings.h"
#include "client/StudioEngine.h"
#include "client/YouTubeImporter.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <memory>
#include <optional>
#include <set>
namespace ensayo
{
// Bottom bar: message + "Reintentar" in both views; in a session also sync state, ping, transfers, device and CPU.
class StatusBar : public juce::Component
{
public:
    StatusBar();
    void resized() override;
    void paint (juce::Graphics&) override;
    void setSessionMode (bool);
    void setMessage (const juce::String& text, bool isError, bool showRetry);
    void setSession (const juce::String& syncText, bool readOnly, int pingMs, int uploads, int downloads,
                     const juce::String& deviceText, double cpuPercent);

    std::function<void()> onRetry;
    juce::Label text, sync, ping, transfers, device, cpu;
    juce::TextButton retry;

private:
    bool sessionMode = false;
};

// Hosts a plugin's editor in its own native window.
class PluginWindow : public juce::DocumentWindow
{
public:
    PluginWindow (juce::AudioPluginInstance&, std::function<void()> onClose);
    void closeButtonPressed() override { if (closeCallback) closeCallback(); }

private:
    std::function<void()> closeCallback;
};

// The app: lobby (identity, audio device, calibration, VST, create/open/join) and the session (DawView). Owns the
// StudioEngine, which lives across sessions, and a SessionController per session (ProjectClient + embedded host).
// Every 50 ms the session state (replica, presence, live takes, downloads, transport) is mapped to a DawState.
class MainComponent : public juce::Component, private juce::Timer, private PeaksProvider
{
public:
    MainComponent();                                   // loads settings, opens the device, shows the lobby
    ~MainComponent() override;
    void showDemoSession();                            // fake 4-track project (for --demo / --screenshot)
    void joinFromLink (const SessionLink&);            // used by Main for ensayo:// arguments
    void hostProject (const juce::File& projectDir);   // Crear / Abrir: hosts the project and connects to it
    LobbyView& getLobby() noexcept                     { return lobby; }
    DawView& getDaw() noexcept                         { return daw; }
    StatusBar& getStatus() noexcept                    { return status; }
    SessionController& getSession() noexcept           { return session; }
    StudioEngine& getEngine() noexcept                 { return engine; }
    const AppSettings& getSettings() const noexcept    { return settings; }
    bool isInSession() const noexcept                  { return inSession; }
    bool isDemo() const noexcept                       { return demoMode; }
    void refreshSessionView();                         // builds the DawState and the status numbers now (timer does it too)

    // Tests and screenshots: no audio device is opened, no plugin is loaded, settings, projects and the cache go to
    // temp folders.
    static void setTestMode (bool);
    static bool isTestMode() noexcept;
    static juce::File projectsRoot();                  // defaultProjectsDirectory(), or a temp folder in test mode
    static juce::File cacheRoot();                     // defaultCacheRoot(), or a temp folder in test mode

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;
    std::vector<Peak> peaksFor (const Id& take) const override;

    // Lobby.
    void wireLobby();
    void applyLobbyToSettings();
    void openAudioFromSettings();
    void refreshDeviceCombos();
    void applyAudioSelection();
    void applyCalibrationFromSettings();
    void startCalibration();
    void pollCalibration();
    void scanPlugins();
    void loadPlugin (const juce::File&);
    void removePlugin();
    void togglePluginEditor();
    void updatePluginButtons();
    void applyMonitorFromSettings();
    void chooseProjectToOpen();
    void createProject();
    void joinFromLinkField();
    void setConnecting (bool);

    // Session.
    void wireSession();
    void wireDaw();
    void leaveSession();
    void abandonConnection (const juce::String& message);
    void showLobby();
    void showSession();
    void recordOrStop();
    void stopTake();
    void addTrack (TrackKind);
    void chooseImportFile();
    void downloadFromYouTube (const juce::String& url);
    void chooseExportFile();
    void retry();
    DawState buildState() const;
    void updateSessionStatus();

    // Demo.
    void demoTick();

    void markSettingsDirty();
    void saveSettingsNow();
    void setStatus (const juce::String&, bool isError = false, bool showRetry = false);
    SessionIdentity identity() const;
    juce::String deviceKey() const;

    AppSettings settings;
    juce::File settingsFile;
    // Declaration order matters: the session (client, host, recorder callbacks) must go before the engine.
    StudioEngine engine;
    SessionController session { engine };
    YouTubeImporter importer;

    LobbyView lobby;
    DawView daw;
    StatusBar status;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<PluginWindow> pluginWindow;
    juce::Array<juce::File> vst3Files;

    bool inSession = false, demoMode = false, deviceOpen = false, connecting = false, calibrating = false;
    bool settingsDirty = false;
    juce::uint32 settingsDirtyAtMs = 0;

    // What Reintentar repeats in the lobby.
    enum class LastAction { none, host, join };
    LastAction lastAction = LastAction::none;
    juce::File lastProjectDir;
    SessionLink lastJoinLink;

    // Local session state (not part of the project).
    Id armedTrack;
    std::set<Id> solo;
    bool click = true, countIn = true, listen = false;
    juce::String lastSyncNote;

    // Demo state.
    std::shared_ptr<Project> demoProject;
    DawState demoState;
    juce::uint32 demoLastMs = 0;

    static bool testMode;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
}
