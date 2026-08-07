#include "pch.h"

#include "Athena/FortPlayerControllerAthena.h"
#include <Templates/Casts.h>
#include "Items/FortInventory.h"

PRISM_NAMESPACE_START

void AFortPlayerControllerAthena::ServerAcknowledgePossession(APawn* P)
{
	static auto ServerAcknowledgePossessionFortPC = InSDKUtils::GetVirtualFunction<void(*)(AFortPlayerController*, APawn*)>(AFortPlayerController::GetDefaultObj(), 0x130);

	ServerAcknowledgePossessionFortPC(this, P);

	AFortPlayerStateAthena* PlayerStateAthena = Cast<AFortPlayerStateAthena>(PlayerState);

	if (!PlayerStateAthena)
		return;

	UFortKismetLibrary::UpdatePlayerCustomCharacterPartsVisualization(PlayerStateAthena);

	MyFortPawn->ResetGameplayAttributes(true);

	static void (*UFortGlobals__InitializeGameplayAbilities)(IFortAbilitySystemInterface*) = decltype(UFortGlobals__InitializeGameplayAbilities)(InSDKUtils::GetImageBase() + 0xA0B5F3C);
	UFortGlobals__InitializeGameplayAbilities(PlayerStateAthena->GetInterfaceAddress<IFortAbilitySystemInterface>());

	static void (*AFortGameStateZone__OnAbilitySystemInterfaceReadyForPersistentGameplayEffects)(AFortGameStateZone * __this, IFortAbilitySystemInterface * InInterface) = decltype(AFortGameStateZone__OnAbilitySystemInterfaceReadyForPersistentGameplayEffects)(InSDKUtils::GetImageBase() + 0x25573D8);
	AFortGameStateZone__OnAbilitySystemInterfaceReadyForPersistentGameplayEffects(Cast<AFortGameStateAthena>(UWorld::GetWorld()->GameState), MyFortPawn->GetInterfaceAddress<IFortAbilitySystemInterface>());

	for (auto AbilitySpec : PlayerStateAthena->AbilitySystemComponent->ActivatableAbilities.Items)
	{ 
		UE_LOG(LogPrism, Log, "Ability %s", AbilitySpec.Ability->GetName().c_str());
	}
}

void AFortPlayerControllerAthena::ServerExecuteInventoryItem(const FGuid& ItemGuid)
{
	AFortInventory* Inventory = ToPrism(WorldInventory);
	if (!Inventory)
		return;

	FFortItemEntry* Entry = Inventory->FindItemEntryByGuid(ItemGuid);
	if (!Entry || !Entry->IsValidItemEntry())
		return;

	if (UFortWeaponItemDefinition* WeaponDefinition = Cast<UFortWeaponItemDefinition>(Entry->ItemDefinition))
	{
		if (MyFortPawn)
		{
			MyFortPawn->EquipWeaponDefinition(WeaponDefinition, Entry->ItemGuid, FGuid(), false);
		}
	}
}

/*
 * GetPlayerViewPoint used to be detoured here, over the game's own native, so that the
 * Replication Graph could get a view point out of a connection. It does not need one:
 * Prism::APlayerController::GetPlayerViewPoint reproduces the engine's chain from reflected
 * members, which is both cheaper (no ProcessEvent per call) and correct for a controller with
 * no pawn, where the version that lived here left its outputs untouched.
 *
 * The detour is gone rather than fixed in place because it was a global one: every caller in
 * the game got "the view target's eyes" in place of Fortnite's own implementation, for the
 * benefit of one caller in this module. Game code sees the shipped behaviour again.
 */

ABuildingSMActor* AFortPlayerControllerAthena::ServerCreateBuildingActorInternal(FBuildingClassData* BuildingClassData, const FVector_NetQuantize10& BuildLoc, const FRotator& BuildRot, bool bMirrored, bool SyncKey, bool bForceOverrideInteractBuildCheck, bool bIgnoreCostCheck, bool bIgnoreBuildingValidityCheck)
{
	if (!MyFortPawn || MyFortPawn->bIsDBNO)
		return nullptr;

	AFortGameStateAthena* GameState = UWorld::GetWorld()->GameState;
	UWorld* World = UWorld::GetWorld();

	if (!GameState->StructuralSupportSystem)
	{
		UE_LOG(LogPrism, Log, "No StructuralSupportSystem!");

		return nullptr;
	}

	TArray<ABuildingActor*> BuildingActors;
	ABuildingSMActor* BuildingSMActor = nullptr;

	if (TSubclassOf<ABuildingActor> BuildingClass = BuildingClassData->BuildingClass)
	{
		EFortBuildPreviewMarkerOptionalAdjustment Adjustment;
		EFortStructuralGridQueryResults QueryResult = GameState->StructuralSupportSystem->CanAddBuildingActorClassToGrid(World, BuildingClass, BuildLoc, BuildRot, bMirrored, &BuildingActors, &Adjustment, false);

		if (QueryResult == EFortStructuralGridQueryResults::CanAdd || bIgnoreBuildingValidityCheck)
		{
			for (ABuildingActor* BuildingActor : BuildingActors)
			{
				if (!BuildingActor)
					continue;

				BuildingActor->K2_DestroyActor();
			}

			if (!CanAffordToPlaceBuildableClass(BuildingClassData))
			{
				return nullptr;
			}


		}
	}

	return nullptr;
}

void AFortPlayerControllerAthena::Tick(float DeltaTime)
{
	AFortGameModeAthena* GameMode = Cast<AFortGameModeAthena>(UWorld::GetWorld()->AuthorityGameMode);

	if (!GameMode)
		return;

	for (SDK::AFortPlayerControllerAthena* Controller : GameMode->AlivePlayers)
	{

	}
}

void AFortPlayerControllerAthena::Initialize()
{
	ServerAcknowledgePossession_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x130);
	ServerExecuteInventoryItem_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x22D);
}

IMPLEMENT_MODULE(AFortPlayerControllerAthena);
//IMPLEMENT_MODULE_TICK(AFortPlayerControllerAthena);

PRISM_NAMESPACE_END


