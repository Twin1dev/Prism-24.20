#include "pch.h"

#include "Athena/FortPlayerControllerAthena.h"
#include <Templates/Casts.h>
#include "Items/FortInventory.h"
#include "Engine/World.h"
#include "Building/BuildingSMActor.h"

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

ABuildingSMActor* AFortPlayerControllerAthena::ServerCreateBuildingActorInternal(FBuildingClassData* BuildingClassData, const FVector_NetQuantize10& BuildLoc, const FRotator& BuildRot, bool bMirrored, float SyncKey, bool bForceOverrideInteractBuildCheck, bool bIgnoreCostCheck, bool bIgnoreBuildingValidityCheck)
{
	if (!MyFortPawn || MyFortPawn->bIsDBNO)
		return nullptr;

	AFortGameStateAthena* GameState = UWorld::GetWorld()->GameState;
	UWorld* World = ToPrism(UWorld::GetWorld());

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

			if (bIgnoreCostCheck)
			{
				if (!CanAffordToPlaceBuildableClass(BuildingClassData))
				{
					return nullptr;
				}

				PayBuildableClassPlacementCost(BuildingClassData);
			}

			FTransform Transform = FTransform(BuildRot.Quaternion(), BuildLoc);
			BuildingSMActor = (ABuildingSMActor*)World->SpawnActor(BuildingClass, Transform);
			if (BuildingSMActor)
			{
				BuildingSMActor->InitializeKismetSpawnedBuildingActor(BuildingSMActor, this, true, nullptr);
				BuildingSMActor->bPlayerPlaced = true;

				if (AFortPlayerStateAthena* FortPlayerState = Cast<AFortPlayerStateAthena>(PlayerState))
				{
					BuildingSMActor->Team = EFortTeam(FortPlayerState->TeamIndex);
					BuildingSMActor->OnRep_Team();
					BuildingSMActor->SetTeam(FortPlayerState->TeamIndex);
				}
			}
		}
	}

	if (!BuildingActors.IsEmpty())
		BuildingActors.Empty();

	return BuildingSMActor;
}

void AFortPlayerControllerAthena::ServerCreateBuildingActor(FCreateBuildingActorData& CreateBuildingData)
{
	FBuildingClassData* BuildingClassData = &CreateBuildingData.BuildingClassData;

	for (auto& BuildableClass : UWorld::GetWorld()->GameState->AllPlayerBuildableClassesIndexLookup)
	{
		if (BuildableClass.Value == CreateBuildingData.BuildingClassHandle)
		{
			CreateBuildingData.BuildingClassData.BuildingClass = BuildableClass.Key;
			break;
		}
	}

	ServerCreateBuildingActorInternal(BuildingClassData, CreateBuildingData.BuildLoc, CreateBuildingData.BuildRot, CreateBuildingData.bMirrored, CreateBuildingData.SyncKey);
}

void AFortPlayerControllerAthena::ServerBeginEditingBuildingActor(ABuildingSMActor* BuildingActorToEdit)
{
	if (!PlayerState || !BuildingActorToEdit || !MyFortPawn)
		return;

	BuildingActorToEdit->SetEditingPlayer(PlayerState);

	/*static UFortGameData* GameData = UFortEngine::GetEngine()->AssetManager->GameDataCommon;
	static UFortWeaponItemDefinition* EditToolItemDefinition = GameData->EditToolItem.Get();
	if (!EditToolItemDefinition)
	{
		EditToolItemDefinition = GameData->EditToolItem.LoadSynchronous();
	}*/

	FFortItemEntry* ItemEntry = WorldInventory->Inventory.ReplicatedEntries.FindByPredicate([&](FFortItemEntry& Entry) {
		return Entry.ItemDefinition->Class == UFortEditToolItemDefinition::StaticClass();
		});

	if (ItemEntry && MyFortPawn->EquipWeaponDefinition(Cast<UFortWeaponItemDefinition>(ItemEntry->ItemDefinition), ItemEntry->ItemGuid, ItemEntry->TrackerGuid, false))
	{
		if (AFortWeap_EditingTool* EditingTool = Cast<AFortWeap_EditingTool>(MyFortPawn->CurrentWeapon))
		{
			EditingTool->EditActor = BuildingActorToEdit;
			EditingTool->OnRep_EditActor();
		}
	}
}

void AFortPlayerControllerAthena::ServerEditBuildingActor(ABuildingSMActor* BuildingActorToEdit, TSubclassOf<ABuildingSMActor> NewBuildingClass, uint8 RotationIterations, bool bMirrored)
{
	if (!BuildingActorToEdit || !BuildingActorToEdit->IsA(ABuildingSMActor::StaticClass()) || !MyFortPawn)
		return;

	BuildingActorToEdit->SetEditingPlayer(nullptr);

	if (ABuildingSMActor* NewBuildingActor = BuildingActorToEdit->ReplaceBuildingActor(EBuildingReplacementType::BRT_Edited, NewBuildingClass, BuildingActorToEdit->CurrentBuildingLevel, RotationIterations, bMirrored, this))
	{
		NewBuildingActor->bPlayerPlaced = true;
	}
}

void AFortPlayerControllerAthena::ServerEndEditingBuildingActor(ABuildingSMActor* BuildingActorToStopEditing)
{
	if (!BuildingActorToStopEditing || BuildingActorToStopEditing->EditingPlayer != PlayerState || BuildingActorToStopEditing->bDestroyed || !MyFortPawn)
		return;

	BuildingActorToStopEditing->SetEditingPlayer(nullptr);

	if (AFortWeap_EditingTool* EditingTool = Cast<AFortWeap_EditingTool>(MyFortPawn->CurrentWeapon))
	{
		EditingTool->EditActor = BuildingActorToStopEditing;
		EditingTool->OnRep_EditActor();
	}
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
	ServerCreateBuildingActor_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x24D);
	ServerBeginEditingBuildingActor_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x254);
	ServerEditBuildingActor_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x24F);
	ServerEndEditingBuildingActor_Override.InstallAtVirtualIndex("FortPlayerControllerAthena", 0x252);
}

IMPLEMENT_MODULE(AFortPlayerControllerAthena);
//IMPLEMENT_MODULE_TICK(AFortPlayerControllerAthena);

PRISM_NAMESPACE_END


