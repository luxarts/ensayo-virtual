#include "ui/MainComponent.h"
#include "ui/Text.h"
#include "core/MixMath.h"
#include <cmath>

namespace ensayo
{
bool MainComponent::testMode = false;
void MainComponent::setTestMode (bool enabled)   { testMode = enabled; }
bool MainComponent::isTestMode() noexcept        { return testMode; }

namespace
{
    juce::File vst3Folder()
    {
        return juce::File (juce::SystemStats::getEnvironmentVariable ("CommonProgramFiles", "C:\\Program Files\\Common Files"))
                   .getChildFile ("VST3");
    }

    juce::File tempFolder()
    {
        auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual");
        dir.createDirectory();
        return dir;
    }

    juce::String percentText (float fraction)
    {
        return juce::String (juce::jlimit (0, 100, juce::roundToInt (fraction * 100.0f))) + " %";
    }

    // Synthetic waveform for the demo session.
    std::vector<Peak> demoPeaks (int count, float seed)
    {
        std::vector<Peak> out;
        out.reserve ((size_t) count);

        for (int i = 0; i < count; ++i)
        {
            const float a = 0.15f + 0.7f * std::abs (std::sin ((float) i * 0.013f + seed)) * (0.5f + 0.5f * std::sin ((float) i * 0.11f + seed));
            out.push_back ({ -a, a * 0.9f });
        }

        return out;
    }
}

//==============================================================================
StatusBar::StatusBar()
{
    setName ("status");

    text.setName ("status.text");
    text.setJustificationType (juce::Justification::centredLeft);
    text.setMinimumHorizontalScale (0.6f);
    addAndMakeVisible (text);

    retry.setName ("status.retry");
    retry.setButtonText ("Reintentar");
    retry.setWantsKeyboardFocus (false);
    retry.onClick = [this] { if (onRetry) onRetry(); };
    addAndMakeVisible (retry);

    const std::pair<juce::Label*, const char*> labels[] = {
        { &sync, "status.sync" }, { &ping, "status.ping" }, { &transfers, "status.transfers" },
        { &device, "status.device" }, { &cpu, "status.cpu" } };

    for (auto& [label, name] : labels)
    {
        label->setName (name);
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (13.0f));
        label->setMinimumHorizontalScale (0.6f);
        addChildComponent (*label);
    }

    setSession ({}, false, -1, 0, 0, {}, 0.0);
}

void StatusBar::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff1a1e24));
    g.setColour (juce::Colours::white.withAlpha (0.15f));
    g.drawLine (0.0f, 0.5f, (float) getWidth(), 0.5f);
}

void StatusBar::setSessionMode (bool enabled)
{
    sessionMode = enabled;

    for (auto* l : { &sync, &ping, &transfers, &device, &cpu })
        l->setVisible (enabled);

    resized();
}

void StatusBar::setMessage (const juce::String& message, bool isError, bool showRetry)
{
    text.setText (message, juce::dontSendNotification);
    text.setColour (juce::Label::textColourId, isError ? juce::Colours::orange : juce::Colours::white);
    retry.setEnabled (showRetry);
}

void StatusBar::setSession (const juce::String& syncText, bool readOnly, int pingMs, int uploads, int downloads,
                            const juce::String& deviceText, double cpuPercent)
{
    sync.setText (syncText, juce::dontSendNotification);
    sync.setColour (juce::Label::textColourId, readOnly ? juce::Colours::orange : juce::Colour (0xff8fd19e));
    ping.setText (pingMs < 0 ? juce::String ("Ping: --") : "Ping: " + juce::String (pingMs) + " ms", juce::dontSendNotification);
    transfers.setText ("Subidas: " + juce::String (uploads) + ES (" \xC2\xB7 Descargas: ") + juce::String (downloads),
                       juce::dontSendNotification);
    device.setText (deviceText, juce::dontSendNotification);
    cpu.setText ("CPU: " + juce::String (juce::roundToInt (cpuPercent)) + " %", juce::dontSendNotification);
}

void StatusBar::resized()
{
    auto r = getLocalBounds().reduced (8, 3);

    if (sessionMode)
    {
        const int widths[] = { 210, 90, 180, 200, 80 };
        juce::Label* labels[] = { &sync, &ping, &transfers, &device, &cpu };

        for (int i = 4; i >= 0; --i)
        {
            labels[i]->setBounds (r.removeFromRight (widths[i]));
            r.removeFromRight (4);
        }
    }

    retry.setBounds (r.removeFromRight (100));
    r.removeFromRight (8);
    text.setBounds (r);
}

//==============================================================================
PluginWindow::PluginWindow (juce::AudioPluginInstance& plugin, std::function<void()> onClose)
    : DocumentWindow (plugin.getName(), juce::Colours::darkgrey, DocumentWindow::closeButton),
      closeCallback (std::move (onClose))
{
    juce::AudioProcessorEditor* editor = plugin.hasEditor() ? plugin.createEditorAndMakeActive() : nullptr;

    if (editor == nullptr)
        editor = new juce::GenericAudioProcessorEditor (plugin);

    setUsingNativeTitleBar (true);
    setContentOwned (editor, true);
    setResizable (editor->isResizable(), false);
    centreWithSize (getWidth(), getHeight());
    setVisible (true);
}

//==============================================================================
juce::File MainComponent::projectsRoot()
{
    return testMode ? tempFolder().getChildFile ("test-projects") : defaultProjectsDirectory();
}

juce::File MainComponent::cacheRoot()
{
    return testMode ? tempFolder().getChildFile ("test-cache") : defaultCacheRoot();
}

