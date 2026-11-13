#include "ui/MainComponent.h"
#include "ui/Text.h"
#include "server/ProjectHost.h"
#include "MessagePump.h"

namespace ensayo
{
namespace
{
    // Pumps the message loop until the predicate holds or the timeout passes.
    bool waitFor (const std::function<bool()>& predicate, int timeoutMs)
    {
        const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;

        while (juce::Time::getMillisecondCounter() < deadline)
        {
            if (predicate())
                return true;

            pumpMessages (10);
        }

        return predicate();
    }

    juce::Component* findByName (juce::Component& root, const juce::String& name)
    {
        if (root.getName() == name)
            return &root;

        for (auto* child : root.getChildren())
            if (auto* found = findByName (*child, name))
                return found;

        return nullptr;
    }

    bool isShowingWithinRoot (juce::Component& root, juce::Component* c)
    {
        for (auto* p = c; p != nullptr; p = p->getParentComponent())
        {
            if (! p->isVisible())
                return false;

            if (p == &root)
                return true;
        }

        return false;
    }

    juce::String labelText (juce::Component& root, const juce::String& name)
    {
        if (auto* label = dynamic_cast<juce::Label*> (findByName (root, name)))
            return label->getText();

        return {};
    }

    juce::String statusText (juce::Component& root)   { return labelText (root, "status.text"); }

    juce::File testSettingsFile()
    {
        return juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("EnsayoVirtual-test-settings.json");
    }

    // Test-mode MainComponents read this file; writing it first sets the name, the host port and a fixed user id.
    AppSettings writeTestSettings (const juce::String& name, int hostPort)
    {
        AppSettings s;
        s.name = name;
        s.hostPort = hostPort;
        s.userId = newId();
        saveSettings (s, testSettingsFile());
        return s;
    }

    const char* const lobbyNames[] = {
        "lobby.name", "lobby.role", "lobby.create", "lobby.open", "lobby.link", "lobby.join",
        "audio.device", "audio.rate", "audio.buffer", "audio.panel", "audio.mic", "audio.instrument",
        "audio.output", "audio.latency", "lobby.calibrate", "lobby.calibration",
        "vst.list", "vst.browse", "vst.editor", "vst.bypass", "vst.remove",
        "status.text", "status.retry" };

    const char* const sessionNames[] = {
        "daw.project", "daw.link", "daw.copy", "daw.users", "daw.listen", "daw.editor", "daw.leave",
        "transport.home", "transport.play", "transport.stop", "transport.record", "transport.time",
        "transport.bpm", "transport.meter", "transport.click", "transport.countin",
        "edit.split", "edit.delete", "daw.export", "timeline",
        "tracks.addVoice", "tracks.addInstrument", "tracks.import", "tracks.youtube",
        "track.1.name", "track.2.name", "track.3.name", "track.4.name",
        "track.1.arm", "track.2.arm", "track.3.lock",
        "track.1.mute", "track.1.solo", "track.1.volume", "track.1.pan",
        "status.text", "status.sync", "status.ping", "status.transfers", "status.device", "status.cpu", "status.retry" };
}

class UiTests : public juce::UnitTest
{
public:
    UiTests() : juce::UnitTest ("Ui", "EnsayoVirtual") {}

    void expectControls (juce::Component& root, const char* const* names, size_t count, const juce::String& view)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const juce::String controlName (names[i]);
            auto* c = findByName (root, controlName);
            expect (c != nullptr, view + ": missing " + controlName);

            if (c == nullptr)
                continue;

