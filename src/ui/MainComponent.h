#pragma once
#include "ui/LobbyView.h"
#include "ui/SessionView.h"
#include "client/AudioEngine.h"
#include "client/SessionClient.h"
#include "client/Settings.h"
#include "client/YouTubeImporter.h"
#include "server/ServerHost.h"
#include <juce_audio_utils/juce_audio_utils.h>
#include <atomic>
#include <map>
#include <memory>
namespace ensayo
{
// Bottom bar shared by both views: message + "Reintentar", and the session numbers (hidden in the lobby).
class StatusBar : public juce::Component
{
public:
    StatusBar();
    void resized() override;
    void paint (juce::Graphics&) override;
    void setSessionMode (bool);
    void setMessage (const juce::String& text, bool isError, bool showRetry);
    void setNumbers (int pingMs, int clientBlocks, int serverBlocks, double latencyMs, double lossPercent, double cpuPercent);

    std::function<void()> onRetry;
    juce::Label text, ping, jitter, latency, loss, cpu;
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

class MainComponent : public juce::Component, private juce::Timer
{
public:
    MainComponent();                                   // loads settings, builds engine, shows lobby
    ~MainComponent() override;
    void showDemoSession();                            // fake state: 3 participants, track loaded & playing (for --demo / --screenshot)
    void joinFromLink (const SessionLink&);            // used by Main for ensayo:// arguments
    LobbyView& getLobby() noexcept                     { return lobby; }
    SessionView& getSession() noexcept                 { return session; }
    bool isInSession() const noexcept                  { return inSession; }

    // Tests and screenshots: no audio device is opened, no plugin is loaded, settings go to a temp file.
    static void setTestMode (bool);
    static bool isTestMode() noexcept;

    void resized() override;
    void paint (juce::Graphics&) override;

private:
    void timerCallback() override;

    // Lobby wiring.
    void wireLobby();
    void applyLobbyToSettings();
    void openAudioFromSettings();
    void refreshDeviceCombos();
    void applyAudioSelection();
    void scanPlugins();
    void loadPlugin (const juce::File&);
    void removePlugin();
    void togglePluginEditor();
    void updatePluginButtons();

    // Session wiring.
    void wireSession();
    void wireClient();
    void createSession();
    void joinFromLinkField();
    void leaveSession();
    void stopEmbeddedHost();
    void showLobby();
    void showSession();
    void queueGain (uint32_t target, const ChannelGain&);
    void flushGains (bool force);
    void applyMonitorFromSettings();
    void chooseTrackFile();
    void downloadTrack (const juce::String& url);
    void updateNumbers();
    void retry();

    void markSettingsDirty();
    void saveSettingsNow();
    void setStatus (const juce::String&, bool isError = false, bool showRetry = false);
    static juce::String firstLanAddress();

    AppSettings settings;
    juce::File settingsFile;
    AudioEngine engine;
    SessionClient client;
    std::unique_ptr<ServerHost> host;
    std::atomic<ServerHost*> activeHost { nullptr };
    YouTubeImporter importer;

    LobbyView lobby;
    SessionView session;
    StatusBar status;

    std::unique_ptr<juce::FileChooser> chooser;
    std::unique_ptr<PluginWindow> pluginWindow;
    juce::Array<juce::File> vst3Files;

    bool inSession = false, demoMode = false, audioError = false, deviceOpen = false;
    juce::String currentLink;
    SessionLink lastJoinLink;
    bool lastWasCreate = false;

    std::map<uint32_t, ChannelGain> pendingGains;
    juce::uint32 lastGainSendMs = 0, settingsDirtyAtMs = 0, sessionStartMs = 0;
    bool settingsDirty = false;

    static bool testMode;
    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MainComponent)
};
}
