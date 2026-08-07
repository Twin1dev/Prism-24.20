#pragma once

#include "CoreGlobals.h"
#include <Templates/Casts.h>
// AddItem hands back the Prism UFortWorldItem, so the declarations below have to mean that one.
#include <Items/FortWorldItem.h>

PRISM_NAMESPACE_START

class AFortInventory : public UE_EXTENDS_SDK_CLASS(AFortInventory)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(AFortInventory);

	constexpr static bool bDebugInventory = true;

	constexpr static int32 PrimaryQuickBarCapacity = 5;

	UFortWorldItem* AddItem(const FFortItemEntry & ItemEntry);
	UFortWorldItem* AddItem(UFortItemDefinition * ItemDefinition, int32 Count);

	FFortItemEntry RemoveItem(FGuid ItemGuid);
	int32 RemoveItemCount(FGuid ItemGuid, int32 CountToRemove);
	bool ConsumeItem(FGuid ItemGuid, int32 Count = 1);
	bool SetItemCount(FGuid ItemGuid, int32 NewCount);

	int32 GetInventoryUsed() const
	{
		return Inventory.ReplicatedEntries.Num();
	}

	//void DebugDumpInventory(const char* Context);

	int32 GetPrimaryQuickBarUsedCount() const;
	bool IsPrimaryQuickBarFull() const
	{
		const bool ToRet = GetPrimaryQuickBarUsedCount() >= PrimaryQuickBarCapacity;

		UE_LOG(LogPrism, Log, __FUNCTION__": %d", ToRet);

		return ToRet;
	}

	AFortPlayerController* GetOwningPlayerController()
	{
		return Cast<AFortPlayerController>(Owner);
	}

	FFortItemEntry* FindItemEntryByDefinition(UFortItemDefinition* ItemDefinition)
	{
		for (FFortItemEntry& Entry : Inventory.ReplicatedEntries)
			if (Entry.ItemDefinition == ItemDefinition)
				return &Entry;

		return nullptr;
	}

	FFortItemEntry* FindItemEntryByGuid(FGuid ItemGuid)
	{
		for (FFortItemEntry& Entry : Inventory.ReplicatedEntries)
			if (Entry.ItemGuid == ItemGuid)
				return &Entry;

		return nullptr;
	}

private:
	bool RemoveItemAtIndex(int32 Index);
	bool SwapOutPrimaryQuickBarItem();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortInventory);
UE_DECLARE_PRISM_TYPE(AFortInventory);

PRISM_NAMESPACE_END