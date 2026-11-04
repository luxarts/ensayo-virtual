#include "server/ServerHost.h"

#include <juce_events/juce_events.h>

#ifndef WIN32_LEAN_AND_MEAN
 #define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
 #define NOMINMAX
#endif
#include <windows.h>

#include <atomic>
#include <iostream>
#include <string>
#include <thread>

namespace
{
std::atomic<bool> shouldQuit { false };

BOOL WINAPI consoleHandler (DWORD type)
{
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
    {
        shouldQuit.store (true);
        return TRUE;
    }

    return FALSE;
}

void printUsage()
{
    std::cout << "EnsayoServer [--port N] [--udp-port N] [--frame 64|128] [--log]\n"
              << "  --port N      puerto TCP de control (y UDP si no se indica --udp-port), por defecto "
              << ensayo::defaultPort << "\n"
              << "  --udp-port N  puerto UDP de audio\n"
              << "  --frame N     tamano de bloque en muestras: 64 o 128 (por defecto 128)\n"
              << "  --log         imprime eventos del servidor\n";
}
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ensayo::ServerConfig config;
    bool log = false;
    bool udpGiven = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg (argv[i]);
        const bool hasValue = i + 1 < argc;

        if (arg == "--port" && hasValue)
        {
            config.tcpPort = std::atoi (argv[++i]);

            if (! udpGiven)
                config.udpPort = config.tcpPort;
        }
        else if (arg == "--udp-port" && hasValue)
        {
            config.udpPort = std::atoi (argv[++i]);
            udpGiven = true;
        }
        else if (arg == "--frame" && hasValue)
        {
            const auto size = ensayo::frameSizeForSamples (std::atoi (argv[++i]));

            if (! size)
            {
                std::cerr << "--frame debe ser 64 o 128\n";
                return 2;
            }

            config.frameSize = *size;
        }
        else if (arg == "--log")
        {
            log = true;
        }
        else if (arg == "--help" || arg == "-h")
        {
            printUsage();
            return 0;
        }
        else
        {
            std::cerr << "Argumento desconocido: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    if (config.tcpPort <= 0 || config.tcpPort > 65535 || config.udpPort < 0 || config.udpPort > 65535)
    {
        std::cerr << "Puerto invalido\n";
        return 2;
    }

    ensayo::ServerHost host (config);

    if (log)
        host.onLog = [] (const juce::String& line)
        {
            std::cout << line.toStdString() << std::endl;
        };

    const auto result = host.start();

    if (result.failed())
    {
        std::cerr << result.getErrorMessage().toStdString() << std::endl;
        return 1;
    }

    host.startInternalClock();

    std::cout << "listening tcp=" << config.tcpPort << " udp=" << host.getUdpPort() << std::endl;
    std::cout.flush();

    SetConsoleCtrlHandler (consoleHandler, TRUE);

    // Stdin EOF also stops the server, so a parent process can end it by closing the pipe.
    std::thread stdinThread ([]
    {
        std::string line;

        while (std::getline (std::cin, line))
        {
        }

        shouldQuit.store (true);
    });
    stdinThread.detach();

    while (! shouldQuit.load())
        std::this_thread::sleep_for (std::chrono::milliseconds (100));

    host.stop();
    std::cout << "stopped" << std::endl;
    return 0;
}
