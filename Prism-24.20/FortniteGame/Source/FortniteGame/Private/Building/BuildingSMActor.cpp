#include "pch.h"

#include "Building/BuildingSMActor.h"
#include <Templates/Casts.h>
#include "Items/FortInventory.h"
#include "UObject/ClassHierarchyOverride.h"

PRISM_NAMESPACE_START

void ABuildingSMActor::AttemptSpawnResources(const FBuildingSMActorAttemptSpawnResourcesParams& Params)
{
	if (!Params.InstigatorPawn)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": no InstigatorPawn, skipping (%s)", GetName().c_str());
		return;
	}

	AFortPlayerController* Controller = Cast<AFortPlayerController>(Params.InstigatorPawn->Controller);

	if (!Controller)
	{
		UE_LOG(LogPrism, Log, __FUNCTION__": InstigatorPawn %s has no AFortPlayerController, skipping", Params.InstigatorPawn->GetName().c_str());
		return;
	}

	float MaxHealth = GetMaxHealth();

	if (MaxHealth <= 0.0f)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": %s has MaxHealth <= 0, skipping", GetName().c_str());
		return;
	}

	UFortPlaylistAthena* Playlist = UWorld::GetWorld()->GameState->CurrentPlaylistInfo.BasePlaylist;
	UCurveTable* ResourceRates = Playlist->ResourceRates.LoadSynchronous();

	if (!ResourceRates)
	{
		ResourceRates = BuildingResourceAmountOverride.CurveTable;
	}

	if (!ResourceRates)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": No resource rates");
		return;
	}

	EEvaluateCurveTableResult CurveTableResult;
	float OutValue = 0.0f;
	UDataTableFunctionLibrary::EvaluateCurveTableRow(ResourceRates, BuildingResourceAmountOverride.RowName, 0, &CurveTableResult, &OutValue, FString());

	if (CurveTableResult != EEvaluateCurveTableResult::RowFound)
		UE_LOG(LogPrism, Warning, __FUNCTION__": EvaluateCurveTableRow for row %s did not find a row", BuildingResourceAmountOverride.RowName);

	int32 ResourceCount = FMath::RoundToInt(OutValue * (Params.ActualDamageDealt / MaxHealth));

	if (ResourceCount <= 0)
	{
		UE_LOG(LogPrism, Log, __FUNCTION__": computed ResourceCount <= 0 (OutValue=%f, ActualDamageDealt=%f, MaxHealth=%f), skipping", OutValue, Params.ActualDamageDealt, MaxHealth);
		return;
	}

	UFortItemDefinition* ResourceDefinition = UFortKismetLibrary::K2_GetResourceItemDefinition(ResourceType);

	if (!ResourceDefinition)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": K2_GetResourceItemDefinition returned null for ResourceType %d, skipping", (int32)ResourceType);
		return;
	}

	ToPrism(Controller->WorldInventory)->AddItem(ResourceDefinition, ResourceCount);
	Controller->ClientReportDamagedResourceBuilding(this, ResourceType, ResourceCount, GetHealth() - Params.ActualDamageDealt <= 0, Params.bJustHitWeakspot);
}

void ABuildingSMActor::Initialize()
{
	/*
	 * Every native class under BuildingSMActor carries its own copy of the virtual function
	 * table, so hooking the base class alone leaves BuildingContainer, BuildingProp, every
	 * trap and every prop subclass still pointing at the original implementation.
	 *
	 * The loop this replaces built one FAttemptSpawnResourcesScriptOverride per default
	 * object but stored a *reference* to it, and let the object die at the end of the
	 * iteration - so every entry referred to the same dead stack slot. Combined with
	 * FVirtualFunctionOverride::Install refusing to run a second time on one object, only the
	 * first table was ever patched. InstallVirtualOverrideForHierarchy gives each table its
	 * own override object, skips the tables Blueprint classes share with their native parent,
	 * and refuses to hook a subclass that carries its own implementation in the slot.
	 */
	UObjectHierarchy::InstallVirtualOverrideForHierarchy(
		SDK::ABuildingSMActor::StaticClass(),
		0x18F,
		&ThisClass::AttemptSpawnResources,
		&AttemptSpawnResources_Original,
		"ABuildingSMActor::AttemptSpawnResources");
}

IMPLEMENT_MODULE(ABuildingSMActor);

PRISM_NAMESPACE_END