MainComponent::MainComponent()
{
    setName ("main");
    setOpaque (true);

    settingsFile = testMode ? juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual-test-settings.json")
                            : defaultSettingsFile();
    juce::String settingsWarning;
    settings = loadSettings (settingsFile, &settingsWarning, &settingsFileKept);

    addAndMakeVisible (lobby);
    addChildComponent (daw);
    addAndMakeVisible (status);
    status.onRetry = [this] { retry(); };
    daw.setPeaksProvider (this);

    wireLobby();
    wireSession();
    wireDaw();

    engine.setChannels (settings.micChannel, settings.instrumentChannel, settings.outputPair);
    applyMonitorFromSettings();
    session.setMetronome (click, countIn);

    scanPlugins();

    if (! testMode)
    {
        openAudioFromSettings();

        if (settings.pluginFile.isNotEmpty() && juce::File (settings.pluginFile).exists())
            loadPlugin (juce::File (settings.pluginFile));
    }
    else
    {
        lobby.deviceBox.clear (juce::dontSendNotification);
        refreshDeviceCombos();
        applyCalibrationFromSettings();
        setStatus ("Modo de prueba: sin dispositivo de audio");
    }

    if (settingsWarning.isNotEmpty())
        setStatus (settingsWarning, true, false);

    showLobby();
    startTimer (50);
    setSize (1100, 760);
}

MainComponent::~MainComponent()
{
    stopTimer();
    pluginWindow = nullptr;
    importer.cancel();
    importer.onProgress = nullptr;
    importer.onFinished = nullptr;
    importer.onError = nullptr;

    // A running take is finished (its finish intent persists), threads are joined and the client and host go away
    // while the engine (and its device) still exist.
    session.leave();
    daw.setPeaksProvider (nullptr);
    engine.getDeviceManager().closeAudioDevice();
    saveSettingsNow();   // settings also persist on change, debounced 1 s in timerCallback
}

void MainComponent::paint (juce::Graphics& g)
{
    g.fillAll (findColour (juce::ResizableWindow::backgroundColourId));
}

void MainComponent::resized()
{
    auto area = getLocalBounds();
    status.setBounds (area.removeFromBottom (30));
    lobby.setBounds (area);
    daw.setBounds (area);
}

//==============================================================================
void MainComponent::setStatus (const juce::String& message, bool isError, bool showRetry)
{
    status.setMessage (message, isError, showRetry);
}

void MainComponent::markSettingsDirty()
{
    settingsDirty = true;
    settingsDirtyAtMs = juce::Time::getMillisecondCounter();
}

void MainComponent::saveSettingsNow()
{
    settingsDirty = false;

    if (settingsFileKept)
        return;   // a corrupt settings file that could not be moved aside is never overwritten

    settings.pluginState = engine.getPluginSlot().isLoaded() ? engine.getPluginSlot().getStateBase64() : juce::String();
    saveSettings (settings, settingsFile);
}

SessionIdentity MainComponent::identity() const
{
    return { settings.userId, settings.name, cacheRoot() };
}

juce::String MainComponent::deviceKey() const
{
    return calibrationKey (settings.deviceName, settings.bufferSize);
}

//==============================================================================
void MainComponent::wireLobby()
{
    lobby.nameEditor.setText (settings.name, juce::dontSendNotification);
    lobby.nameEditor.onTextChange = [this] { settings.name = lobby.nameEditor.getText().trim(); markSettingsDirty(); };

    lobby.roleBox.setSelectedId (LobbyView::roleToId (settings.role), juce::dontSendNotification);
    lobby.roleBox.onChange = [this]
    {
        settings.role = LobbyView::roleFromId (lobby.roleBox.getSelectedId());
        markSettingsDirty();
    };

    lobby.linkEditor.setText (settings.lastLink, juce::dontSendNotification);
    lobby.createButton.onClick = [this] { createProject(); };
    lobby.openButton.onClick = [this] { chooseProjectToOpen(); };
    lobby.joinButton.onClick = [this] { joinFromLinkField(); };
    lobby.linkEditor.onReturnKey = [this] { joinFromLinkField(); };
    lobby.calibrateButton.onClick = [this] { startCalibration(); };

    lobby.deviceBox.onChange = [this]
    {
        const auto name = lobby.deviceBox.getText();

        if (name.isNotEmpty() && name != settings.deviceName)
        {
            settings.deviceName = name;
            settings.deviceType = "ASIO";
            markSettingsDirty();
            openAudioFromSettings();
        }
    };

    lobby.rateBox.onChange = [this] { applyAudioSelection(); };
    lobby.bufferBox.onChange = [this] { applyAudioSelection(); };

    auto channelChanged = [this]
    {
        if (lobby.micBox.getSelectedId() > 0)        settings.micChannel = lobby.micBox.getSelectedId() - 1;
        if (lobby.instrumentBox.getSelectedId() > 0) settings.instrumentChannel = lobby.instrumentBox.getSelectedId() - 1;
        if (lobby.outputBox.getSelectedId() > 0)     settings.outputPair = lobby.outputBox.getSelectedId() - 1;
        engine.setChannels (settings.micChannel, settings.instrumentChannel, settings.outputPair);
        markSettingsDirty();
    };
    lobby.micBox.onChange = channelChanged;
    lobby.instrumentBox.onChange = channelChanged;
    lobby.outputBox.onChange = channelChanged;

    lobby.panelButton.onClick = [this]
    {
        if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
        {
            if (device->hasControlPanel() && device->showControlPanel())
                openAudioFromSettings();
        }
        else
        {
            setStatus ("No hay un dispositivo ASIO abierto", true, true);
        }
    };

    lobby.pluginBox.onChange = [this]
    {
        const int id = lobby.pluginBox.getSelectedId();

        if (id == 1)
            removePlugin();
        else if (id >= 2 && id - 2 < vst3Files.size())
            loadPlugin (vst3Files[id - 2]);
    };

    lobby.browseButton.onClick = [this]
    {
        chooser = std::make_unique<juce::FileChooser> (ES ("Eleg\xC3\xAD un plugin VST3"), vst3Folder(), "*.vst3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  if (fc.getResult() != juce::File())
                                      loadPlugin (fc.getResult());
                              });
    };

    lobby.editorButton.onClick = [this] { togglePluginEditor(); };
    lobby.removeButton.onClick = [this] { removePlugin(); };
    lobby.bypassButton.setToggleState (settings.pluginBypassed, juce::dontSendNotification);
    lobby.bypassButton.onClick = [this]
    {
        settings.pluginBypassed = lobby.bypassButton.getToggleState();
        engine.getPluginSlot().setBypassed (settings.pluginBypassed);
        markSettingsDirty();
    };

    updatePluginButtons();
}

