#include "pch.h"

#include "Building/BuildingContainer.h"
#include "Athena/FortGameModeAthena.h"
#include "FortKismetLibrary.h"
#include "Items/FortPickup.h"
#include <Templates/Casts.h>

PRISM_NAMESPACE_START

FVector ABuildingContainer::DetermineSpawnLocationAthena(AFortPlayerPawn* Pawn)
{
	return K2_GetActorLocation() +
		GetActorForwardVector() * LootSpawnLocation_Athena.X +
		GetActorRightVector() * LootSpawnLocation_Athena.Y +
		GetActorUpVector() * LootSpawnLocation_Athena.Z;
}

bool ABuildingContainer::SpawnLoot(AFortPlayerPawn* FortPlayerPawn, EFortPickupSourceTypeFlag SourceTypeFlag, EFortPickupSpawnSource SpawnSource)
{
	if (Role != ENetRole::ROLE_Authority)
	{
		return false;
	}

	UWorld* World = UWorld::GetWorld();
	if (!World)
	{
		return false;
	}

	AFortGameModeAthena* GameMode = ToPrism(World->AuthorityGameMode);
	if (!GameMode)
	{
		return false;
	}

	FName RedirectedTierGroup = GameMode->RedirectLootTierGroup(SearchLootTierGroup);

	TArray<FFortItemEntry> LootToDrop;
	const bool bPickedLoot = UFortKismetLibrary::PickLootDrops(this, LootToDrop, RedirectedTierGroup, 0, -1);

	if (!bPickedLoot)
	{
		return false;
	}

	const FVector BaseSpawnLocation = DetermineSpawnLocationAthena(FortPlayerPawn);

	bool bSpawnedAny = false;

	for (FFortItemEntry& Entry : LootToDrop)
	{
		FortPickupCreationData Data(World, Entry, BaseSpawnLocation, SourceTypeFlag, SpawnSource, true,
			FortPlayerPawn ? Cast<AFortPlayerController>(FortPlayerPawn->GetOwner()) : nullptr, this);

		if (AFortPickup* Pickup = AFortPickup::CreateFromData(Data))
		{
			bSpawnedAny = true;
		}
	}

	if (bSpawnedAny)
	{
		SearchBounceData.SearchAnimationCount++;
		BounceContainer();
		bAlreadySearched = true;
		OnRep_bAlreadySearched();
	}

	return bSpawnedAny;
}

void ABuildingContainer::PostUpdate(EFortBuildingPersistentState PersistentState, __int64 ReservedRandomValues)
{
	if (!IsA(ABuildingContainer::StaticClass()))
		return PostUpdate_Original(this, PersistentState, ReservedRandomValues);

	if (bStartAlreadySearched_Athena)
	{
		if (K2_GetActorLocation().Z == 0)
		{
			return;
		}

		SpawnLoot(nullptr, EFortPickupSourceTypeFlag::FloorLoot, EFortPickupSpawnSource::Unset);
	}

	return PostUpdate_Original(this, PersistentState, ReservedRandomValues);
}

void ABuildingContainer::Initialize()
{
	SpawnLoot_Override.InstallAtRelativeAddress(0x9C7483C);
	PostUpdate_Override.InstallAtRelativeAddress(0x9C70DE0);
}

IMPLEMENT_MODULE(ABuildingContainer);

PRISM_NAMESPACE_END


