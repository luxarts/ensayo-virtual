#include "ui/MainComponent.h"

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

    ChannelGain combinedMonitor (const ChannelGain& voice, const ChannelGain& instrument)
    {
        return { voice.muted ? minFaderDb : voice.voiceDb,
                 instrument.muted ? minFaderDb : instrument.instrumentDb,
                 voice.muted && instrument.muted };
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
    retry.onClick = [this] { if (onRetry) onRetry(); };
    addAndMakeVisible (retry);

    const std::pair<juce::Label*, const char*> numbers[] = {
        { &ping, "status.ping" }, { &jitter, "status.jitter" }, { &latency, "status.latency" },
        { &loss, "status.loss" }, { &cpu, "status.cpu" } };

    for (auto& [label, name] : numbers)
    {
        label->setName (name);
        label->setJustificationType (juce::Justification::centred);
        label->setFont (juce::FontOptions (13.0f));
        label->setMinimumHorizontalScale (0.6f);
        addChildComponent (*label);
    }

    setNumbers (-1, 0, 0, 0.0, 0.0, 0.0);
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

    for (auto* l : { &ping, &jitter, &latency, &loss, &cpu })
        l->setVisible (enabled);

    resized();
}

void StatusBar::setMessage (const juce::String& message, bool isError, bool showRetry)
{
    text.setText (message, juce::dontSendNotification);
    text.setColour (juce::Label::textColourId, isError ? juce::Colours::orange : juce::Colours::white);
    retry.setEnabled (showRetry);
}

void StatusBar::setNumbers (int pingMs, int clientBlocks, int serverBlocks, double latencyMs, double lossPercent, double cpuPercent)
{
    ping.setText (pingMs < 0 ? juce::String ("Ping: --") : "Ping: " + juce::String (pingMs) + " ms", juce::dontSendNotification);
    jitter.setText ("Jitter: " + juce::String (clientBlocks) + "/" + juce::String (serverBlocks), juce::dontSendNotification);
    latency.setText ("Latencia est.: " + juce::String (juce::roundToInt (latencyMs)) + " ms", juce::dontSendNotification);
    loss.setText (ES ("Pérdida: ") + juce::String (lossPercent, 1) + " %", juce::dontSendNotification);
    cpu.setText ("CPU: " + juce::String (juce::roundToInt (cpuPercent)) + " %", juce::dontSendNotification);
}

void StatusBar::resized()
{
    auto r = getLocalBounds().reduced (8, 3);

    if (sessionMode)
    {
        const int widths[] = { 90, 90, 140, 110, 80 };
        juce::Label* labels[] = { &ping, &jitter, &latency, &loss, &cpu };

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
MainComponent::MainComponent()
{
    setName ("main");
    setOpaque (true);

    settingsFile = testMode ? juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual-test-settings.json")
                            : defaultSettingsFile();
    settings = loadSettings (settingsFile);

    // The hook stays installed for the lifetime of the engine; it only ticks while an embedded host is active.
    engine.setFrameHook ([this]
    {
        if (auto* h = activeHost.load (std::memory_order_acquire))
            h->tick();
    });

    addAndMakeVisible (lobby);
    addChildComponent (session);
    addAndMakeVisible (status);
    status.onRetry = [this] { retry(); };

    wireLobby();
    wireSession();
    wireClient();

    engine.setRole (settings.role);
    engine.setChannels (settings.micChannel, settings.instrumentChannel, settings.outputPair);
    engine.setReceiveGain (settings.receiveDb);
    applyMonitorFromSettings();

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
        setStatus ("Modo de prueba: sin dispositivo de audio");
    }

    showLobby();
    startTimerHz (30);
    setSize (1100, 760);
}

MainComponent::~MainComponent()
{
    stopTimer();
    pluginWindow = nullptr;
    importer.cancel();
    client.onStateChanged = nullptr;
    client.onError = nullptr;
    client.onJoined = nullptr;
    engine.setTransport (nullptr);
    client.leave();
    stopEmbeddedHost();
    saveSettingsNow();
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
    session.setBounds (area);
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
    settings.pluginState = engine.getPluginSlot().isLoaded() ? engine.getPluginSlot().getStateBase64() : juce::String();
    saveSettings (settings, settingsFile);
}

juce::String MainComponent::firstLanAddress()
{
    for (const auto& address : juce::IPAddress::getAllAddresses (false))
        if (! address.isIPv6 && address != juce::IPAddress::local() && address.toString() != "0.0.0.0")
            return address.toString();

    return "127.0.0.1";
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
        engine.setRole (settings.role);
        markSettingsDirty();
    };

    lobby.linkEditor.setText (settings.lastLink, juce::dontSendNotification);
    lobby.createButton.onClick = [this] { createSession(); };
    lobby.joinButton.onClick = [this] { joinFromLinkField(); };
    lobby.linkEditor.onReturnKey = [this] { joinFromLinkField(); };

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
        if (auto* device = engine.getDevice())
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
        chooser = std::make_unique<juce::FileChooser> (ES ("Elegí un plugin VST3"), vst3Folder(), "*.vst3");
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
    engine.setRole (settings.role);
}

