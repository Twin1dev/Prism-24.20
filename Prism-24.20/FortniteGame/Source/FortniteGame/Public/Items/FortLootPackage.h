#pragma once

/*=============================================================================
	FortLootPackage.h: Turning a loot tier key into item entries.

	The second half of a loot drop. The tier row names a loot package and how
	many drops to take from it; each drop rolls one weighted FFortLootPackageData
	row, and that row either names an item definition or calls another package -
	which is how a single "AthenaFloorLoot" resolves into a weapon, its ammo and
	a consumable.

	FFortLootPackageData is generated and 'final', so the predicates that would be
	members of it in the engine are free functions here.
=============================================================================*/

#include "CoreGlobals.h"

#include <vector>

PRISM_NAMESPACE_START

/** What a package row has to satisfy to be a candidate. A field left at its default does not filter. */
struct FFortLootPackageDataCriteria
{
	FName LootPackageID;
	int32 LootPackageCategory = -1;
	const FGameplayTagContainer* GameplayTags = nullptr;
	int32 WorldLevel = -1;
	bool bIgnoreRequiredTags = false;
};

class UFortLootPackage
{
public:
	static bool PassesTagRequirement(const FFortLootPackageData* PackageData, const FGameplayTagContainer* AvailableTags);

	static bool IsValidLootPackage(FFortLootPackageData* PackageData, const FFortLootPackageDataCriteria& Criteria);

	static void CollectLootPackageDatas(std::vector<FFortLootPackageData*>& OutLootPackageDatas, float& OutWeight, const std::vector<UDataTable*>& LootPackageDataTables, const FFortLootPackageDataCriteria& Criteria);

	static void ChooseLootPackageDatas(std::vector<FFortLootPackageData*>& OutLootPackageDatas, float& OutWeight, const FFortLootPackageDataCriteria& Criteria);

	static void PickLootDropsFromLootPackage(TArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32 WorldLevel, int32 LootPackageCategory, const FGameplayTagContainer* RequiredTags);

	static bool PickLootDrops(TArray<FFortItemEntry>& OutLootToDrop, uint32 WorldLevel, FName LootTierKey, const FGameplayTagContainer* StaticGameplayTags);
};

PRISM_NAMESPACE_END
