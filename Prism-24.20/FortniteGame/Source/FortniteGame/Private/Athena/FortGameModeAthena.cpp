#include "pch.h"

#include "Athena/FortGameModeAthena.h"

PRISM_NAMESPACE_START

bool AFortGameModeAthena::ReadyToStartMatch()
{
    if (!bWorldIsReady)
    {


        bWorldIsReady = true;
    }

    return AlivePlayers.Num() > 0;
}

void AFortGameModeAthena::Initialize()
{
    ReadyToStartMatch_Override.Install("FortGameModeBR");
}

IMPLEMENT_MODULE(AFortGameModeAthena);

PRISM_NAMESPACE_END
