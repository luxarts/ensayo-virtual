#include "client/SessionLink.h"

#include <juce_core/juce_core.h>

namespace ensayo
{
class SessionLinkTests : public juce::UnitTest
{
public:
    SessionLinkTests() : juce::UnitTest ("SessionLink", "EnsayoVirtual") {}

    void runTest() override
    {
        beginTest ("parse with scheme");
        {
            const auto link = parseSessionLink ("ensayo://1.2.3.4:22140/ab12cd");
            expect (link.has_value());

            if (link.has_value())
            {
                expectEquals (link->host, juce::String ("1.2.3.4"));
                expectEquals (link->port, 22140);
                expectEquals (link->sessionId, juce::String ("AB12CD"));
            }
        }

        beginTest ("parse without scheme, trimmed");
        {
            const auto link = parseSessionLink ("  host.name:22140/ABCDEF \n");
            expect (link.has_value());

            if (link.has_value())
            {
                expectEquals (link->host, juce::String ("host.name"));
                expectEquals (link->port, 22140);
                expectEquals (link->sessionId, juce::String ("ABCDEF"));
            }
        }

        beginTest ("rejects malformed links");
        {
            expect (! parseSessionLink ("ensayo://1.2.3.4/ABCDEF").has_value(), "missing port accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:22140/ABC").has_value(), "short id accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:22140/ABCDEFG").has_value(), "long id accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:22140/ABC-EF").has_value(), "id with punctuation accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:22140/").has_value(), "empty id accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:0/ABCDEF").has_value(), "port 0 accepted");
            expect (! parseSessionLink ("ensayo://1.2.3.4:70000/ABCDEF").has_value(), "port 70000 accepted");
            expect (! parseSessionLink ("ensayo://:22140/ABCDEF").has_value(), "empty host accepted");
            expect (! parseSessionLink ("").has_value(), "empty string accepted");
            expect (! parseSessionLink ("http://1.2.3.4:22140/ABCDEF").has_value(), "other scheme accepted");
        }

        beginTest ("rejects IPv6 hosts (the UDP path is IPv4 only)");
        {
            expect (! parseSessionLink ("ensayo://[::1]:22140/ABCDEF").has_value(), "[::1] accepted");
            expect (! parseSessionLink ("[::1]:22140/ABCDEF").has_value(), "[::1] without scheme accepted");
            expect (! parseSessionLink ("ensayo://[2001:db8::1]:22140/ABCDEF").has_value(), "[2001:db8::1] accepted");
            expect (! parseSessionLink ("ensayo://::1:22140/ABCDEF").has_value(), "bare ::1 accepted");
            expect (parseSessionLink ("ensayo://host-name_1.example.com:22140/ABCDEF").has_value(), "plain host name rejected");
        }

        beginTest ("toString round trip");
        {
            SessionLink link;
            link.host = "example.com";
            link.port = 22141;
            link.sessionId = "QWERTY";

            expectEquals (link.toString(), juce::String ("ensayo://example.com:22141/QWERTY"));

            const auto parsed = parseSessionLink (link.toString());
            expect (parsed.has_value());

            if (parsed.has_value())
                expectEquals (parsed->toString(), link.toString());
        }

        beginTest ("linkFromCommandLine");
        {
            juce::StringArray args { "EnsayoVirtual.exe", "--demo", "ensayo://10.0.0.5:22140/abcdef" };
            const auto link = linkFromCommandLine (args);
            expect (link.has_value());

            if (link.has_value())
            {
                expectEquals (link->host, juce::String ("10.0.0.5"));
                expectEquals (link->sessionId, juce::String ("ABCDEF"));
            }

            juce::StringArray quoted { "\"ensayo://10.0.0.5:22140/abcdef\"" };
            expect (linkFromCommandLine (quoted).has_value(), "quoted argument rejected");

            juce::StringArray none { "EnsayoVirtual.exe", "--headless" };
            expect (! linkFromCommandLine (none).has_value());

            expect (! linkFromCommandLine ({}).has_value());
        }
    }
};

static SessionLinkTests sessionLinkTests;
}
