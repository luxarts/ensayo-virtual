#include <juce_gui_extra/juce_gui_extra.h>

namespace ensayo
{
class EnsayoVirtualApplication : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override       { return "Ensayo Virtual"; }
    const juce::String getApplicationVersion() override    { return "0.1.0"; }
    bool moreThanOneInstanceAllowed() override             { return false; }

    void initialise (const juce::String&) override         { mainWindow = std::make_unique<MainWindow>(); }
    void shutdown() override                               { mainWindow = nullptr; }
    void systemRequestedQuit() override                    { quit(); }

private:
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
            setContentOwned (new juce::Component(), true);
            setResizable (true, true);
            centreWithSize (400, 300);
            setVisible (true);
        }

        void closeButtonPressed() override { juce::JUCEApplication::getInstance()->systemRequestedQuit(); }
    };

    std::unique_ptr<MainWindow> mainWindow;
};
}

START_JUCE_APPLICATION (ensayo::EnsayoVirtualApplication)