void MainComponent::applyLobbyToSettings()
{
    settings.name = lobby.nameEditor.getText().trim();
    settings.role = LobbyView::roleFromId (lobby.roleBox.getSelectedId());
}

void MainComponent::applyMonitorFromSettings()
{
    engine.setMonitor (settings.monitorVoiceMuted ? 0.0f : dbToGain (settings.monitorVoiceDb),
                       settings.monitorInstrumentMuted ? 0.0f : dbToGain (settings.monitorInstrumentDb));
}

void MainComponent::openAudioFromSettings()
{
    deviceOpen = false;

    const auto names = engine.getAsioDeviceNames();
    lobby.deviceBox.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        lobby.deviceBox.addItem (names[i], i + 1);

    if (names.isEmpty())
    {
        refreshDeviceCombos();
        applyCalibrationFromSettings();
        setStatus ("No se encontraron drivers ASIO", true, true);
        return;
    }

    if (! names.contains (settings.deviceName))
        settings.deviceName = names[0];

    lobby.deviceBox.setSelectedId (names.indexOf (settings.deviceName) + 1, juce::dontSendNotification);

    // The studio runs at 48 kHz only (spec: global constraints); any buffer size.
    const auto error = engine.openDevice ("ASIO", settings.deviceName, (double) sampleRate, settings.bufferSize);

    if (error.isNotEmpty())
    {
        refreshDeviceCombos();
        applyCalibrationFromSettings();
        setStatus ("Error de audio: " + error, true, true);
        return;
    }

    deviceOpen = true;

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice())
    {
        settings.sampleRate = device->getCurrentSampleRate();
        settings.bufferSize = device->getCurrentBufferSizeSamples();
    }

    refreshDeviceCombos();
    applyCalibrationFromSettings();
    engine.setChannels (settings.micChannel, settings.instrumentChannel, settings.outputPair);

    if ((int) settings.sampleRate != sampleRate)
        setStatus ("El dispositivo funciona a " + juce::String ((int) settings.sampleRate)
                   + " Hz: cambialo a 48000 Hz en el Panel ASIO", true, true);
    else
        setStatus ("Audio: " + settings.deviceName + " @ 48000 Hz, " + juce::String (settings.bufferSize) + " muestras");
}

void MainComponent::refreshDeviceCombos()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();

    lobby.rateBox.clear (juce::dontSendNotification);
    lobby.bufferBox.clear (juce::dontSendNotification);
    lobby.micBox.clear (juce::dontSendNotification);
    lobby.instrumentBox.clear (juce::dontSendNotification);
    lobby.outputBox.clear (juce::dontSendNotification);

    lobby.rateBox.addItem ("48000 Hz", 1);
    lobby.rateBox.setSelectedId (1, juce::dontSendNotification);

    if (device == nullptr)
    {
        const int sizes[] = { 64, 128, 256, 512 };

        for (int i = 0; i < 4; ++i)
        {
            lobby.bufferBox.addItem (juce::String (sizes[i]), i + 1);

            if (sizes[i] == settings.bufferSize)
                lobby.bufferBox.setSelectedId (i + 1, juce::dontSendNotification);
        }

        lobby.micBox.addItem ("Entrada " + juce::String (settings.micChannel + 1), settings.micChannel + 1);
        lobby.micBox.setSelectedId (settings.micChannel + 1, juce::dontSendNotification);
        lobby.instrumentBox.addItem ("Entrada " + juce::String (settings.instrumentChannel + 1), settings.instrumentChannel + 1);
        lobby.instrumentBox.setSelectedId (settings.instrumentChannel + 1, juce::dontSendNotification);
        lobby.outputBox.addItem ("Salida " + juce::String (settings.outputPair * 2 + 1) + "+" + juce::String (settings.outputPair * 2 + 2), settings.outputPair + 1);
        lobby.outputBox.setSelectedId (settings.outputPair + 1, juce::dontSendNotification);
        lobby.latencyLabel.setText ("Latencia: --", juce::dontSendNotification);
        return;
    }

    const auto sizes = device->getAvailableBufferSizes();

    for (int i = 0; i < sizes.size(); ++i)
    {
        lobby.bufferBox.addItem (juce::String (sizes[i]), i + 1);

        if (sizes[i] == device->getCurrentBufferSizeSamples())
            lobby.bufferBox.setSelectedId (i + 1, juce::dontSendNotification);
    }

    const auto inputs = device->getInputChannelNames();

    for (int i = 0; i < inputs.size(); ++i)
    {
        lobby.micBox.addItem (juce::String (i + 1) + ": " + inputs[i], i + 1);
        lobby.instrumentBox.addItem (juce::String (i + 1) + ": " + inputs[i], i + 1);
    }

    lobby.micBox.setSelectedId (juce::jlimit (0, inputs.size(), settings.micChannel + 1), juce::dontSendNotification);
    lobby.instrumentBox.setSelectedId (juce::jlimit (0, inputs.size(), settings.instrumentChannel + 1), juce::dontSendNotification);

    const auto outputs = device->getOutputChannelNames();

    for (int i = 0; i + 1 < outputs.size(); i += 2)
        lobby.outputBox.addItem (outputs[i] + " + " + outputs[i + 1], i / 2 + 1);

    lobby.outputBox.setSelectedId (juce::jlimit (0, outputs.size() / 2, settings.outputPair + 1), juce::dontSendNotification);

    const double rate = juce::jmax (1.0, device->getCurrentSampleRate());
    lobby.latencyLabel.setText ("Latencia ASIO: " + juce::String (device->getInputLatencyInSamples() * 1000.0 / rate, 1) + " ms entrada / "
                                + juce::String (device->getOutputLatencyInSamples() * 1000.0 / rate, 1) + " ms salida",
                                juce::dontSendNotification);
}

