#include "ui/AudioDiagnosis.h"
#include "ui/Text.h"

namespace ensayo
{
namespace
{
    const ParticipantStats* findStats (const AudioDiagnosisInput& in, uint32_t id)
    {
        for (const auto& s : in.stats)
            if (s.id == id)
                return &s;

        return nullptr;
    }

    bool reported (const ParticipantStats& s) noexcept   { return s.packets != ~0u; }
}

juce::String diagnoseAudio (const AudioDiagnosisInput& in)
{
    using UdpFailure = SessionClient::UdpFailure;

    switch (in.udpFailure)
    {
        case UdpFailure::resolve: return ES ("No se pudo resolver la dirección del host: ") + in.host;
        case UdpFailure::socket:
        case UdpFailure::bind:    return ES ("No se pudo abrir el puerto UDP en esta PC (firewall/antivirus)");
        case UdpFailure::none:    break;
    }

    if (in.secondsInSession < audioDiagnosisGraceSeconds)
        return {};

    const auto* mine = findStats (in, in.clientId);
    const bool serverGetsMe = mine != nullptr && reported (*mine) && mine->packets > 0;

    if (mine != nullptr && reported (*mine) && mine->packets == 0)
    {
        if (mine->wrongFrame > 0)
            return ES ("El servidor descarta tu audio: tu buffer no coincide con el de la sesión");

        return ES ("El host no recibe tu audio: el UDP a ") + in.host + ":" + juce::String (in.port)
               + ES (" no llega (firewall/antivirus)");
    }

    if (in.mixPacketsReceived == 0)
    {
        if (serverGetsMe)
            return ES ("El host recibe tu audio pero el suyo no llega a esta PC (firewall/antivirus de esta PC)");

        return "Sin audio del servidor";
    }

    for (const auto& p : in.participants)
    {
        if (p.id == in.clientId)
            continue;

        const auto joined = in.secondsSinceJoined.find (p.id);

        if (joined == in.secondsSinceJoined.end() || joined->second < audioDiagnosisGraceSeconds)
            continue;

        if (const auto* s = findStats (in, p.id); s != nullptr && reported (*s) && s->packets == 0)
        {
            if (s->wrongFrame > 0)
                return p.name + ES (" usa otro tamaño de buffer: el servidor descarta su audio");

            return ES ("No llega el audio de ") + p.name + ES (": su UDP no alcanza al servidor (firewall/antivirus en su PC)");
        }
    }

    if (in.secondsSinceLastMix > audioDiagnosisOutageSeconds)
        return "Sin audio del servidor";

    return {};
}
}
