#pragma once

#include "CoreGlobals.h"
#include "Building/BuildingSMActor.h"

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
		static bool (*Function)(AFortPlayerController*, FBuildingClassData*) = decltype(Function)(InSDKUtils::GetImageBase() + 0xA66D8A0);

		return Function(this, ClassToBuildData);
	}

	int32 PayBuildableClassPlacementCost(FBuildingClassData* BuildingClassData)
	{
		static int32 (*Function)(AFortPlayerController*, FBuildingClassData*) = decltype(Function)(InSDKUtils::GetImageBase() + 0xA69F6D4);

		return Function(this, BuildingClassData);
	}

	ABuildingSMActor* ServerCreateBuildingActorInternal(FBuildingClassData* BuildingClassData, const FVector_NetQuantize10& BuildLoc, const FRotator& BuildRot, bool bMirrored, float SyncKey, 
		bool bForceOverrideInteractBuildCheck = false, bool bIgnoreCostCheck = false, bool bIgnoreBuildingValidityCheck = false);
	UE_DECLARE_CALL_SITE_OVERRIDE(ServerCreateBuildingActorInternal);

	void ServerCreateBuildingActor(FCreateBuildingActorData& CreateBuildingData);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerCreateBuildingActor);

	void ServerBeginEditingBuildingActor(ABuildingSMActor* BuildingActorToEdit);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerBeginEditingBuildingActor);

	void ServerEditBuildingActor(ABuildingSMActor* BuildingActorToEdit, TSubclassOf<ABuildingSMActor> NewBuildingClass, uint8 RotationIterations, bool bMirrored);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerEditBuildingActor);

	void ServerEndEditingBuildingActor(ABuildingSMActor* BuildingActorToStopEditing);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerEndEditingBuildingActor);

	static void Tick(float DeltaTime);
	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortPlayerControllerAthena);
UE_DECLARE_PRISM_TYPE(AFortPlayerControllerAthena);

PRISM_NAMESPACE_END