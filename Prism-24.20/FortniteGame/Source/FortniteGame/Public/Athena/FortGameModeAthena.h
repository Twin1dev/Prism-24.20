#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class AFortGameModeAthena : public UE_EXTENDS_SDK_CLASS(AFortGameModeAthena)
{
public:
    UE_FUNCTION_OVERRIDE_BODY(AFortGameModeAthena)

    bool ReadyToStartMatch();
    UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ReadyToStartMatch);

    APawn* SpawnDefaultPawnFor(AController* NewPlayer, AActor* StartSpot);
    UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(SpawnDefaultPawnFor);

    void SpawnInitialSafeZone();
	UE_DECLARE_NATIVE_OVERRIDE(SpawnInitialSafeZone);

    void UpdateSafeZonesPhase();
	UE_DECLARE_NATIVE_OVERRIDE(UpdateSafeZonesPhase);

    static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortGameModeAthena);
UE_DECLARE_PRISM_TYPE(AFortGameModeAthena);

PRISM_NAMESPACE_END