void MainComponent::openAudioFromSettings()
{
    deviceOpen = false;
    audioError = false;

    const auto names = engine.getAsioDeviceNames();
    lobby.deviceBox.clear (juce::dontSendNotification);

    for (int i = 0; i < names.size(); ++i)
        lobby.deviceBox.addItem (names[i], i + 1);

    if (names.isEmpty())
    {
        refreshDeviceCombos();
        setStatus ("No se encontraron drivers ASIO", true, true);
        return;
    }

    if (! names.contains (settings.deviceName))
        settings.deviceName = names[0];

    lobby.deviceBox.setSelectedId (names.indexOf (settings.deviceName) + 1, juce::dontSendNotification);

    const auto error = engine.openDevice ("ASIO", settings.deviceName, settings.sampleRate, settings.bufferSize);

    if (error.isNotEmpty())
    {
        audioError = true;
        refreshDeviceCombos();
        setStatus ("Error de audio: " + error, true, true);
        return;
    }

    deviceOpen = true;
    refreshDeviceCombos();

    if (auto* device = engine.getDevice())
    {
        settings.sampleRate = device->getCurrentSampleRate();
        settings.bufferSize = device->getCurrentBufferSizeSamples();
    }

    engine.setChannels (settings.micChannel, settings.instrumentChannel, settings.outputPair);
    setStatus ("Audio: " + settings.deviceName + " @ " + juce::String ((int) settings.sampleRate) + " Hz, "
               + juce::String (settings.bufferSize) + " muestras");
}

