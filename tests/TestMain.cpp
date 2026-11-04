#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <iostream>

namespace
{
// UnitTest::logMessage goes through the JUCE logger, which only reaches the debugger by default.
class StdoutLogger : public juce::Logger
{
    void logMessage (const juce::String& message) override { std::cout << message << std::endl; }
};
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;
    StdoutLogger logger;
    juce::Logger::setCurrentLogger (&logger);

    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);

    if (argc > 1)
    {
        const juce::String wanted (argv[1]);
        juce::Array<juce::UnitTest*> selected;

        for (auto* test : juce::UnitTest::getAllTests())
            if (test->getCategory() == "EnsayoVirtual" && test->getName().containsIgnoreCase (wanted))
                selected.add (test);

        runner.runTests (selected);
    }
    else
    {
        runner.runTestsInCategory ("EnsayoVirtual");
    }

    int failures = 0;

    for (int i = 0; i < runner.getNumResults(); ++i)
    {
        const auto* result = runner.getResult (i);

        std::cout << result->unitTestName.toStdString() << " / "
                  << result->subcategoryName.toStdString() << ": "
                  << result->passes << " passed, "
                  << result->failures << " failed" << std::endl;

        failures += result->failures;
    }

    std::cout << "TOTAL: " << runner.getNumResults() << " results, "
              << failures << " failures" << std::endl;

    juce::Logger::setCurrentLogger (nullptr);
    return (runner.getNumResults() == 0 || failures > 0) ? 1 : 0;
}
