#pragma once
#include <juce_core/juce_core.h>
#include <optional>
namespace ensayo
{
struct SessionLink { juce::String host; int port = 0; juce::String sessionId;
                     juce::String toString() const; };               // "ensayo://host:port/ID"
std::optional<SessionLink> parseSessionLink (const juce::String&);   // accepts with/without scheme, trims, uppercases ID
bool registerUrlScheme (const juce::File& exe);                      // HKCU\Software\Classes\ensayo (...)
std::optional<SessionLink> linkFromCommandLine (const juce::StringArray& args);
}
