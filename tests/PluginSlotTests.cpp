#include "client/PluginSlot.h"

namespace ensayo
{
class PluginSlotTests : public juce::UnitTest
{
public:
    PluginSlotTests() : juce::UnitTest ("PluginSlot", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("findVst3Plugins lists files and bundles but not bundle internals");
        {
            const auto root = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                  .getNonexistentChildFile ("ensayo-vst3", {}, false);
            root.getChildFile ("Zeta.vst3").create();
            root.getChildFile ("Vendor/Beta.vst3/Contents/x86_64-win/Beta.vst3").create();
            root.getChildFile ("Alpha.vst3/Contents/x86_64-win/Alpha.vst3").create();
            root.getChildFile ("readme.txt").create();

            const auto found = findVst3Plugins (root);

            expectEquals (found.size(), 3);
            expectEquals (found[0].getFileName(), juce::String ("Alpha.vst3"));
            expectEquals (found[1].getFileName(), juce::String ("Beta.vst3"));
            expect (found[1].isDirectory());
            expectEquals (found[2].getFileName(), juce::String ("Zeta.vst3"));

            root.deleteRecursively();
        }

        beginTest ("findVst3Plugins on a missing folder returns nothing");
        expect (findVst3Plugins (juce::File ("C:\\ensayo-no-such-folder")).isEmpty());

        beginTest ("without a plugin the instrument passes dry to both channels");
        {
            PluginSlot slot;
            slot.prepare (48000.0, 8);

            juce::AudioBuffer<float> buffer (2, 8);
            buffer.clear();
            buffer.setSample (0, 3, 0.25f);

            slot.process (buffer, 8);

            expectEquals (buffer.getSample (0, 3), 0.25f);
            expectEquals (buffer.getSample (1, 3), 0.25f);
            expect (! slot.isLoaded());
        }

        beginTest ("loading a missing file fails and leaves the slot empty");
        {
            PluginSlot slot;
            const auto result = slot.load (juce::File ("C:\\ensayo-no-such-folder\\Missing.vst3"), 48000.0, 64);

            expect (result.failed());
            expect (! slot.isLoaded());
            expectEquals (slot.getStateBase64(), juce::String());
        }

        beginTest ("loading a missing file with an initial state still fails cleanly");
        {
            PluginSlot slot;
            const auto result = slot.load (juce::File ("C:\\ensayo-no-such-folder\\Missing.vst3"), 48000.0, 64, "AAECAw==");

            expect (result.failed());
            expect (! slot.isLoaded());
        }
    }
};

static PluginSlotTests pluginSlotTests;
}