            expect (isShowingWithinRoot (root, c), view + ": not visible " + controlName);
            expect (c->getWidth() > 0 && c->getHeight() > 0, view + ": empty bounds " + controlName);
        }
    }

    void runTest() override
    {
        MainComponent::setTestMode (true);
        MainComponent::projectsRoot().deleteRecursively();

        beginTest ("lobby shows every named control, the calibration says sin calibrar");
        {
            testSettingsFile().deleteFile();
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            expect (! main.isInSession());
            expectControls (main, lobbyNames, std::size (lobbyNames), "lobby");
            expect (findByName (main, "timeline") == nullptr || ! isShowingWithinRoot (main, findByName (main, "timeline")));
            expect (labelText (main, "lobby.calibration").contains ("sin calibrar"), labelText (main, "lobby.calibration"));
            expectEquals (dynamic_cast<juce::Button*> (findByName (main, "lobby.open"))->getButtonText(), ES ("Abrir proyecto\xE2\x80\xA6"));
            expectEquals (dynamic_cast<juce::Button*> (findByName (main, "lobby.calibrate"))->getButtonText(), juce::String ("Calibrar latencia"));
            expect (isValidId (main.getSettings().userId));
        }

        beginTest ("a stored calibration for the device and buffer shows in the lobby");
        {
            auto s = writeTestSettings ("Ana", 22246);
            s.calibration[calibrationKey ("", 256)] = 37;
            saveSettings (s, testSettingsFile());
            MainComponent main;
            main.setSize (1100, 760);
            expect (labelText (main, "lobby.calibration").contains ("37 muestras"), labelText (main, "lobby.calibration"));
        }

        beginTest ("demo session is the DAW view with every named control and hides the lobby");
        {
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            main.showDemoSession();
            expect (main.isInSession() && main.isDemo());
            expectControls (main, sessionNames, std::size (sessionNames), "session");
            expect (findByName (main, "track.5.name") == nullptr);
            expect (findByName (main, "track.4.arm") == nullptr || ! isShowingWithinRoot (main, findByName (main, "track.4.arm")),
                    "imported tracks cannot be armed");

            for (auto* lobbyName : lobbyNames)
            {
                const juce::String n (lobbyName);

                if (n.startsWith ("status."))
                    continue;

                auto* c = findByName (main, n);
                expect (c == nullptr || ! isShowingWithinRoot (main, c), "lobby control still visible in session: " + n);
            }

            const auto& state = main.getDaw().getState();
            expect (state.project != nullptr && state.project->tracks.size() == 4);
            expect (state.live.size() == 1, "the other user's live take");
            expect (state.takeAvailability.size() == 1 && std::abs (state.takeAvailability.begin()->second - 0.64f) < 1e-6f);
            expect (labelText (main, "status.sync").startsWith ("Sincronizado (v "), labelText (main, "status.sync"));
            expect (labelText (main, "status.transfers").startsWith ("Subidas: "), labelText (main, "status.transfers"));

            // Demo edits go through the real op validation.
            const auto before = state.project->version;
            main.getDaw().onAddVoice();
            expect (main.getDaw().getState().project->tracks.size() == 5);
            expect (main.getDaw().getState().project->version == before + 1);

            // YouTube URLs are validated before anything is launched.
            main.getDaw().onImportYouTube ("https://example.com/watch?v=abc");
            expect (statusText (main).contains ("URL de YouTube no v"), "status: " + statusText (main));

            main.getDaw().onLeave();
            expect (! main.isInSession());
        }

        beginTest ("Crear sesion hosts a new project, connects as host and edits go through the host");
        {
            const auto s = writeTestSettings ("Ana", 22247);
            juce::File projectDir;

            {
                MainComponent main;
                main.setVisible (true);
                main.setSize (1100, 760);
                main.getLobby().createButton.onClick();
                expect (statusText (main).contains ("Conectando"), "status: " + statusText (main));
                expect (! main.getLobby().createButton.isEnabled() && ! main.getLobby().joinButton.isEnabled(),
                        "buttons stay enabled while connecting");
                expect (waitFor ([&] { return main.isInSession(); }, 8000), "never joined: " + statusText (main));
                expect (main.getLobby().createButton.isEnabled(), "buttons not re-enabled after joining");

                main.refreshSessionView();
                const auto& state = main.getDaw().getState();
                expect (state.project != nullptr && state.isHost && ! state.readOnly);
                expectEquals (state.me, s.userId);
                expect (state.project != nullptr && state.project->name.startsWith ("Proyecto "));
                expect (state.link.startsWith ("ensayo://") && state.link.contains (":22247/"), state.link);
                expect (labelText (main, "status.sync").startsWith ("Sincronizado (v "), labelText (main, "status.sync"));
                expectEquals (labelText (main, "status.transfers"), ES ("Subidas: 0 \xC2\xB7 Descargas: 0"));

                main.getDaw().onAddVoice();
                main.getDaw().onAddInstrument();
                expect (waitFor ([&] { auto p = main.getSession().project(); return p != nullptr && p->tracks.size() == 2; }, 5000),
                        "tracks never applied");
                main.refreshSessionView();
                expectEquals (main.getDaw().getState().project->tracks[0].name, juce::String ("Voz 1"));
                expect (main.getDaw().getState().project->tracks[1].kind == TrackKind::instrument);
                expect (findByName (main, "track.2.arm") != nullptr);

                // No device in test mode: Grabar explains instead of recording.
                main.getDaw().onRecord();
                expect (statusText (main).contains (ES ("Arm\xC3\xA1")), "status: " + statusText (main));
                main.getDaw().onArm (main.getDaw().getState().project->tracks[0].id);
                main.getDaw().onRecord();
                expect (statusText (main).contains ("dispositivo"), "status: " + statusText (main));
                expect (! main.getSession().isRecording());

                projectDir = juce::File (main.getSettings().lastProject);
                expect (projectDir.getChildFile ("project.json").existsAsFile(), projectDir.getFullPathName());
                expect (projectDir.isAChildOf (MainComponent::projectsRoot()));

                main.getDaw().onLeave();
                expect (! main.isInSession());
                expect (! main.getSession().isActive());
            }

            beginTest ("reopening the project keeps its tracks");
            {
                MainComponent main;
                main.setVisible (true);
                main.setSize (1100, 760);
                main.hostProject (projectDir);
                expect (waitFor ([&] { return main.isInSession(); }, 8000), "never joined: " + statusText (main));
                const auto p = main.getSession().project();
                expect (p != nullptr && p->tracks.size() == 2);
                main.getDaw().onLeave();
            }
        }

        beginTest ("a guest joins a link; a lost connection is read-only until Reintentar reconnects");
        {
            const auto hostUser = newId();
            const auto dir = MainComponent::projectsRoot().getChildFile ("guest-test");
            dir.deleteRecursively();
            ProjectHost host ({ 22248, 8 });
            expect (host.start (dir, hostUser, "Host", "Proyecto invitado").wasOk());
            const auto s = writeTestSettings ("Bruno", 22249);

            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            main.joinFromLink ({ "127.0.0.1", 22248, host.getSessionId() });
            expect (waitFor ([&] { return main.isInSession(); }, 8000), "never joined: " + statusText (main));
            main.refreshSessionView();
            expect (! main.getDaw().getState().isHost && ! main.getDaw().getState().readOnly);
            expect (main.getDaw().getState().link.contains (host.getSessionId()));
            expect (main.getDaw().getState().online.count (hostUser) == 1, "the host is listed");

            main.getSession().getClient()->dropConnectionForTest();
            expect (waitFor ([&] { return ! main.getSession().isConnected(); }, 8000));
            main.refreshSessionView();
            expect (main.getDaw().getState().readOnly);
            expectEquals (labelText (main, "status.sync"), ES ("S\xC3\xB3lo lectura: host desconectado"));
            expect (main.getStatus().retry.isEnabled());
            expect (main.isInSession(), "a lost connection keeps the session view");

            main.getStatus().retry.onClick();
            expect (waitFor ([&] { return main.getSession().isConnected(); }, 8000), "Reintentar did not reconnect: " + statusText (main));
            main.refreshSessionView();
            expect (! main.getDaw().getState().readOnly);
            expect (labelText (main, "status.sync").startsWith ("Sincronizado"));

            main.getDaw().onLeave();
            host.stop();
        }

        beginTest ("joining a dead link goes back to the lobby with Reintentar");
        {
            writeTestSettings ("Caro", 22250);
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            main.joinFromLink ({ "127.0.0.1", 22251, "ABCDEF" });
            expect (waitFor ([&] { return ! main.getLobby().joinButton.isEnabled() == false && statusText (main).isNotEmpty()
                                          && ! statusText (main).startsWith ("Conectando"); }, 15000),
                    "still connecting: " + statusText (main));
            expect (! main.isInSession());
            expect (main.getStatus().retry.isEnabled());
            expect (waitFor ([&] { return ! main.getSession().isActive(); }, 2000), "the failed client was not released");
        }

        testSettingsFile().deleteFile();
        MainComponent::projectsRoot().deleteRecursively();
    }
};

static UiTests uiTests;
}