void MainComponent::applyAudioSelection()
{
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    const int bufferId = lobby.bufferBox.getSelectedId();

    if (device == nullptr)
    {
        const int sizes[] = { 64, 128, 256, 512 };

        if (bufferId >= 1 && bufferId <= 4)
            settings.bufferSize = sizes[bufferId - 1];

        markSettingsDirty();
        applyCalibrationFromSettings();
        return;
    }

    const auto sizes = device->getAvailableBufferSizes();

    if (bufferId >= 1 && bufferId <= sizes.size())
        settings.bufferSize = sizes[bufferId - 1];

    markSettingsDirty();

    if (settings.bufferSize != device->getCurrentBufferSizeSamples())
        openAudioFromSettings();
}

void MainComponent::applyCalibrationFromSettings()
{
    const auto it = settings.calibration.find (deviceKey());
    const auto value = it != settings.calibration.end() ? std::optional<int> (it->second) : std::nullopt;
    session.setCalibration (value.value_or (0));
    lobby.setCalibration (value);
}

void MainComponent::startCalibration()
{
    if (! deviceOpen || engine.getDeviceManager().getCurrentAudioDevice() == nullptr)
    {
        setStatus (ES ("Abr\xC3\xAD un dispositivo ASIO antes de calibrar"), true, false);
        return;
    }

    if (calibrating || session.isRecording())
        return;

    calibrating = true;
    lobby.calibrateButton.setEnabled (false);
    engine.startCalibration();
    setStatus (ES ("Calibrando\xE2\x80\xA6 Conect\xC3\xA1 la salida a la entrada del micr\xC3\xB3" "fono (8 clicks, unos 5 s)"));
}

void MainComponent::pollCalibration()
{
    if (! calibrating || engine.isCalibrating())
        return;

    calibrating = false;
    lobby.calibrateButton.setEnabled (true);
    auto* device = engine.getDeviceManager().getCurrentAudioDevice();
    const auto roundTrip = engine.calibrationRoundTrip();

    if (! roundTrip || device == nullptr)
    {
        setStatus (ES ("No se detectaron los clicks en la entrada: revis\xC3\xA1 el cable o el loopback y el canal de micr\xC3\xB3" "fono"),
                   true, false);
        return;
    }

    const int value = LatencyCalibrator::calibrationFor (*roundTrip, device->getInputLatencyInSamples(), device->getOutputLatencyInSamples());
    settings.calibration[deviceKey()] = value;
    markSettingsDirty();
    applyCalibrationFromSettings();
    setStatus (ES ("Calibraci\xC3\xB3n: ") + juce::String (value) + " muestras (ida y vuelta " + juce::String (*roundTrip) + ")");
}

//==============================================================================
void MainComponent::scanPlugins()
{
    vst3Files = findVst3Plugins (vst3Folder());
    lobby.pluginBox.clear (juce::dontSendNotification);
    lobby.pluginBox.addItem ("(ninguno)", 1);

    for (int i = 0; i < vst3Files.size(); ++i)
        lobby.pluginBox.addItem (vst3Files[i].getFileNameWithoutExtension(), i + 2);

    const int index = vst3Files.indexOf (juce::File (settings.pluginFile));
    lobby.pluginBox.setSelectedId (index >= 0 && engine.getPluginSlot().isLoaded() ? index + 2 : 1, juce::dontSendNotification);
}

void MainComponent::loadPlugin (const juce::File& file)
{
    const bool reloadSameState = file == juce::File (settings.pluginFile);
    const auto result = engine.getPluginSlot().load (file, (double) sampleRate, settings.bufferSize,
                                                     reloadSameState ? settings.pluginState : juce::String());

    if (result.failed())
    {
        setStatus ("No se pudo cargar el plugin: " + result.getErrorMessage(), true, false);
        updatePluginButtons();
        return;
    }

    settings.pluginFile = file.getFullPathName();
    engine.getPluginSlot().setBypassed (settings.pluginBypassed);
    markSettingsDirty();

    if (! vst3Files.contains (file))
    {
        vst3Files.add (file);
        lobby.pluginBox.addItem (file.getFileNameWithoutExtension(), vst3Files.size() + 1);
    }

    lobby.pluginBox.setSelectedId (vst3Files.indexOf (file) + 2, juce::dontSendNotification);
    setStatus ("Plugin cargado: " + engine.getPluginSlot().getName());
    updatePluginButtons();
}

void MainComponent::removePlugin()
{
    pluginWindow = nullptr;
    engine.getPluginSlot().unload();
    settings.pluginFile.clear();
    settings.pluginState.clear();
    markSettingsDirty();
    lobby.pluginBox.setSelectedId (1, juce::dontSendNotification);
    updatePluginButtons();
}

void MainComponent::togglePluginEditor()
{
    if (pluginWindow != nullptr)
    {
        pluginWindow = nullptr;
    }
    else if (auto* instance = engine.getPluginSlot().getInstance())
    {
        pluginWindow = std::make_unique<PluginWindow> (*instance, [safe = juce::Component::SafePointer<MainComponent> (this)]
        {
            if (safe != nullptr)
            {
                safe->pluginWindow = nullptr;
                safe->updatePluginButtons();
            }
        });
    }
    else if (inSession)
    {
        setStatus (ES ("No hay un plugin VST cargado: elegilo en el lobby"), true, false);
    }

    updatePluginButtons();
}

void MainComponent::updatePluginButtons()
{
    const bool loaded = engine.getPluginSlot().isLoaded();
    lobby.editorButton.setEnabled (loaded);
    lobby.removeButton.setEnabled (loaded);
    lobby.bypassButton.setEnabled (loaded);
    lobby.editorButton.setButtonText (pluginWindow != nullptr ? "Cerrar editor" : "Abrir editor");
}

//==============================================================================
void MainComponent::setConnecting (bool pending)
{
    connecting = pending;
    lobby.createButton.setEnabled (! pending);
    lobby.openButton.setEnabled (! pending);
    lobby.joinButton.setEnabled (! pending);
}

void MainComponent::createProject()
{
    const auto dir = projectsRoot().getChildFile (newId());
    hostProject (dir);
}

