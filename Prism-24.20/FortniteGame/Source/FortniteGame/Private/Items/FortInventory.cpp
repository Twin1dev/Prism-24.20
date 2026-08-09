#include "pch.h"

#include "Items/FortInventory.h"
#include <Items/FortWorldItem.h>
#include <Items/FortPickup.h>

PRISM_NAMESPACE_START

static bool CanMergeIntoStack(const FFortItemEntry& NewEntry, const FFortItemEntry& ExistingEntry, int32 MaxStack)
{
	if (ExistingEntry.Count >= MaxStack)
		return false;

/*	if (ExistingEntry.Durability < 1.0f)
	{
		UE_LOG(LogPrism, Log, __FUNCTION__": ExistingEntry.Durability < 1.0f");
		return false;
	}*/

	return FFortItemEntry::CanStackTogether(NewEntry, ExistingEntry);
}

static bool IsPrimaryQuickBar(UFortItemDefinition* ItemDefinition)
{
	/*if (!ItemDefinition || !ItemDefinition->ItemType_IsValid())
		return false;

	if (ItemDefinition->bForceIntoOverflow_IsValid() &&
		ItemDefinition->bForceIntoOverflow)
	{
		return false;
	}

	static const char* const ExcludedItemTypes[] = {
		"WeaponHarvest",
		"WorldResource",
		"Ammo",
		"Trap",
		"BuildingPiece",
		"EditTool",
		"Ingredient",
	};

	for (const char* ItemTypeName : ExcludedItemTypes)
		if (ItemDefinition->IsItemType(ItemTypeName))
			return false;*/

	return (!ItemDefinition->IsA<UFortWeaponMeleeItemDefinition>() &&
		!ItemDefinition->IsA<UFortBuildingItemDefinition>() &&
		!ItemDefinition->IsA<UFortAmmoItemDefinition>() &&
		!ItemDefinition->IsA<UFortResourceItemDefinition>() &&
		!ItemDefinition->IsA<UFortTrapItemDefinition>());
}

// TODO: Move to own class
int32 GetMaxStackSize(UFortItemDefinition* ItemDefinition)
{
	float MaxStackSize = 0;

	if (!ItemDefinition->MaxStackSize.Curve.CurveTable)
		MaxStackSize = ItemDefinition->MaxStackSize.Value;
	else
	{
		EEvaluateCurveTableResult Result;
		UDataTableFunctionLibrary::GetDefaultObj()->EvaluateCurveTableRow(ItemDefinition->MaxStackSize.Curve.CurveTable, ItemDefinition->MaxStackSize.Curve.RowName, 0, &Result, &MaxStackSize, FString());

		if (Result == EEvaluateCurveTableResult::RowNotFound)
		{
			UE_LOG(LogPrism, Warning, "Failed to find MaxStackSize Row for Def");
		}
	}

	return MaxStackSize;
}

