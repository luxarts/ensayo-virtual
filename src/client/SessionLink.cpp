#include "client/SessionLink.h"

namespace ensayo
{
namespace
{
    const juce::String scheme = "ensayo";

    // The link only needs the shape of an id (six letters or digits); the server validates the real alphabet.
    bool looksLikeSessionId (const juce::String& id)
    {
        return id.length() == 6 && id.containsOnly ("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789");
    }

    // Host names and IPv4 literals only: the UDP resolver is AF_INET, so an IPv6 literal ("[::1]") would join over
    // TCP and then never receive audio.
    bool isValidHost (const juce::String& host)
    {
        if (host.isEmpty() || host.length() > 253)
            return false;

        for (const auto c : host)
            if (! (juce::CharacterFunctions::isLetterOrDigit (c) || c == '.' || c == '-' || c == '_'))
                return false;

        return true;
    }
}

juce::String SessionLink::toString() const
{
    return scheme + "://" + host + ":" + juce::String (port) + "/" + sessionId;
}

std::optional<SessionLink> parseSessionLink (const juce::String& input)
{
    auto text = input.trim().unquoted().trim();

    // Strip the scheme when present; reject any other scheme.
    const auto schemeSeparator = text.indexOf ("://");

    if (schemeSeparator >= 0)
    {
        if (! text.substring (0, schemeSeparator).equalsIgnoreCase (scheme))
            return std::nullopt;

        text = text.substring (schemeSeparator + 3);
    }
    else if (text.startsWithIgnoreCase (scheme + ":"))
    {
        text = text.substring (scheme.length() + 1);
    }

    text = text.trimCharactersAtEnd ("/");

    const auto slash = text.indexOfChar ('/');

    if (slash < 0)
        return std::nullopt;

    const auto hostPort = text.substring (0, slash).trim();
    const auto id = text.substring (slash + 1).trim().toUpperCase();

    const auto colon = hostPort.lastIndexOfChar (':');

    if (colon <= 0)
        return std::nullopt;

    const auto host = hostPort.substring (0, colon).trim();
    const auto portText = hostPort.substring (colon + 1).trim();

    if (portText.isEmpty() || ! portText.containsOnly ("0123456789"))
        return std::nullopt;

    const int port = portText.getIntValue();

    if (port < 1 || port > 65535)
        return std::nullopt;

    if (! isValidHost (host) || ! looksLikeSessionId (id))
        return std::nullopt;

    SessionLink link;
    link.host = host;
    link.port = port;
    link.sessionId = id;
    return link;
}

bool registerUrlScheme (const juce::File& exe)
{
   #if JUCE_WINDOWS
    if (! exe.existsAsFile())
        return false;

    const juce::String root = "HKEY_CURRENT_USER\\Software\\Classes\\" + scheme + "\\";
    const auto command = exe.getFullPathName().quoted() + " \"%1\"";

    bool ok = true;
    ok = juce::WindowsRegistry::setValue (root, "URL:Ensayo Virtual") && ok;
    ok = juce::WindowsRegistry::setValue (root + "URL Protocol", "") && ok;
    ok = juce::WindowsRegistry::setValue (root + "DefaultIcon\\", exe.getFullPathName().quoted() + ",0") && ok;
    ok = juce::WindowsRegistry::setValue (root + "shell\\open\\command\\", command) && ok;
    return ok;
   #else
    juce::ignoreUnused (exe);
    return false;
   #endif
}

std::optional<SessionLink> linkFromCommandLine (const juce::StringArray& args)
{
    for (const auto& arg : args)
    {
        const auto text = arg.trim().unquoted().trim();

        if (text.startsWithIgnoreCase (scheme + ":"))
            if (auto link = parseSessionLink (text))
                return link;
    }

    return std::nullopt;
}
}