void MainComponent::chooseProjectToOpen()
{
    const auto projects = listProjects (projectsRoot());
    juce::PopupMenu menu;
    std::vector<juce::File> dirs;

    for (const auto& dir : projects)
    {
        juce::String error;
        const auto parsed = projectFromJson (dir.getChildFile ("project.json").loadFileAsString(), error);

        if (! parsed)
            continue;

        dirs.push_back (dir);
        const auto when = dir.getChildFile ("project.json").getLastModificationTime().formatted ("%d/%m/%Y %H:%M");
        menu.addItem ((int) dirs.size(), parsed->name + "  (" + when + ")");
    }

    if (dirs.empty())
        menu.addItem (-2, "No hay proyectos guardados", false);

    menu.addSeparator();
    menu.addItem (-1, ES ("Buscar project.json en otra carpeta\xE2\x80\xA6"));

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&lobby.openButton),
                        [safe = juce::Component::SafePointer<MainComponent> (this), dirs] (int result)
                        {
                            if (safe == nullptr || result == 0)
                                return;

                            if (result > 0 && result <= (int) dirs.size())
                            {
                                safe->hostProject (dirs[(size_t) result - 1]);
                                return;
                            }

                            if (result != -1)
                                return;

                            safe->chooser = std::make_unique<juce::FileChooser> (ES ("Eleg\xC3\xAD el project.json del proyecto"),
                                                                                  projectsRoot(), "project.json");
                            safe->chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                                                        [safe] (const juce::FileChooser& fc)
                                                        {
                                                            if (safe != nullptr && fc.getResult().existsAsFile())
                                                                safe->hostProject (fc.getResult().getParentDirectory());
                                                        });
                        });
}

void MainComponent::hostProject (const juce::File& projectDir)
{
    applyLobbyToSettings();

    if (settings.name.isEmpty())
    {
        setStatus (ES ("Ingres\xC3\xA1 tu nombre antes de crear la sesi\xC3\xB3n"), true, false);
        return;
    }

    if (inSession)
        leaveSession();

    lastAction = LastAction::host;
    lastProjectDir = projectDir;
    const auto newName = "Proyecto " + juce::Time::getCurrentTime().formatted ("%d-%m-%Y %H.%M");
    const auto result = session.host (projectDir, settings.hostPort, identity(), newName);

    if (result.failed())
    {
        setStatus ("No se pudo abrir el proyecto en el puerto " + juce::String (settings.hostPort) + ": " + result.getErrorMessage(),
                   true, true);
        return;
    }

    settings.lastProject = projectDir.getFullPathName();
    markSettingsDirty();
    setConnecting (true);
    setStatus (ES ("Abriendo el proyecto\xE2\x80\xA6 Conectando\xE2\x80\xA6"));
}

void MainComponent::joinFromLinkField()
{
    const auto link = parseSessionLink (lobby.linkEditor.getText());

    if (! link)
    {
        setStatus (ES ("Link inv\xC3\xA1lido. Formato: ensayo://host:puerto/ID"), true, false);
        return;
    }

    joinFromLink (*link);
}

void MainComponent::joinFromLink (const SessionLink& link)
{
    applyLobbyToSettings();

    if (settings.name.isEmpty())
        settings.name = "Invitado";

    if (inSession)
        leaveSession();

    demoMode = false;
    lobby.linkEditor.setText (link.toString(), juce::dontSendNotification);
    settings.lastLink = link.toString();
    markSettingsDirty();
    lastAction = LastAction::join;
    lastJoinLink = link;
    setConnecting (true);
    setStatus ("Conectando a " + link.host + ":" + juce::String (link.port) + ES ("\xE2\x80\xA6"));
    session.join (link, identity());
}

//==============================================================================
void MainComponent::wireSession()
{
    session.onConnected = [this]
    {
        setConnecting (false);
        const bool reconnected = inSession;

        if (! inSession)
            showSession();

        const auto p = session.project();
        setStatus (reconnected ? ES ("Reconectado: sincronizado con el anfitri\xC3\xB3n")
                               : (session.isHosting() ? ES ("Proyecto abierto: ") : ES ("Conectado a ")) + (p != nullptr ? p->name : juce::String())
                                     + (session.isHosting() ? ES (". Compart\xC3\xAD el link con tu banda (puerto ")
                                                                  + juce::String (session.getConnectLink().port) + " abierto en el router si es por internet)."
                                                            : juce::String()));
        refreshSessionView();
    };

    session.onProjectChanged = [this] { if (inSession) refreshSessionView(); };

    session.onDisconnected = [this] (const juce::String& reason)
    {
        if (! inSession)
        {
            abandonConnection (reason);
            return;
        }

        // Spec §8: read-only until the user reconnects; a take in progress keeps recording locally.
        setStatus (ES ("S\xC3\xB3lo lectura: host desconectado (") + reason + ES ("). Toc\xC3\xA1 Reintentar para reconectar."), true, true);
        refreshSessionView();
    };

    session.onError = [this] (const juce::String& message)
    {
        if (! session.hasConnected())
        {
            abandonConnection (message);   // the connection or the join failed before any welcome
            return;
        }

        setStatus (message, true, ! session.isConnected());
    };

    session.onWarning = [this] (const juce::String& message) { setStatus (message, true, ! session.isConnected()); };
    session.onRejected = [this] (const juce::String& reason) { setStatus (ES ("Operaci\xC3\xB3n rechazada: ") + reason, true, false); };
    session.onImportProgress = [this] (const juce::String& name, float fraction)
    {
        setStatus ("Importando " + name + ": " + percentText (fraction));
    };
    session.onImportFinished = [this] (const juce::String& name, const juce::Result& result)
    {
        if (result.wasOk())
            setStatus ("Importado: " + name + ES (" (se sube en segundo plano)"));
        else
            setStatus (result.getErrorMessage(), true, false);
    };
    session.onExportProgress = [this] (float fraction) { setStatus ("Exportando la mezcla: " + percentText (fraction)); };
    session.onExportFinished = [this] (const juce::File& file, const juce::Result& result)
    {
        if (result.wasOk())
        {
            const auto info = session.lastExportInfo();
            const auto clipped = info.clippedSamples > 0
                ? ES (" (la mezcla satur\xC3\xB3: pico +") + juce::String (info.peakDb(), 1) + " dBFS)"
                : juce::String();
            setStatus ("Mezcla exportada: " + file.getFullPathName() + clipped, clipped.isNotEmpty(), false);
        }
        else
            setStatus ("No se pudo exportar: " + result.getErrorMessage(), true, false);
    };
    session.onTakeAborted = [this] (const juce::String& reason) { setStatus (reason, true, false); };

    importer.onProgress = [this] (float fraction) { setStatus ("Descargando de YouTube: " + percentText (fraction)); };
    importer.onFinished = [this] (const juce::File& file)
    {
        if (! inSession || demoMode)
            return;

        if (const auto result = session.importFile (file, file.getFileNameWithoutExtension()); result.failed())
            setStatus (result.getErrorMessage(), true, false);
    };
    importer.onError = [this] (const juce::String& message) { setStatus ("yt-dlp: " + message, true, false); };
}

