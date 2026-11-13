#include "server/ProjectHost.h"

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
    std::cout << "EnsayoServer --project <carpeta> [--port N] [--name <proyecto>] [--user <id>] [--user-name <nombre>] [--log]\n"
              << "  --project D     carpeta del proyecto (project.json + takes); se crea si no existe\n"
              << "  --port N        puerto TCP (control y datos) y UDP (escucha en vivo), por defecto " << ensayo::defaultPort << "\n"
              << "  --name P        nombre de un proyecto nuevo (por defecto, el nombre de la carpeta)\n"
              << "  --user ID       id del usuario anfitrion (32 hex); por defecto el del proyecto, o uno nuevo.\n"
              << "                  Un cliente con ese id solo puede conectarse desde 127.0.0.1.\n"
              << "  --user-name N   nombre del anfitrion (por defecto el guardado en el proyecto, o \"Anfitrion\")\n"
              << "  --log           imprime eventos del servidor\n"
              << "Imprime \"listening port=N session=ID\" y termina al cerrarse la entrada estandar (o con Ctrl+C).\n";
}

juce::String fromArg (const char* text)   { return juce::String (juce::CharPointer_UTF8 (text)); }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI juceInitialiser;

    ensayo::HostConfig config;
    juce::File projectDir;
    juce::String projectName, user, userName;
    bool log = false;

    for (int i = 1; i < argc; ++i)
    {
        const std::string arg (argv[i]);
        const bool hasValue = i + 1 < argc;

        if (arg == "--project" && hasValue)
        {
            const auto path = fromArg (argv[++i]).unquoted();
            projectDir = juce::File::isAbsolutePath (path) ? juce::File (path) : juce::File::getCurrentWorkingDirectory().getChildFile (path);
        }
        else if (arg == "--port" && hasValue)
        {
            config.port = std::atoi (argv[++i]);
        }
        else if (arg == "--name" && hasValue)
        {
            projectName = fromArg (argv[++i]);
        }
        else if (arg == "--user" && hasValue)
        {
            user = fromArg (argv[++i]).trim().toLowerCase();
        }
        else if (arg == "--user-name" && hasValue)
        {
            userName = fromArg (argv[++i]).trim();
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
            std::cerr << "Argumento desconocido o sin valor: " << arg << "\n";
            printUsage();
            return 2;
        }
    }

    if (projectDir == juce::File())
    {
        std::cerr << "Falta --project <carpeta>\n";
        printUsage();
        return 2;
    }

    if (config.port <= 0 || config.port > 65535)
    {
        std::cerr << "Puerto invalido\n";
        return 2;
    }

    if (user.isNotEmpty() && ! ensayo::isValidId (user))
    {
        std::cerr << "--user debe ser un id de 32 caracteres hexadecimales\n";
        return 2;
    }

    // Defaults come from an existing project: its host user (a different one would be refused) and that user's name.
    if (const auto existing = projectDir.getChildFile ("project.json"); existing.existsAsFile())
    {
        juce::String error;

        if (const auto project = ensayo::projectFromJson (existing.loadFileAsString(), error))
        {
            if (user.isEmpty())
                user = project->hostUser;

            if (userName.isEmpty())
                userName = project->userName (project->hostUser);
        }
    }

    if (user.isEmpty())
        user = ensayo::newId();

    if (userName.isEmpty())
        userName = juce::String (juce::CharPointer_UTF8 ("Anfitri\xc3\xb3n"));

    if (projectName.isEmpty())
        projectName = projectDir.getFileName();

    ensayo::ProjectHost host (config);

    if (log)
        host.onLog = [] (const juce::String& line)
        {
            std::cout << line.toStdString() << std::endl;
        };

    const auto result = host.start (projectDir, user, userName, projectName);

    if (result.failed())
    {
        std::cerr << result.getErrorMessage().toStdString() << std::endl;
        return 1;
    }

    std::cout << "listening port=" << host.getPort() << " session=" << host.getSessionId().toStdString() << std::endl;
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
