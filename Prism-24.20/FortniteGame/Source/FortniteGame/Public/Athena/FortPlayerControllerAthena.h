#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class AFortPlayerControllerAthena : public UE_EXTENDS_SDK_CLASS(AFortPlayerControllerAthena)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(AFortPlayerControllerAthena);

	void ServerAcknowledgePossession(APawn* P);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerAcknowledgePossession);

	void ServerExecuteInventoryItem(const FGuid& ItemGuid);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerExecuteInventoryItem);

	bool CanAffordToPlaceBuildableClass(FBuildingClassData* ClassToBuildData)
	{
		static void (*Function)(AFortPlayerController*, FBuildingClassData*) = decltype(Function)(InSDKUtils::GetImageBase() + 0xA66D8A0);

		Function(this, ClassToBuildData);
	}

	ABuildingSMActor* ServerCreateBuildingActorInternal(FBuildingClassData* BuildingClassData, const FVector_NetQuantize10& BuildLoc, const FRotator& BuildRot, bool bMirrored, bool SyncKey, bool bForceOverrideInteractBuildCheck, bool bIgnoreCostCheck, bool bIgnoreBuildingValidityCheck);

	static void Tick(float DeltaTime);
	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortPlayerControllerAthena);
UE_DECLARE_PRISM_TYPE(AFortPlayerControllerAthena);

PRISM_NAMESPACE_END