// Called from inside a ProjectClient callback: the client is destroyed on the next message, not under its own call.
void MainComponent::abandonConnection (const juce::String& message)
{
    setConnecting (false);
    showLobby();
    setStatus (message, true, true);

    juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<MainComponent> (this)]
    {
        if (safe != nullptr && ! safe->inSession && ! safe->connecting)
            safe->session.leave();
    });
}

void MainComponent::wireDaw()
{
    daw.onOp = [this] (const juce::var& op)
    {
        if (demoMode)
        {
            if (const auto r = applyOp (*demoProject, op, demoState.me); ! r.ok)
                setStatus (ES ("Operaci\xC3\xB3n rechazada: ") + r.error, true, false);

            demoState.project = std::make_shared<const Project> (*demoProject);
            daw.setState (demoState);
            return;
        }

        if (auto* c = session.getClient())
            c->sendOp (op);
    };

    daw.onPlay = [this]
    {
        if (demoMode) { demoState.playing = true; daw.setState (demoState); return; }
        if (! session.isRecording()) engine.play();
    };

    daw.onStop = [this]
    {
        if (demoMode) { demoState.playing = false; demoState.recording = false; daw.setState (demoState); return; }

        if (session.isRecording())
            stopTake();
        else
            engine.stop();
    };

    daw.onRecord = [this] { recordOrStop(); };

    daw.onHome = [this]
    {
        if (demoMode) { demoState.playhead = 0; daw.setState (demoState); return; }
        engine.setPosition (0);
    };

    daw.onSeek = [this] (int64_t position)
    {
        if (demoMode) { demoState.playhead = juce::jmax ((int64_t) 0, position); daw.setState (demoState); return; }
        engine.setPosition (juce::jmax ((int64_t) 0, position));
    };

    daw.onArm = [this] (const Id& track)
    {
        armedTrack = track;
        if (demoMode) { demoState.armedTrack = track; daw.setState (demoState); return; }
        session.setArmedTrack (track);
    };

    daw.onSolo = [this] (const Id& track, bool on)
    {
        if (on) solo.insert (track); else solo.erase (track);
        engine.setSolo (solo);
        if (demoMode) { demoState.solo = solo; daw.setState (demoState); }
    };

    daw.onClick = [this] (bool on) { click = on; session.setMetronome (click, countIn); if (demoMode) demoState.click = on; };
    daw.onCountIn = [this] (bool on) { countIn = on; session.setMetronome (click, countIn); if (demoMode) demoState.countIn = on; };

    daw.onListen = [this] (bool on)
    {
        listen = on;

        if (demoMode) demoState.listen = on;   // the voice chat is always on in a session; the toggle goes away
    };

    daw.onCopyLink = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (daw.getState().link);
        setStatus ("Link copiado al portapapeles");
    };

    daw.onLeave = [this] { leaveSession(); };
    daw.onExport = [this] { chooseExportFile(); };
    daw.onToggleEditor = [this] { togglePluginEditor(); };
    daw.onImportFile = [this] { chooseImportFile(); };
    daw.onImportYouTube = [this] (const juce::String& url) { downloadFromYouTube (url); };
    daw.onAddVoice = [this] { addTrack (TrackKind::voice); };
    daw.onAddInstrument = [this] { addTrack (TrackKind::instrument); };
}

void MainComponent::addTrack (TrackKind kind)
{
    const auto p = demoMode ? std::shared_ptr<const Project> (demoProject) : session.project();
    const auto me = demoMode ? demoState.me : settings.userId;

    if (p == nullptr)
        return;

    int count = 1;

    for (const auto& t : p->tracks)
        if (t.author == me && t.kind == kind)
            ++count;

    const auto name = (kind == TrackKind::voice ? juce::String ("Voz ") : juce::String ("Instrumento ")) + juce::String (count);
    daw.onOp (Ops::addTrack (newId(), name, kind));
}

void MainComponent::recordOrStop()
{
    if (demoMode)
    {
        demoState.recording = ! demoState.recording;
        demoState.playing = demoState.recording;
        daw.setState (demoState);
        return;
    }

    if (session.isRecording())
    {
        stopTake();
        return;
    }

    if (armedTrack.isEmpty())
    {
        setStatus (ES ("Arm\xC3\xA1 una pista propia (\xE2\x97\x8FR) para grabar"), true, false);
        return;
    }

    if (const auto result = session.startTake (armedTrack); result.failed())
        setStatus (result.getErrorMessage(), true, false);
    else
        setStatus (countIn ? ES ("Grabando (cuenta de un comp\xC3\xA1s)\xE2\x80\xA6") : ES ("Grabando\xE2\x80\xA6"));

    refreshSessionView();
}

void MainComponent::stopTake()
{
    const auto result = session.stopTake();

    if (result)
        setStatus ("Toma terminada: " + formatTime (result->region.length) + ES (" (se sube en segundo plano)"));
    else
        setStatus (session.getTakeError().isNotEmpty() ? session.getTakeError() : juce::String ("La toma fue demasiado corta: no se guarda nada"),
                   true, false);

    refreshSessionView();
}

