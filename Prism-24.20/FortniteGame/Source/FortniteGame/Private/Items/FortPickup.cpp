#include "pch.h"

#include "Items/FortPickup.h"
#include <Templates/Casts.h>
#include "Items/FortInventory.h"

PRISM_NAMESPACE_START

void AFortPickup::GivePickupTo(IFortInventoryOwnerInterface* OwnerInterface, bool bDestroyAfterPickup)
{
	GivePickupTo_Original(this, OwnerInterface, bDestroyAfterPickup);

	if (Role != ENetRole::ROLE_Authority)
		return;

	AFortPlayerPawn* Pawn = Cast<AFortPlayerPawn>(PickupLocationData.PickupTarget); // UNSURE

	if (Pawn == nullptr)
	{
		return;
	}

	AFortPlayerController* PlayerController = Cast<AFortPlayerController>(Pawn->Controller);

	if (!PlayerController)
	{
		return;
	}

	AFortInventory* Inventory = ToPrism(PlayerController->WorldInventory);
	if (!Inventory)
	{
		return;
	}

	if (PrimaryPickupItemEntry.IsValidItemEntry())
		Inventory->AddItem(PrimaryPickupItemEntry);

	for (FFortItemEntry& Entry : MultiItemPickupEntries)
	{
		if (Entry.IsValidItemEntry())
			Inventory->AddItem(Entry);
	}
}

void AFortPickup::Initialize()
{
	GivePickupTo_Override.InstallAtVirtualIndex("FortPickupAthena", 0xE4);
}

IMPLEMENT_MODULE(AFortPickup);

PRISM_NAMESPACE_END