UFortWorldItem* AFortInventory::AddItem(const FFortItemEntry& ItemEntry)
{
	if (!ItemEntry.IsValidItemEntry() || ItemEntry.Count <= 0)
	{
		if (bDebugInventory)
		{
			UE_LOG(LogPrism, Log, __FUNCTION__": Failed to add item due to ItemEntry being invalid, skipping.");
		}
		return nullptr;
	}

	UFortItemDefinition* ItemDefinition = ItemEntry.ItemDefinition;
	const int32 MaxStack = GetMaxStackSize(ItemDefinition);

	UE_LOG(LogPrism, Log, "ItemDef %s MaxStackSize: %d", ItemDefinition->GetName().c_str(), MaxStack);

	int32 Remaining = ItemEntry.Count;
	UFortWorldItem* ResultItem = nullptr;

	if (MaxStack > 1)
	{
		for (int32 Index = 0; Index < Inventory.ReplicatedEntries.Num(); ++Index)
		{
			if (Remaining <= 0)
				break;

			FFortItemEntry& ExistingEntry = Inventory.ReplicatedEntries[Index];
			if (!CanMergeIntoStack(ItemEntry, ExistingEntry, MaxStack))
			{
				UE_LOG(LogPrism, Log, __FUNCTION__": !CanMergeIntoStack");
				continue;
			}

			const int32 Room = MaxStack - ExistingEntry.Count;
			const int32 AddedAmount = Remaining < Room ? Remaining : Room;

			ExistingEntry.PreviousCount = ExistingEntry.Count;
			ExistingEntry.Count += AddedAmount;
			Remaining -= AddedAmount;
			Inventory.MarkItemDirty(ExistingEntry);

			ResultItem = UFortWorldItem::New(this, &ExistingEntry);
			if (ResultItem && Inventory.ItemInstances.IsValidIndex(Index))
				Inventory.ItemInstances[Index] = ResultItem;
		}
	}

	const bool bIsPrimaryQuickBarType = IsPrimaryQuickBar(ItemDefinition);
	bool bGuidAssigned = false;

	while (Remaining > 0)
	{
		if (bIsPrimaryQuickBarType && IsPrimaryQuickBarFull() && !SwapOutPrimaryQuickBarItem())
		{
			break;
		}

		const int32 StackCount = MaxStack > 1 ? (Remaining < MaxStack ? Remaining : MaxStack) : 1;

		FFortItemEntry NewStack{};
		NewStack = ItemEntry;
		NewStack.Count = StackCount;
		NewStack.PreviousCount = 0;

		if (!bGuidAssigned && ItemEntry.ItemGuid.IsValid())
			bGuidAssigned = true;
		else
			NewStack.ItemGuid = FGuid::NewGuid();

		const int32 AddedIndex = Inventory.ReplicatedEntries.Add(NewStack);
		FFortItemEntry& AddedEntry = Inventory.ReplicatedEntries[AddedIndex];
		AddedEntry.ParentInventory = this;

		if (UFortWorldItem* NewItem = UFortWorldItem::New(this, &AddedEntry))
		{
			UE_LOG(LogFortInventory, Log, "Added %s muddy", AddedEntry.ItemDefinition->GetName().c_str());
			NewItem->SetOwningControllerForTemporaryItem(GetOwningPlayerController());
			Inventory.MarkItemDirty(AddedEntry);
			RecentlyAdded.Add(NewItem);
			ResultItem = NewItem;
			Inventory.ItemInstances.Add(NewItem);
		}
		else
		{
			Inventory.ReplicatedEntries.RemoveAt(AddedIndex);
		}

		Remaining -= StackCount;
	}

	if (ResultItem)
	{
		bRequiresLocalUpdate = true;
		bRequiresSaving = true;

		HandleInventoryLocalUpdate();
	}

	if (bDebugInventory)
	{
		UE_LOG(LogPrism, Log, __FUNCTION__": Added %s to PlayerController with Count %d", ItemEntry.ItemDefinition->GetName().c_str(), ItemEntry.Count);
	}

	return ResultItem;
}

UFortWorldItem* AFortInventory::AddItem(UFortItemDefinition* ItemDefinition, int32 Count)
{
	if (!ItemDefinition)
		return nullptr;

	FFortItemEntry Entry{};
	Entry.Count = Count;
	Entry.ItemDefinition = ItemDefinition;

	return AddItem(Entry);
}

FFortItemEntry AFortInventory::RemoveItem(FGuid ItemGuid)
{
	FFortItemEntry Removed{};

	for (int32 Index = 0; Index < Inventory.ReplicatedEntries.Num(); ++Index)
	{
		if (Inventory.ReplicatedEntries[Index].ItemGuid != ItemGuid)
			continue;

		Removed = Inventory.ReplicatedEntries[Index];
		RemoveItemAtIndex(Index);
		break;
	}

	return Removed;
}

int32 AFortInventory::RemoveItemCount(FGuid ItemGuid, int32 CountToRemove)
{
	if (CountToRemove <= 0)
		return 0;

	for (int32 Index = 0; Index < Inventory.ReplicatedEntries.Num(); ++Index)
	{
		FFortItemEntry& Entry = Inventory.ReplicatedEntries[Index];
		if (Entry.ItemGuid != ItemGuid)
			continue;

		const int32 Removed = Entry.Count < CountToRemove ? Entry.Count : CountToRemove;

		Entry.PreviousCount = Entry.Count;
		Entry.Count -= Removed;

		if (Entry.Count <= 0)
		{
			RemoveItemAtIndex(Index);
		}
		else
		{
			Inventory.MarkItemDirty(Entry);
			bRequiresLocalUpdate = true;
			bRequiresSaving = true;
			HandleInventoryLocalUpdate();
		}

		return Removed;
	}

	return 0;
}

bool AFortInventory::ConsumeItem(FGuid ItemGuid, int32 Count)
{
	if (Count <= 0)
		return false;

	FFortItemEntry* Entry = FindItemEntryByGuid(ItemGuid);
	if (!Entry || !Entry->IsValidItemEntry() || Entry->Count < Count)
		return false;

	RemoveItemCount(ItemGuid, Count);
	return true;
}