void MainComponent::chooseImportFile()
{
    if (demoMode)
        return;

    chooser = std::make_unique<juce::FileChooser> (ES ("Eleg\xC3\xAD un archivo de audio"), juce::File(),
                                                   "*.wav;*.flac;*.ogg;*.mp3;*.m4a;*.mp4;*.aac");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file == juce::File() || ! inSession)
                                  return;

                              if (const auto result = session.importFile (file); result.failed())
                                  setStatus (result.getErrorMessage(), true, false);
                          });
}

void MainComponent::downloadFromYouTube (const juce::String& url)
{
    if (url.isEmpty())
    {
        setStatus (ES ("Peg\xC3\xA1 una URL de YouTube primero"), true, false);
        return;
    }

    if (! YouTubeImporter::looksLikeYouTubeUrl (url))
    {
        setStatus (ES ("URL de YouTube no v\xC3\xA1lida"), true, false);
        return;
    }

    if (demoMode)
        return;

    const auto ytDlp = YouTubeImporter::findYtDlp (settings.ytDlpPath);

    if (ytDlp == juce::File())
    {
        setStatus (ES ("Falta tools\\yt-dlp.exe junto al programa (viene en el zip de la app). Respet\xC3\xA1 los t\xC3\xA9rminos de uso de YouTube."),
                   true, false);
        return;
    }

    setStatus (ES ("Descargando de YouTube\xE2\x80\xA6"));
    const auto outDir = tempFolder().getChildFile ("youtube");
    outDir.createDirectory();
    importer.start (ytDlp, url, outDir, YouTubeImporter::findDeno (ytDlp));
}

void MainComponent::chooseExportFile()
{
    if (demoMode)
        return;

    const auto p = session.project();

    if (p == nullptr)
        return;

    const auto safeName = juce::File::createLegalFileName (p->name.isNotEmpty() ? p->name : juce::String ("mezcla"));
    const auto initial = juce::File::getSpecialLocation (juce::File::userMusicDirectory).getChildFile (safeName + ".wav");
    chooser = std::make_unique<juce::FileChooser> ("Exportar la mezcla", initial, "*.wav");
    chooser->launchAsync (juce::FileBrowserComponent::saveMode | juce::FileBrowserComponent::canSelectFiles
                              | juce::FileBrowserComponent::warnAboutOverwriting,
                          [this] (const juce::FileChooser& fc)
                          {
                              auto file = fc.getResult();

                              if (file == juce::File() || ! inSession)
                                  return;

                              if (! file.hasFileExtension ("wav"))
                                  file = file.withFileExtension ("wav");

                              if (const auto result = session.exportMix (file); result.failed())
                                  setStatus (result.getErrorMessage(), true, false);
                          });
}

void MainComponent::leaveSession()
{
    int pending = 0;

    if (auto* c = session.getClient())
        pending = c->transfers().pendingUploads;

    importer.cancel();
    session.leave();
    demoMode = false;
    armedTrack.clear();
    solo.clear();
    engine.setSolo ({});
    listen = false;
    showLobby();
    setConnecting (false);

    setStatus (pending > 0 ? ES ("Saliste de la sesi\xC3\xB3n. Quedan ") + juce::String (pending)
                                 + ES (" segmentos por subir: se env\xC3\xAD" "an la pr\xC3\xB3xima vez que abras este proyecto.")
                           : ES ("Saliste de la sesi\xC3\xB3n"));
}

void MainComponent::showLobby()
{
    inSession = false;
    daw.setVisible (false);
    lobby.setVisible (true);
    status.setSessionMode (false);
}

void MainComponent::showSession()
{
    inSession = true;
    lobby.setVisible (false);
    daw.setVisible (true);
    status.setSessionMode (true);
    refreshSessionView();

    if (daw.isShowing())
        daw.grabKeyboardFocus();
}

void MainComponent::retry()
{
    if (inSession)
    {
        if (demoMode)
            return;

        if (! deviceOpen && ! testMode)
            openAudioFromSettings();

        if (session.isActive() && ! session.isConnected())
        {
            setStatus (ES ("Reconectando\xE2\x80\xA6"));
            session.reconnect();
        }

        return;
    }

    if (! deviceOpen && ! testMode)
        openAudioFromSettings();

    if (lastAction == LastAction::host && lastProjectDir != juce::File())
        hostProject (lastProjectDir);
    else if (lastAction == LastAction::join && lastJoinLink.port > 0)
        joinFromLink (lastJoinLink);
}

//==============================================================================
std::vector<Peak> MainComponent::peaksFor (const Id& take) const
{
    if (demoMode)
        return demoPeaks (8000, (float) (take.hashCode() % 97) * 0.1f);

    if (auto* c = const_cast<SessionController&> (session).getClient())
        if (auto* cache = c->cache())
            return cache->peaks (take);

    return {};
}

DawState MainComponent::buildState() const
{
    if (demoMode)
        return demoState;

    DawState s;
    auto& controller = const_cast<SessionController&> (session);
    auto* c = controller.getClient();

    if (c == nullptr)
        return s;

    s.project = c->project();
    s.me = settings.userId;
    s.isHost = c->isHost();
    s.readOnly = ! c->isConnected();
    s.link = controller.getShareLink().toString();

    for (const auto& p : c->presence())
        s.online[p.user] = p.online;

    s.online[settings.userId] = true;

    for (const auto& live : c->liveRecordings())
        s.live.push_back ({ live.user, live.take, live.track, live.start, live.peaks });

    if (const auto own = controller.ownLiveRecording())
        s.live.push_back ({ own->user, own->take, own->track, own->start, own->peaks });

    if (auto* cache = c->cache(); cache != nullptr && s.project != nullptr)
    {
        for (const auto& take : s.project->takes)
        {
            if (take.samples <= 0)
                continue;

            const auto cached = cache->sampleCount (take.id);

            if (cached < take.samples)
                s.takeAvailability[take.id] = (float) ((double) cached / (double) take.samples);
        }
    }

    s.playhead = engine.getPosition();
    s.playing = engine.isPlaying();
    s.recording = controller.isRecording();
    s.armedTrack = armedTrack;

    // Forget an arm or a solo whose track was deleted.
    if (s.project != nullptr && s.project->findTrack (armedTrack) == nullptr)
        s.armedTrack.clear();

    s.solo = solo;
    s.click = click;
    s.countIn = countIn;
    s.listen = listen;
    return s;
}

