#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class AFortGameModeAthena : public UE_EXTENDS_SDK_CLASS(AFortGameModeAthena)
{
public:
    UE_FUNCTION_OVERRIDE_BODY(AFortGameModeAthena)

    bool ReadyToStartMatch();
    UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ReadyToStartMatch);

    static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortGameModeAthena);

PRISM_NAMESPACE_END