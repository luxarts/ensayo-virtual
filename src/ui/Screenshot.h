#pragma once
#include <juce_gui_basics/juce_gui_basics.h>
namespace ensayo
{
// Renders lobby.png and session.png (the DAW view with the demo project) at 1100x760 (SoftwareImageType) and controls.json:
// [{"name":"lobby.name","type":"TextEditor","x":..,"y":..,"w":..,"h":..,"visible":true,"view":"lobby"}, ...]
juce::Result runScreenshots (const juce::File& outDir);
}