void MainComponent::refreshDeviceCombos()
{
    auto* device = engine.getDevice();

    lobby.rateBox.clear (juce::dontSendNotification);
    lobby.bufferBox.clear (juce::dontSendNotification);
    lobby.micBox.clear (juce::dontSendNotification);
    lobby.instrumentBox.clear (juce::dontSendNotification);
    lobby.outputBox.clear (juce::dontSendNotification);

    if (device == nullptr)
    {
        lobby.rateBox.addItem ("48000 Hz", 1);
        lobby.rateBox.setSelectedId (1, juce::dontSendNotification);
        const int sizes[] = { 64, 128, 256 };

        for (int i = 0; i < 3; ++i)
            lobby.bufferBox.addItem (juce::String (sizes[i]), i + 1);

        lobby.bufferBox.setSelectedId (settings.bufferSize == 64 ? 1 : (settings.bufferSize == 256 ? 3 : 2), juce::dontSendNotification);
        lobby.micBox.addItem ("Entrada " + juce::String (settings.micChannel + 1), settings.micChannel + 1);
        lobby.micBox.setSelectedId (settings.micChannel + 1, juce::dontSendNotification);
        lobby.instrumentBox.addItem ("Entrada " + juce::String (settings.instrumentChannel + 1), settings.instrumentChannel + 1);
        lobby.instrumentBox.setSelectedId (settings.instrumentChannel + 1, juce::dontSendNotification);
        lobby.outputBox.addItem ("Salida " + juce::String (settings.outputPair * 2 + 1) + "+" + juce::String (settings.outputPair * 2 + 2), settings.outputPair + 1);
        lobby.outputBox.setSelectedId (settings.outputPair + 1, juce::dontSendNotification);
        lobby.latencyLabel.setText ("Latencia: --", juce::dontSendNotification);
        return;
    }

    const auto rates = device->getAvailableSampleRates();

    for (int i = 0; i < rates.size(); ++i)
    {
        lobby.rateBox.addItem (juce::String ((int) rates[i]) + " Hz", i + 1);

        if ((int) rates[i] == (int) device->getCurrentSampleRate())
            lobby.rateBox.setSelectedId (i + 1, juce::dontSendNotification);
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

    const double inMs = device->getInputLatencyInSamples() * 1000.0 / device->getCurrentSampleRate();
    const double outMs = device->getOutputLatencyInSamples() * 1000.0 / device->getCurrentSampleRate();
    lobby.latencyLabel.setText ("Latencia ASIO: " + juce::String (inMs, 1) + " ms entrada / " + juce::String (outMs, 1) + " ms salida",
                                juce::dontSendNotification);
}

void MainComponent::applyAudioSelection()
{
    auto* device = engine.getDevice();

    if (device == nullptr)
    {
        const int id = lobby.bufferBox.getSelectedId();
        settings.bufferSize = id == 1 ? 64 : (id == 3 ? 256 : 128);
        markSettingsDirty();
        return;
    }

    const auto rates = device->getAvailableSampleRates();
    const auto sizes = device->getAvailableBufferSizes();
    const int rateId = lobby.rateBox.getSelectedId(), bufferId = lobby.bufferBox.getSelectedId();

    if (rateId >= 1 && rateId <= rates.size())
        settings.sampleRate = rates[rateId - 1];

    if (bufferId >= 1 && bufferId <= sizes.size())
        settings.bufferSize = sizes[bufferId - 1];

    markSettingsDirty();

    if (settings.sampleRate != device->getCurrentSampleRate() || settings.bufferSize != device->getCurrentBufferSizeSamples())
        openAudioFromSettings();
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
    const auto result = engine.getPluginSlot().load (file, settings.sampleRate, settings.bufferSize,
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
void MainComponent::wireSession()
{
    session.onCopyLink = [this]
    {
        juce::SystemClipboard::copyTextToClipboard (currentLink);
        setStatus ("Link copiado al portapapeles");
    };

    session.onLeave = [this] { leaveSession(); };

    session.onMonitorChanged = [this] (const ChannelGain& voice, const ChannelGain& instrument)
    {
        settings.monitorVoiceDb = voice.voiceDb;
        settings.monitorVoiceMuted = voice.muted;
        settings.monitorInstrumentDb = instrument.instrumentDb;
        settings.monitorInstrumentMuted = instrument.muted;
        engine.setMonitor (combinedMonitor (voice, instrument));
        markSettingsDirty();
    };

    session.onParticipantGain = [this] (uint32_t id, const ChannelGain& gain) { queueGain (id, gain); };

    session.onTrackGain = [this] (const ChannelGain& gain)
    {
        settings.trackDb = gain.voiceDb;
        settings.trackMuted = gain.muted;
        markSettingsDirty();
        queueGain (0, gain);
    };

    session.onTransport = [this] (const juce::String& cmd, int64_t position, bool loop)
    {
        if (demoMode)
        {
            auto s = session.getState();

            if (cmd == "play")       s.transport.playing = true;
            else if (cmd == "pause") s.transport.playing = false;
            else if (cmd == "stop")  { s.transport.playing = false; s.transport.position = 0; }
            else if (cmd == "seek")  s.transport.position = position;
            else if (cmd == "loop")  s.transport.loop = loop;

            session.setState (s);
            return;
        }

        client.sendTransport (cmd, position, loop);
    };

    session.onLoadFile = [this] { chooseTrackFile(); };
    session.onDownload = [this] (const juce::String& url) { downloadTrack (url); };

    importer.onProgress = [this] (float fraction) { session.setUploadProgress (ES ("Descargando…"), fraction); };
    importer.onFinished = [this] (const juce::File& file)
    {
        session.setUploadProgress ("Subiendo " + file.getFileName(), 0.0f);
        client.uploadTrack (file);
    };
    importer.onError = [this] (const juce::String& message)
    {
        session.setUploadProgress ("", 0.0f);
        setStatus ("yt-dlp: " + message, true, false);
    };
}

void MainComponent::wireClient()
{
    client.onJoined = [this]
    {
        engine.setTransport (&client);
        const auto& s = client.getState();
        SessionLink link { host != nullptr ? firstLanAddress() : s.host, s.port, s.sessionId };
        currentLink = link.toString();
        settings.lastLink = currentLink;
        lobby.linkEditor.setText (currentLink, juce::dontSendNotification);
        markSettingsDirty();

        const auto hint = host != nullptr
            ? ES ("Compartí este link con tu banda. Si están fuera de tu red, abrí el puerto UDP/TCP ") + juce::String (s.port) + " en tu router."
            : ES ("Conectado a ") + s.host + ":" + juce::String (s.port);

        session.setLink (currentLink, hint);
        session.setState (s);
        sessionStartMs = juce::Time::getMillisecondCounter();
        showSession();
        setStatus (host != nullptr ? ES ("Sesión creada: ") + s.sessionId : ES ("Conectado a la sesión ") + s.sessionId);
        queueGain (0, { settings.trackDb, settings.trackDb, settings.trackMuted });
        flushGains (true);
    };

    client.onStateChanged = [this] (const SessionState& s)
    {
        if (inSession && ! demoMode)
            session.setState (s);
    };

    client.onError = [this] (const juce::String& message)
    {
        engine.setTransport (nullptr);
        client.leave();
        stopEmbeddedHost();
        showLobby();
        setStatus (message, true, true);
    };
}

void MainComponent::createSession()
{
    applyLobbyToSettings();

    if (settings.name.isEmpty())
    {
        setStatus (ES ("Ingresá tu nombre antes de crear la sesión"), true, false);
        return;
    }

    if (inSession)
        leaveSession();

    ServerConfig config;
    config.tcpPort = config.udpPort = settings.hostPort;
    config.frameSize = engine.getFrameSize();
    host = std::make_unique<ServerHost> (config);
    const auto result = host->start();

    if (result.failed())
    {
        host = nullptr;
        setStatus ("No se pudo abrir el puerto " + juce::String (settings.hostPort) + ": " + result.getErrorMessage()
                   + ES (". Probá con otro puerto en la configuración."), true, true);
        return;
    }

    if (engine.getDevice() != nullptr && deviceOpen)
        activeHost.store (host.get(), std::memory_order_release);
    else
        host->startInternalClock();

    lastWasCreate = true;
    setStatus (ES ("Creando sesión…"));
    client.create ("127.0.0.1", settings.hostPort, settings.name, settings.role);
}

void MainComponent::joinFromLinkField()
{
    const auto link = parseSessionLink (lobby.linkEditor.getText());

    if (! link)
    {
        setStatus (ES ("Link inválido. Formato: ensayo://host:puerto/ID"), true, false);
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

    lobby.linkEditor.setText (link.toString(), juce::dontSendNotification);
    lastJoinLink = link;
    lastWasCreate = false;
    setStatus ("Conectando a " + link.host + ":" + juce::String (link.port) + ES ("…"));
    client.join (link, settings.name, settings.role);
}

void MainComponent::leaveSession()
{
    client.leave();
    engine.setTransport (nullptr);
    stopEmbeddedHost();
    demoMode = false;
    showLobby();
    setStatus (ES ("Saliste de la sesión"));
}

void MainComponent::stopEmbeddedHost()
{
    if (host == nullptr)
        return;

    activeHost.store (nullptr, std::memory_order_release);
    juce::Thread::sleep (20);   // let an in-flight tick() finish before the host goes away
    host->stop();
    host = nullptr;
}

void MainComponent::showLobby()
{
    inSession = false;
    session.setVisible (false);
    lobby.setVisible (true);
    status.setSessionMode (false);
}

void MainComponent::showSession()
{
    inSession = true;
    lobby.setVisible (false);
    session.setVisible (true);
    status.setSessionMode (true);
    session.getLocalStrip().setGain ({ settings.monitorVoiceDb, settings.monitorVoiceDb, settings.monitorVoiceMuted },
                                     { settings.monitorInstrumentDb, settings.monitorInstrumentDb, settings.monitorInstrumentMuted },
                                     juce::dontSendNotification);
    session.setTrackGain ({ settings.trackDb, settings.trackDb, settings.trackMuted }, juce::dontSendNotification);
    updateNumbers();
}

void MainComponent::applyMonitorFromSettings()
{
    engine.setMonitor (combinedMonitor ({ settings.monitorVoiceDb, settings.monitorVoiceDb, settings.monitorVoiceMuted },
                                        { settings.monitorInstrumentDb, settings.monitorInstrumentDb, settings.monitorInstrumentMuted }));
}

void MainComponent::queueGain (uint32_t target, const ChannelGain& gain)
{
    pendingGains[target] = gain;
    flushGains (false);
}

void MainComponent::flushGains (bool force)
{
    if (pendingGains.empty() || demoMode)
    {
        if (demoMode)
            pendingGains.clear();

        return;
    }

    const auto now = juce::Time::getMillisecondCounter();

    if (! force && now - lastGainSendMs < 50)
        return;

    for (const auto& [target, gain] : pendingGains)
        client.sendGain (target, gain);

    pendingGains.clear();
    lastGainSendMs = now;
}

void MainComponent::chooseTrackFile()
{
    chooser = std::make_unique<juce::FileChooser> (ES ("Elegí un backing track"), juce::File(), "*.wav;*.mp3;*.flac;*.ogg");
    chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles,
                          [this] (const juce::FileChooser& fc)
                          {
                              const auto file = fc.getResult();

                              if (file == juce::File())
                                  return;

                              if (demoMode)
                              {
                                  auto s = session.getState();
                                  s.transport.name = file.getFileName();
                                  s.transport.loaded = true;
                                  session.setState (s);
                                  return;
                              }

                              session.setUploadProgress ("Subiendo " + file.getFileName(), 0.0f);
                              client.uploadTrack (file);
                          });
}

void MainComponent::downloadTrack (const juce::String& url)
{
    if (url.isEmpty())
    {
        setStatus ("Pegá una URL de YouTube primero", true, false);
        return;
    }

    const auto ytDlp = YouTubeImporter::findYtDlp (settings.ytDlpPath);

    if (ytDlp == juce::File())
    {
        setStatus (ES ("No se encontró yt-dlp.exe: instalalo (yt-dlp + deno + ffmpeg) junto al programa o en el PATH. Respetá los términos de uso de YouTube."), true, false);
        return;
    }

    session.setUploadProgress (ES ("Descargando…"), 0.0f);
    importer.start (ytDlp, url, tempFolder());
}

void MainComponent::retry()
{
    if (inSession)
        return;

    if (! deviceOpen)
        openAudioFromSettings();

    if (lastWasCreate && ! deviceOpen)
        return;

    if (lastWasCreate)
        createSession();
    else if (lastJoinLink.port > 0)
        joinFromLink (lastJoinLink);
}

//==============================================================================
void MainComponent::showDemoSession()
{
    if (inSession && ! demoMode)
        leaveSession();

    demoMode = true;

    SessionState s;
    s.connected = true;
    s.sessionId = "DEMO42";
    s.clientId = 7;
    s.host = "192.168.1.10";
    s.port = defaultPort;
    s.participants = { { 7, settings.name.isNotEmpty() ? settings.name : juce::String ("Lucas"), Role::both },
                       { 1, "Ana", Role::voice }, { 2, "Bruno", Role::instrument }, { 3, "Caro", Role::both } };
    s.stats = { { 7, 4, 0, 0, 0.0f, 0.0f }, { 1, 4, 0, 0, 0.45f, 0.0f }, { 2, 5, 2, 1, 0.0f, 0.7f }, { 3, 3, 0, 0, 0.3f, 0.55f } };
    s.transport.loaded = true;
    s.transport.playing = true;
    s.transport.loop = false;
    s.transport.name = "ensayo.mp3";
    s.transport.position = (int64_t) (83 * sampleRate);
    s.transport.length = (int64_t) (225 * sampleRate);
    s.pingMs = 12;
    s.frameSize = FrameSize::f128;

    currentLink = SessionLink { s.host, s.port, s.sessionId }.toString();
    session.setLink (currentLink, ES ("Compartí este link con tu banda. Si están fuera de tu red, abrí el puerto UDP/TCP ")
                                  + juce::String (s.port) + " en tu router.");
    session.setState (s);
    session.setLocalMeters (0.4f, 0.6f);
    sessionStartMs = juce::Time::getMillisecondCounter();
    showSession();
    setStatus (ES ("Sesión de demostración"));
}

//==============================================================================
void MainComponent::updateNumbers()
{
    if (! inSession)
        return;

    const auto& s = demoMode ? session.getState() : client.getState();
    const double frameMs = frameSamples (s.frameSize) * 1000.0 / sampleRate;

    int clientBlocks = demoMode ? 4 : client.receiveBuffer().getStats().sizeBlocks;
    int serverBlocks = 0;
    uint32_t underruns = 0;

    for (const auto& st : s.stats)
    {
        if (st.id == s.clientId)
        {
            serverBlocks = st.jitterBlocks;
            underruns = st.underruns;
        }
    }

    double asioMs = demoMode ? 6.0 : 0.0;

    if (auto* device = engine.getDevice(); device != nullptr && ! demoMode)
        asioMs = (device->getInputLatencyInSamples() + device->getOutputLatencyInSamples()) * 1000.0 / juce::jmax (1.0, device->getCurrentSampleRate());

    const double ping = s.pingMs < 0 ? 0.0 : (double) s.pingMs;
    const double latency = ping + frameMs + 0.7 * (clientBlocks + serverBlocks) * frameMs + asioMs + 1.5;

    double lossPercent = 0.0;

    if (! demoMode)
    {
        const auto stats = client.receiveBuffer().getStats();
        const double elapsedFrames = juce::jmax (1.0, (juce::Time::getMillisecondCounter() - sessionStartMs) / frameMs);
        lossPercent = juce::jlimit (0.0, 100.0, 100.0 * (double) (stats.underruns + stats.late + underruns) / elapsedFrames);
    }

    const double cpu = demoMode ? 3.0 : engine.getDeviceManager().getCpuUsage() * 100.0;
    status.setNumbers (s.pingMs, clientBlocks, serverBlocks, latency, lossPercent, cpu);
}

void MainComponent::timerCallback()
{
    const auto peaks = engine.takePeaks();

    if (inSession && ! demoMode)
        session.setLocalMeters (peaks.voice, peaks.instrument);

    if (const auto error = engine.takeDeviceError(); error.isNotEmpty())
    {
        audioError = true;
        deviceOpen = false;
        setStatus ("Error de audio: " + error, true, true);
    }

    flushGains (false);

    if (settingsDirty && juce::Time::getMillisecondCounter() - settingsDirtyAtMs > 1000)
        saveSettingsNow();

    if (demoMode && inSession)
    {
        auto s = session.getState();

        if (s.transport.playing)
        {
            s.transport.position += sampleRate / 30;

            if (s.transport.position >= s.transport.length)
                s.transport.position = s.transport.loop ? 0 : s.transport.length;

            if (s.transport.position >= s.transport.length)
                s.transport.playing = false;

            session.setState (s);
        }

        session.setLocalMeters (0.4f + 0.2f * (float) std::sin (juce::Time::getMillisecondCounterHiRes() / 300.0),
                                0.6f + 0.2f * (float) std::sin (juce::Time::getMillisecondCounterHiRes() / 410.0));
    }

    if (inSession)
    {
        updateNumbers();

        if (! demoMode && ! audioError && engine.getDevice() != nullptr
            && juce::Time::getMillisecondCounter() - sessionStartMs > 3000 && ! client.receiveBuffer().getStats().primed)
            setStatus ("Sin audio del servidor", true, false);
    }
}
}
