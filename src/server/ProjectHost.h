#pragma once
#include "core/Project.h"
#include <functional>
#include <memory>
namespace ensayo
{
constexpr int protocolVersionV2 = 2;

struct HostConfig
{
    int port = defaultPort;            // TCP (control + data) and UDP (live-audio relay)
    int maxClients = 8;
};

juce::File defaultProjectsDirectory();                                   // %APPDATA%\EnsayoVirtual\projects
juce::Array<juce::File> listProjects (const juce::File& projectsDirectory);   // dirs with project.json, newest first
juce::Result saveProjectAtomically (const Project&, const juce::File& projectJson);   // temp file + replace

class ProjectHost
{
public:
    explicit ProjectHost (HostConfig);
    ~ProjectHost();
    // Opens projectDir/project.json, or creates a new project there named newProjectName when missing.
    // Registers the host user, picks a session id, binds TCP + UDP and starts the threads.
    juce::Result start (const juce::File& projectDir, const Id& hostUser, const juce::String& hostName,
                        const juce::String& newProjectName);
    void stop();
    juce::String getSessionId() const;
    int getPort() const noexcept;
    std::shared_ptr<const Project> snapshot() const;
    std::function<void (const juce::String&)> onLog;   // any thread

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
}
