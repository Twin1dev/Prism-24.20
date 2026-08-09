#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class ABuildingContainer : public UE_EXTENDS_SDK_CLASS(ABuildingContainer)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(ABuildingContainer)

	FVector DetermineSpawnLocationAthena(AFortPlayerPawn * Pawn = nullptr); // Not a real function i just feel special naming it this

	bool SpawnLoot(AFortPlayerPawn* FortPlayerPawn, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource);
	UE_DECLARE_NATIVE_OVERRIDE(SpawnLoot);

	void PostUpdate(EFortBuildingPersistentState PersistentState, __int64 ReservedRandomValues);
	UE_DECLARE_NATIVE_OVERRIDE(PostUpdate);
public:
	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(ABuildingContainer);
UE_DECLARE_PRISM_TYPE(ABuildingContainer);

PRISM_NAMESPACE_END