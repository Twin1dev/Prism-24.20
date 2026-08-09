#pragma once

/*=============================================================================
	FortLootTier.h: Picking a loot tier row out of a tier group.

	The first half of a loot drop. A tier group name - "Loot_AthenaFloorLoot",
	say - selects a weighted set of FFortLootTierData rows; one of them is rolled
	for, and its row name becomes the loot tier key that FortLootPackage then
	turns into actual items.

	FFortLootTierData itself is generated and 'final', so the predicates that
	would be members of it in the engine are free functions here.
=============================================================================*/

#include "CoreGlobals.h"

#include <utility>
#include <vector>

PRISM_NAMESPACE_START

/**
 * FGameplayTagContainer::HasTag with bExactMatch off, which is the match the loot tables want.
 *
 * A container keeps its tags' ancestors in ParentTags precisely so this is a lookup rather than a
 * walk up the tag hierarchy, so testing both arrays is the whole of it. Written out here rather
 * than called through UBlueprintGameplayTagLibrary because that would be a ProcessEvent per
 * candidate row, and a floor spawner tests hundreds of them.
 */
inline bool ContainerHasTag(const FGameplayTagContainer& Container, const FGameplayTag& Tag)
{
	return Container.GameplayTags.Contains(Tag) || Container.ParentTags.Contains(Tag);
}

/** FGameplayTagContainer::HasAll with bExactMatch off. An empty requirement is satisfied by anything. */
inline bool ContainerHasAllTags(const FGameplayTagContainer& Container, const FGameplayTagContainer& Required)
{
	for (int32 Index = 0; Index < Required.GameplayTags.Num(); ++Index)
		if (!ContainerHasTag(Container, Required.GameplayTags[Index]))
			return false;

	return true;
}

/** What a tier row has to satisfy to be a candidate. A field left at its default does not filter. */
struct FFortLootTierDataCriteria
{
	FName TierGroup;
	int32 LootTier = -1;
	ELootQuotaLevel QuotaLevel = ELootQuotaLevel::Unlimited;
	const FGameplayTagContainer* RequiredTags = nullptr;
	int32 WorldLevel = -1;
	bool bIgnoreRequiredTags = false;
};

class UFortLootTier
{
public:
	/** The engine's FMath::FRand, near enough: rand() scaled into [0, 1). */
	static float RandomFraction();

	static bool PassesTagRequirement(const FFortLootTierData* TierData, const FGameplayTagContainer* AvailableTags);

	static bool IsValidLootTierData(const FFortLootTierDataCriteria& Criteria, FFortLootTierData* TierData);

	static void CollectLootTierDatas(std::vector<std::pair<FName, FFortLootTierData*>>& OutLootTierDatas, float& OutWeight, const std::vector<UDataTable*>& LootTierDataTables, const FFortLootTierDataCriteria& Criteria);

	static void ChooseLootTierDatas(std::vector<std::pair<FName, FFortLootTierData*>>& OutLootTierDatas, float& OutWeight, const FFortLootTierDataCriteria& Criteria);

	static bool ChooseLootTierKeyAndLootTierByWeight(FName& OutLootTierKey, int32& OutLootTier, float TotalWeight, const std::vector<std::pair<FName, FFortLootTierData*>>& LootTierDatas);

	/** Spreads RequestedNumDrops over the row's loot package categories, honouring their min/max/weight arrays. */
	static bool ProcessLootDrops(FFortLootTierData* LootTierData, int32 RequestedNumDrops, std::vector<std::pair<int32, int32>>& OutLootMap);

	static bool PickLootTierKeyAndLootTierFromTierGroup(FName& OutLootTierKey, int32& OutLootTier, FName TierGroupName, uint32 WorldLevel, int32 ForcedLootTier);

	static FFortLootTierData* FindLootTierDataRow(FName LootTierKey);
};

PRISM_NAMESPACE_END
