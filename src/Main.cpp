#include "ui/MainComponent.h"
#include "ui/Screenshot.h"
#include "client/HeadlessClient.h"
#include "client/SessionLink.h"
#include "client/Settings.h"
#include <juce_gui_extra/juce_gui_extra.h>

namespace ensayo
{
class EnsayoVirtualApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "Ensayo Virtual"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    // Headless instances (tests, latency probes) must be able to run side by side on one machine.
    bool moreThanOneInstanceAllowed() override             { return getCommandLineParameterArray().contains ("--headless"); }

    void initialise (const juce::String& commandLine) override
    {
        auto args = getCommandLineParameterArray();

        if (args.isEmpty())
            args = tokenise (commandLine);


        if (args.contains ("--headless"))
        {
            juce::String error;
            const auto options = parseHeadlessOptions (args, error);

            if (! options)
                headlessPrint ("error " + error);

            setApplicationReturnValue (options ? runHeadless (*options) : 2);
            quit();
            return;
        }

        if (const int index = args.indexOf ("--screenshot"); index >= 0)
        {
            const auto dir = index + 1 < args.size() ? juce::File::getCurrentWorkingDirectory().getChildFile (args[index + 1])
                                                     : juce::File::getCurrentWorkingDirectory().getChildFile ("shots");
            const auto result = runScreenshots (dir);

            if (result.failed())
                headlessPrint ("error " + result.getErrorMessage());

            setApplicationReturnValue (result.wasOk() ? 0 : 1);
            quit();
            return;
        }

        const bool firstRun = ! defaultSettingsFile().existsAsFile();
        mainWindow = std::make_unique<MainWindow>();

        if (firstRun)
            registerUrlScheme (juce::File::getSpecialLocation (juce::File::currentExecutableFile));   // best effort

        if (args.contains ("--demo"))
            mainWindow->getMain().showDemoSession();
        else if (const auto link = linkFromCommandLine (args))
            mainWindow->getMain().joinFromLink (*link);
    }

    void shutdown() override                               { mainWindow = nullptr; }
    void systemRequestedQuit() override                    { quit(); }

    void anotherInstanceStarted (const juce::String& commandLine) override
    {
        if (mainWindow == nullptr)
            return;

        if (const auto link = linkFromCommandLine (tokenise (commandLine)))
        {
            mainWindow->getMain().joinFromLink (*link);
            mainWindow->toFront (true);
        }
    }

private:
    static juce::StringArray tokenise (const juce::String& commandLine)
    {
        auto args = juce::StringArray::fromTokens (commandLine, " \t", "\"");
        args.removeEmptyStrings();

        for (auto& a : args)
            a = a.unquoted();

        return args;
    }

    class MainWindow : public juce::DocumentWindow
    {
    public:
        MainWindow()
            : DocumentWindow ("Ensayo Virtual",
                              juce::Desktop::getInstance().getDefaultLookAndFeel()
                                  .findColour (juce::ResizableWindow::backgroundColourId),
                              DocumentWindow::allButtons)
        {
            setUsingNativeTitleBar (true);
            main = new MainComponent();
            setContentOwned (main, true);
            setResizable (true, false);
            setResizeLimits (1100, 760, 8192, 8192);
            centreWithSize (1100, 760);
            setVisible (true);
        }

        MainComponent& getMain() { return *main; }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }

    private:
        MainComponent* main = nullptr;
    };

    std::unique_ptr<MainWindow> mainWindow;
};
}

START_JUCE_APPLICATION (ensayo::EnsayoVirtualApplication)