void MainComponent::updateSessionStatus()
{
    if (demoMode)
    {
        status.setSession ("Sincronizado (v " + juce::String ((juce::int64) demoProject->version) + ")", false, 12, 0, 1,
                           ES ("Demo \xC2\xB7 48 kHz \xC2\xB7 256"), 3.0);
        return;
    }

    auto* c = session.getClient();

    if (c == nullptr)
        return;

    const auto p = c->project();
    const bool connected = c->isConnected();
    const auto transfers = c->transfers();
    juce::String deviceText = "Sin dispositivo";

    if (auto* device = engine.getDeviceManager().getCurrentAudioDevice(); device != nullptr && deviceOpen)
        deviceText = device->getName() + ES (" \xC2\xB7 ") + juce::String (juce::roundToInt (device->getCurrentSampleRate() / 1000.0))
                     + ES (" kHz \xC2\xB7 ") + juce::String (device->getCurrentBufferSizeSamples());

    status.setSession (connected ? "Sincronizado (v " + juce::String ((juce::int64) (p != nullptr ? p->version : 0)) + ")"
                                 : ES ("S\xC3\xB3lo lectura: host desconectado"),
                       ! connected, connected ? c->pingMs() : -1, transfers.pendingUploads, transfers.downloadingTakes,
                       deviceText, engine.getDeviceManager().getCpuUsage() * 100.0);

    if (! connected)
        status.retry.setEnabled (true);
}

void MainComponent::refreshSessionView()
{
    if (! inSession)
        return;

    daw.setState (buildState());
    updateSessionStatus();
}

//==============================================================================
void MainComponent::showDemoSession()
{
    if (inSession && ! demoMode)
        leaveSession();

    demoMode = true;

    const Id me = settings.userId.isNotEmpty() ? settings.userId : newId();
    const Id other = newId();
    const Id guitarTake = newId(), voiceTake = newId(), importedTake = newId(), liveTake = newId();
    const Id guitar = newId(), voice = newId(), bass = newId(), drums = newId();

    auto p = std::make_shared<Project>();
    p->id = newId();
    p->name = "Tema nuevo";
    p->hostUser = other;   // I am a guest: the other user's track shows its lock
    p->bpm = 92.0;
    p->beatsPerBar = 4;
    p->version = 128;
    p->users = { { other, ES ("Mart\xC3\xADn") }, { me, settings.name.isNotEmpty() ? settings.name : juce::String ("Lucas") } };
    p->takes = { { guitarTake, me, 2, 12, 560000, true }, { voiceTake, me, 1, 8, 360000, true },
                 { importedTake, me, 2, 40, 1900000, true }, { liveTake, other, 1, 3, 150000, false } };
    p->tracks.push_back ({ guitar, ES ("Guitarra r\xC3\xADtmica"), me, TrackKind::instrument, -3.0f, -0.2f, false,
                           { { newId(), guitarTake, 48000, 0, 240000 }, { newId(), guitarTake, 340000, 260000, 280000 } } });
    p->tracks.push_back ({ voice, ES ("Voz gu\xC3\xAD" "a"), me, TrackKind::voice, -6.0f, 0.0f, false,
                           { { newId(), voiceTake, 144000, 0, 300000 } } });
    p->tracks.push_back ({ bass, "Bajo", other, TrackKind::instrument, 0.0f, 0.1f, false, {} });
    p->tracks.push_back ({ drums, ES ("Bater\xC3\xAD" "a (YouTube)"), me, TrackKind::imported, -8.5f, 0.0f, false,
                           { { newId(), importedTake, 0, 0, 1900000 } } });
    demoProject = p;

    DawState s;
    s.project = std::make_shared<const Project> (*p);
    s.me = me;
    s.isHost = false;
    s.link = SessionLink { "192.168.1.10", defaultPort, "DEMO42" }.toString();
    s.online[me] = true;
    s.online[other] = true;
    s.takeAvailability[importedTake] = 0.64f;
    s.live.push_back ({ other, liveTake, bass, 5 * (int64_t) sampleRate, demoPeaks (400, 1.3f) });
    s.playhead = 5 * (int64_t) sampleRate;
    s.armedTrack = guitar;
    s.click = click;
    s.countIn = countIn;
    demoState = s;
    armedTrack = guitar;
    demoLastMs = juce::Time::getMillisecondCounter();

    showSession();
    setStatus (ES ("Sesi\xC3\xB3n de demostraci\xC3\xB3n"));
}

void MainComponent::demoTick()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto elapsedSamples = (int64_t) (now - demoLastMs) * sampleRate / 1000;
    demoLastMs = now;

    if (demoState.playing)
        demoState.playhead += elapsedSamples;

    // The other user's take keeps growing: one peak per 256 samples.
    if (! demoState.live.empty())
    {
        auto& live = demoState.live.front();
        const auto target = live.peaks.size() + (size_t) (elapsedSamples / peakBlock);
        const auto more = demoPeaks ((int) target, 1.3f);
        live.peaks.assign (more.begin(), more.end());
    }

    daw.setState (demoState);
    updateSessionStatus();
}

//==============================================================================
void MainComponent::timerCallback()
{
    if (const auto error = engine.takeDeviceError(); error.isNotEmpty())
    {
        deviceOpen = engine.getDeviceManager().getCurrentAudioDevice() != nullptr
                     && engine.getDeviceManager().getCurrentAudioDevice()->isPlaying();

        // Spec §8: a device error while recording stops the take and keeps what was recorded.
        juce::String outcome;

        if (session.isRecording())
        {
            if (session.stopTake())
                outcome = ES (" (se conserva lo grabado)");
            else if (session.getTakeError().isNotEmpty())
                outcome = ". " + session.getTakeError();
        }

        setStatus ("Error de audio: " + error + outcome, true, true);
    }

    pollCalibration();

    if (settingsDirty && juce::Time::getMillisecondCounter() - settingsDirtyAtMs > 1000)
        saveSettingsNow();

    if (! inSession)
        return;

    if (demoMode)
        demoTick();
    else
        refreshSessionView();
}
}