bool AFortInventory::SetItemCount(FGuid ItemGuid, int32 NewCount)
{
	FFortItemEntry* Entry = FindItemEntryByGuid(ItemGuid);
	if (!Entry || !Entry->IsValidItemEntry())
		return false;

	if (NewCount <= 0)
	{
		RemoveItem(ItemGuid);
		return true;
	}

	const int32 MaxStack = GetMaxStackSize(Entry->ItemDefinition);
	const int32 ClampedCount = NewCount < MaxStack ? NewCount : MaxStack;

	if (ClampedCount == Entry->Count)
		return true;

	Entry->PreviousCount = Entry->Count;
	Entry->Count = ClampedCount;
	Inventory.MarkItemDirty(*Entry);

	bRequiresLocalUpdate = true;
	bRequiresSaving = true;
	HandleInventoryLocalUpdate();

	return true;
}

int32 AFortInventory::GetPrimaryQuickBarUsedCount() const
{
	int32 Count = 0;

	for (const FFortItemEntry& Entry : Inventory.ReplicatedEntries)
	{
		if (Entry.ItemDefinition && (IsPrimaryQuickBar(Entry.ItemDefinition)))
			++Count;
	}

	return Count;
}

bool AFortInventory::SwapOutPrimaryQuickBarItem()
{
	AFortPlayerController* PlayerController = GetOwningPlayerController();
	AFortPlayerPawn* Pawn = PlayerController ? PlayerController->MyFortPawn : nullptr;
	if (!Pawn)
		return false;

	UWorld* World = UWorld::GetWorld();
	if (!World)
		return false;

	FGuid EntryGuidToSwap;
	bool bFoundEntry = false;

	if (AFortWeapon* CurrentWeapon = Pawn->CurrentWeapon)
	{
		if (FFortItemEntry* Entry = FindItemEntryByGuid(CurrentWeapon->ItemEntryGuid))
		{
			if (Entry->ItemDefinition && IsPrimaryQuickBar(Entry->ItemDefinition))
			{
				EntryGuidToSwap = Entry->ItemGuid;
				bFoundEntry = true;
			}
		}
	}

	if (!bFoundEntry)
	{
		for (const FFortItemEntry& Entry : Inventory.ReplicatedEntries)
		{
			if (Entry.ItemDefinition && IsPrimaryQuickBar(Entry.ItemDefinition))
			{
				EntryGuidToSwap = Entry.ItemGuid;
				bFoundEntry = true;
				break;
			}
		}
	}

	if (!bFoundEntry)
		return false;

	FFortItemEntry RemovedEntry = RemoveItem(EntryGuidToSwap);
	if (!RemovedEntry.IsValidItemEntry())
		return false;

	FortPickupCreationData Data(World, RemovedEntry, Pawn->K2_GetActorLocation(),
		EFortPickupSourceTypeFlag::Player, EFortPickupSpawnSource::Unset, true, PlayerController);

	AFortPickup* Pickup = AFortPickup::CreateFromData(Data);

	return true;
}

bool AFortInventory::RemoveItemAtIndex(int32 Index)
{
	if (!Inventory.ReplicatedEntries.IsValidIndex(Index))
		return false;

	FFortItemEntry& Entry = Inventory.ReplicatedEntries[Index];

	if (UFortWorldItem* RemovedItem = UFortWorldItem::New(this, &Entry))
		RecentlyRemoved.Add(RemovedItem);

	const int32 LastIndex = Inventory.ReplicatedEntries.Num() - 1;
	if (Index != LastIndex)
	{
		//CopyStruct(&Inventory.ReplicatedEntries[Index], &Inventory.ReplicatedEntries[LastIndex]);
		Inventory.ReplicatedEntries[Index] = Inventory.ReplicatedEntries[LastIndex];
		if (Inventory.ItemInstances.IsValidIndex(LastIndex))
			Inventory.ItemInstances[Index] = Inventory.ItemInstances[LastIndex];
	}

	Inventory.ReplicatedEntries.RemoveAt(LastIndex);
	if (Inventory.ItemInstances.IsValidIndex(LastIndex))
		Inventory.ItemInstances.RemoveAt(LastIndex);
	Inventory.MarkArrayDirty();

	bRequiresLocalUpdate = true;
	bRequiresSaving = true;
	HandleInventoryLocalUpdate();

	return true;
}

PRISM_NAMESPACE_END

