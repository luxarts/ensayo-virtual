#include "ui/MainComponent.h"
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

    juce::String statusText (juce::Component& root);

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

    juce::String statusText (juce::Component& root)
    {
        if (auto* label = dynamic_cast<juce::Label*> (findByName (root, "status.text")))
            return label->getText();

        return {};
    }

    const char* const lobbyNames[] = {
        "lobby.name", "lobby.role", "lobby.create", "lobby.link", "lobby.join",
        "audio.device", "audio.rate", "audio.buffer", "audio.panel", "audio.mic", "audio.instrument",
        "audio.output", "audio.latency", "vst.list", "vst.browse", "vst.editor", "vst.bypass", "vst.remove",
        "status.text", "status.retry" };

    const char* const sessionNames[] = {
        "session.link", "session.copy", "session.leave",
        "me.voice.fader", "me.voice.mute", "me.voice.meter", "me.instrument.fader", "me.instrument.mute", "me.instrument.meter",
        "participant.1.name", "participant.1.role", "participant.1.voice.fader", "participant.1.voice.mute",
        "participant.1.instrument.fader", "participant.1.instrument.mute", "participant.1.meter",
        "participant.2.name", "participant.2.role", "participant.2.voice.fader", "participant.2.voice.mute",
        "participant.2.instrument.fader", "participant.2.instrument.mute", "participant.2.meter",
        "participant.3.name", "participant.3.role", "participant.3.voice.fader", "participant.3.voice.mute",
        "participant.3.instrument.fader", "participant.3.instrument.mute", "participant.3.meter",
        "track.load", "track.url", "track.download", "track.name", "track.play", "track.stop", "track.loop",
        "track.position", "track.time", "track.fader", "track.mute", "track.progress",
        "status.ping", "status.jitter", "status.latency", "status.loss", "status.cpu", "status.text", "status.retry" };
}

class UiTests : public juce::UnitTest
{
public:
    UiTests() : juce::UnitTest ("Ui", "EnsayoVirtual") {}

    void expectControls (MainComponent& main, const char* const* names, size_t count, const juce::String& view)
    {
        for (size_t i = 0; i < count; ++i)
        {
            const juce::String controlName (names[i]);
            auto* c = findByName (main, controlName);
            expect (c != nullptr, view + ": missing " + controlName);

            if (c == nullptr)
                continue;

            expect (isShowingWithinRoot (main, c), view + ": not visible " + controlName);
            expect (c->getWidth() > 0 && c->getHeight() > 0, view + ": empty bounds " + controlName);
        }
    }

    void runTest() override
    {
        MainComponent::setTestMode (true);

        beginTest ("lobby shows every named control");
        {
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            expect (! main.isInSession());
            expectControls (main, lobbyNames, std::size (lobbyNames), "lobby");
            expect (findByName (main, "participant.1.name") == nullptr);
        }

        beginTest ("demo session shows every named control and hides the lobby");
        {
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            main.showDemoSession();
            expect (main.isInSession());
            expectControls (main, sessionNames, std::size (sessionNames), "session");
            expect (findByName (main, "participant.4.name") == nullptr);

            for (auto* lobbyName : lobbyNames)
            {
                const juce::String n (lobbyName);

                if (n.startsWith ("status."))
                    continue;

                auto* c = findByName (main, n);
                expect (c != nullptr, "lobby control gone: " + n);
                expect (c == nullptr || ! isShowingWithinRoot (main, c), "lobby control still visible in session: " + n);
            }

            auto& session = main.getSession();
            expect (session.getState().sessionId == "DEMO42");
            expect (findByName (main, "session.link")->getName() == "session.link");
        }

        beginTest ("a second Crear sesion while the first join is pending tears the first attempt down");
        {
            MainComponent main;
            main.setVisible (true);
            main.setSize (1100, 760);
            auto& lobby = main.getLobby();
            lobby.nameEditor.setText ("Ana", juce::dontSendNotification);

            lobby.createButton.onClick();
            expect (! main.isInSession());
            expect (! lobby.createButton.isEnabled() && ! lobby.joinButton.isEnabled(), "buttons stay enabled while connecting");
            expect (statusText (main).contains ("Conectando"), "status: " + statusText (main));

            // What a queued click or an ensayo:// link arriving while the first join is pending does: the first host
            // and client attempt must be torn down (not replaced underneath the frame hook) and the second must win.
            lobby.createButton.onClick();
            expect (waitFor ([&] { return main.isInSession(); }, 5000), "embedded session never joined: " + statusText (main));
            expect (lobby.createButton.isEnabled() && lobby.joinButton.isEnabled(), "buttons not re-enabled after joined");
            expect (main.getSession().getState().participants.size() == 1);

            main.getSession().onLeave();
            expect (! main.isInSession());
            expect (lobby.createButton.isEnabled());
        }

        beginTest ("session view renders from a state alone");
        {
            SessionView view;
            view.setSize (1100, 600);
            SessionState s;
            s.connected = true; s.sessionId = "ABC234"; s.clientId = 1;
            s.participants = { { 1, "Yo", Role::both }, { 2, "Otro", Role::voice } };
            s.stats = { { 1, 4, 0, 0, 0.1f, 0.2f }, { 2, 3, 1, 0, 0.5f, 0.0f } };
            view.setState (s);
            auto* strip = findByName (view, "participant.1.name");
            expect (strip != nullptr && findByName (view, "participant.2.name") == nullptr);

            if (auto* fader = dynamic_cast<juce::Slider*> (findByName (view, "participant.1.instrument.fader")))
            {
                expect (! fader->isEnabled());
                expectEquals (fader->getMinimum(), -60.0);
                expectEquals (fader->getMaximum(), 12.0);
            }
            else
            {
                expect (false, "participant.1.instrument.fader is not a Slider");
            }

            if (auto* fader = dynamic_cast<juce::Slider*> (findByName (view, "participant.1.voice.fader")))
                expect (fader->isEnabled());
        }
    }
};

static UiTests uiTests;
}
