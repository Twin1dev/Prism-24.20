#include "pch.h"

#include "Items/FortLootTier.h"
#include "FortGameData.h"

#include <cstdlib>
#include <unordered_map>

PRISM_NAMESPACE_START

/** Below this a total weight is treated as zero, so the roll falls back to an unweighted pick. */
static const float GMinimumUsableWeight = 0.0000000099999999f;

static const size_t GMaxLoggedRows = 8;

/** An FName is one index in this build, so it is its own map key. */
static int32 MakeRowKey(FName Name)
{
	return Name.ComparisonIndex;
}

/**
 * One table's rows, grouped the two ways they get looked up.
 *
 * A loot drop asks "every row in this tier group" and then "the row with this name", and a table
 * has tens of thousands of rows, so both are indexed once per table and reused until the resolved
 * set of tables changes.
 */
struct FLootTierTableIndex
{
	std::unordered_map<int32, std::vector<std::pair<FName, FFortLootTierData*>>> ByTierGroup;
	std::unordered_map<int32, FFortLootTierData*> ByRowName;
	std::vector<FName> TierGroups;
};

static const FLootTierTableIndex& GetLootTierTableIndex(UDataTable* Table)
{
	static std::unordered_map<UDataTable*, FLootTierTableIndex> Cache;
	static uint32 CachedGeneration = 0;

	const uint32 Generation = UFortGameData::GetLootTableGeneration();
	if (Generation != CachedGeneration)
	{
		Cache.clear();
		CachedGeneration = Generation;
	}

	const auto It = Cache.find(Table);
	if (It != Cache.end())
		return It->second;

	FLootTierTableIndex Index;
	for (auto& Row : Table->RowMap)
	{
		FFortLootTierData* TierData = reinterpret_cast<FFortLootTierData*>(Row.Value);
		if (!TierData)
			continue;

		const FName TierGroup = TierData->TierGroup;

		Index.ByRowName.emplace(MakeRowKey(Row.Key), TierData);

		std::vector<std::pair<FName, FFortLootTierData*>>& Group = Index.ByTierGroup[MakeRowKey(TierGroup)];
		if (Group.empty())
			Index.TierGroups.push_back(TierGroup);

		Group.emplace_back(Row.Key, TierData);
	}

	const std::string TableName = Table->GetName();
	UE_LOG(LogPrism, Log, __FUNCTION__": %s indexed %zu row(s) into %zu tier group(s)",
		TableName.c_str(), Index.ByRowName.size(), Index.ByTierGroup.size());

	return Cache.emplace(Table, std::move(Index)).first->second;
}

bool UFortLootTier::PassesTagRequirement(const FFortLootTierData* TierData, const FGameplayTagContainer* AvailableTags)
{
	const FGameplayTagContainer& Required = TierData->RequiredGameplayTags;
	if (Required.GameplayTags.Num() == 0)
		return true;

	if (!AvailableTags || AvailableTags->GameplayTags.Num() == 0)
		return false;

	return ContainerHasAllTags(*AvailableTags, Required);
}

float UFortLootTier::RandomFraction()
{
	return static_cast<float>(std::rand()) * 0.000030518509f;
}

bool UFortLootTier::IsValidLootTierData(const FFortLootTierDataCriteria& Criteria, FFortLootTierData* TierData)
{
	if (!TierData)
		return false;

	if (Criteria.TierGroup != TierData->TierGroup)
		return false;

	if (Criteria.LootTier != -1 && Criteria.LootTier != TierData->LootTier)
		return false;

	if (Criteria.QuotaLevel < ELootQuotaLevel::NumLevels && Criteria.QuotaLevel != TierData->QuotaLevel)
		return false;

	if (!Criteria.bIgnoreRequiredTags && !PassesTagRequirement(TierData, Criteria.RequiredTags))
		return false;

	if (Criteria.WorldLevel >= 0)
	{
		const int32 MaxWorldLevel = TierData->MaxWorldLevel;
		if (MaxWorldLevel >= 0 && Criteria.WorldLevel > MaxWorldLevel)
			return false;

		const int32 MinWorldLevel = TierData->MinWorldLevel;
		if (MinWorldLevel >= 0 && Criteria.WorldLevel < MinWorldLevel)
			return false;
	}

	return true;
}

void UFortLootTier::CollectLootTierDatas(std::vector<std::pair<FName, FFortLootTierData*>>& OutLootTierDatas, float& OutWeight, const std::vector<UDataTable*>& LootTierDataTables, const FFortLootTierDataCriteria& Criteria)
{
	const int32 TierGroupKey = MakeRowKey(Criteria.TierGroup);

	for (UDataTable* Table : LootTierDataTables)
	{
		if (!Table)
			continue;

		const FLootTierTableIndex& Index = GetLootTierTableIndex(Table);
		const auto Group = Index.ByTierGroup.find(TierGroupKey);
		if (Group == Index.ByTierGroup.end())
			continue;

		OutLootTierDatas.reserve(OutLootTierDatas.size() + Group->second.size());

		for (const auto& Entry : Group->second)
		{
			if (!IsValidLootTierData(Criteria, Entry.second))
				continue;

			OutLootTierDatas.push_back(Entry);
			OutWeight += Entry.second->Weight;
		}
	}
}

void UFortLootTier::ChooseLootTierDatas(std::vector<std::pair<FName, FFortLootTierData*>>& OutLootTierDatas, float& OutWeight, const FFortLootTierDataCriteria& Criteria)
{
	CollectLootTierDatas(OutLootTierDatas, OutWeight, UFortGameData::GetLootTierDataTables(), Criteria);
}

bool UFortLootTier::ChooseLootTierKeyAndLootTierByWeight(FName& OutLootTierKey, int32& OutLootTier, float TotalWeight, const std::vector<std::pair<FName, FFortLootTierData*>>& LootTierDatas)
{
	if (LootTierDatas.empty())
		return false;

	if (TotalWeight > GMinimumUsableWeight || TotalWeight < -GMinimumUsableWeight)
	{
		float Roll = RandomFraction() * TotalWeight;

		for (const auto& Entry : LootTierDatas)
		{
			const float Weight = Entry.second->Weight;
			if (Roll <= Weight)
			{
				OutLootTierKey = Entry.first;
				OutLootTier = Entry.second->LootTier;
				return true;
			}

			Roll -= Weight;
		}
	}

	const size_t FallbackIndex = static_cast<size_t>(std::rand()) % LootTierDatas.size();
	const auto& Fallback = LootTierDatas[FallbackIndex];

	OutLootTierKey = Fallback.first;
	OutLootTier = Fallback.second->LootTier;

	UE_LOG(LogPrism, Warning, __FUNCTION__": %zu candidate(s) carry no usable weight (TotalWeight=%f), falling back to an unweighted pick",
		LootTierDatas.size(), TotalWeight);

	return true;
}

bool UFortLootTier::ProcessLootDrops(FFortLootTierData* LootTierData, int32 RequestedNumDrops, std::vector<std::pair<int32, int32>>& OutLootMap)
{
	OutLootMap.clear();

	if (!LootTierData)
		return false;

	TArray<int32>& MinArray = LootTierData->LootPackageCategoryMinArray;
	TArray<int32>& WeightArray = LootTierData->LootPackageCategoryWeightArray;
	TArray<int32>& MaxArray = LootTierData->LootPackageCategoryMaxArray;

	const int32 NumCategories = MinArray.Num();
	if (NumCategories != WeightArray.Num() || NumCategories != MaxArray.Num())
		return false;

	std::vector<int32> CountByCategory(static_cast<size_t>(NumCategories), 0);

	int32 NumAssigned = 0;
	for (int32 Category = 0; Category < NumCategories; ++Category)
	{
		const int32 Min = MinArray[Category];
		if (Min <= 0)
			continue;

		CountByCategory[Category] = Min;
		NumAssigned += Min;
	}

	std::vector<std::pair<int32, int32>> Pool;
	int32 TotalWeight = 0;

	for (int32 Category = 0; Category < NumCategories; ++Category)
	{
		const int32 Weight = WeightArray[Category];
		if (Weight <= 0)
			continue;

		const int32 Max = MaxArray[Category];
		if (Max >= 0 && CountByCategory[Category] >= Max)
			continue;

		Pool.emplace_back(Category, Weight);
		TotalWeight += Weight;
	}

	while (NumAssigned < RequestedNumDrops && TotalWeight > 0 && !Pool.empty())
	{
		const float Roll = RandomFraction() * static_cast<float>(TotalWeight);
		int32 RemainingWeight = static_cast<int32>(Roll + Roll + 0.5f) >> 1;

		size_t PickIndex = 0;
		while (PickIndex + 1 < Pool.size() && RemainingWeight > Pool[PickIndex].second)
		{
			RemainingWeight -= Pool[PickIndex].second;
			++PickIndex;
		}

		const int32 Category = Pool[PickIndex].first;
		++CountByCategory[Category];
		++NumAssigned;

		const int32 Max = MaxArray[Category];
		if (Max >= 0 && CountByCategory[Category] >= Max)
		{
			TotalWeight -= Pool[PickIndex].second;
			Pool.erase(Pool.begin() + PickIndex);
		}
	}

	for (int32 Category = 0; Category < NumCategories; ++Category)
		if (CountByCategory[Category] > 0)
			OutLootMap.emplace_back(Category, CountByCategory[Category]);

	return true;
}

/**
 * Dumps what the tables actually hold for a tier group that resolved to nothing.
 *
 * Once per process: it is only worth having the first time a group comes up empty, and it walks
 * every candidate row.
 */
static void LogTierGroupLookup(const FFortLootTierDataCriteria& Criteria)
{
	static bool bLogged = false;
	if (bLogged)
		return;

	bLogged = true;

	const int32 TierGroupKey = MakeRowKey(Criteria.TierGroup);
	const std::vector<UDataTable*>& Tables = UFortGameData::GetLootTierDataTables();

	UE_LOG(LogPrism, Warning, __FUNCTION__": looking up Key=%d across %zu table(s), LootTier=%d QuotaLevel=%d WorldLevel=%d IgnoreTags=%d",
		TierGroupKey, Tables.size(), Criteria.LootTier, static_cast<int32>(Criteria.QuotaLevel), Criteria.WorldLevel, Criteria.bIgnoreRequiredTags ? 1 : 0);

	for (UDataTable* Table : Tables)
	{
		if (!Table)
			continue;

		const std::string TableName = Table->GetName();
		const FLootTierTableIndex& Index = GetLootTierTableIndex(Table);
		const auto Group = Index.ByTierGroup.find(TierGroupKey);

		if (Group == Index.ByTierGroup.end())
		{
			UE_LOG(LogPrism, Warning, __FUNCTION__": '%s' Table=%p has %zu group(s), Key not present", TableName.c_str(), Table, Index.ByTierGroup.size());
			continue;
		}

		UE_LOG(LogPrism, Warning, __FUNCTION__": '%s' Table=%p Key present with %zu row(s)", TableName.c_str(), Table, Group->second.size());

		size_t Logged = 0;
		for (const auto& Entry : Group->second)
		{
			if (Logged++ >= GMaxLoggedRows)
				break;

			FFortLootTierData* TierData = Entry.second;

			UE_LOG(LogPrism, Warning, __FUNCTION__":   row=%p TierGroup=%d LootTier=%d QuotaLevel=%d World=[%d,%d] Weight=%f TagsEmpty=%d IsValid=%d",
				TierData, TierData->TierGroup.ComparisonIndex, TierData->LootTier, static_cast<int32>(TierData->QuotaLevel),
				TierData->MinWorldLevel, TierData->MaxWorldLevel, TierData->Weight,
				TierData->RequiredGameplayTags.GameplayTags.Num() == 0 ? 1 : 0,
				UFortLootTier::IsValidLootTierData(Criteria, TierData) ? 1 : 0);
		}
	}
}

/**
 * Rolls one tier key out of whatever the criteria match.
 *
 * ForcedLootTier is a ceiling rather than an equality: the caller asks for tier N and gets the
 * highest tier at or below N that has any rows at all, which is what keeps a low-tier chest from
 * dropping nothing when its exact tier is missing from the table.
 */
static bool PickLootTierForCriteria(FName& OutLootTierKey, int32& OutLootTier, const FFortLootTierDataCriteria& Criteria, int32 ForcedLootTier)
{
	std::vector<std::pair<FName, FFortLootTierData*>> LootTierDatas;
	float TotalWeight = 0.0f;
	UFortLootTier::ChooseLootTierDatas(LootTierDatas, TotalWeight, Criteria);

	if (LootTierDatas.empty())
		return false;

	if (ForcedLootTier != -1)
	{
		std::vector<std::pair<FName, FFortLootTierData*>> Filtered;
		float FilteredWeight = 0.0f;

		for (int32 Tier = ForcedLootTier; Tier >= 0 && Filtered.empty(); --Tier)
		{
			for (const auto& Entry : LootTierDatas)
			{
				if (Entry.second->LootTier != Tier)
					continue;

				Filtered.push_back(Entry);
				FilteredWeight += Entry.second->Weight;
			}
		}

		LootTierDatas = std::move(Filtered);
		TotalWeight = FilteredWeight;
	}

	return UFortLootTier::ChooseLootTierKeyAndLootTierByWeight(OutLootTierKey, OutLootTier, TotalWeight, LootTierDatas);
}

static bool PickLootTierWithFallbacks(FName& OutLootTierKey, int32& OutLootTier, const FFortLootTierDataCriteria& Criteria, const FFortLootTierDataCriteria& Relaxed, int32 ForcedLootTier)
{
	if (PickLootTierForCriteria(OutLootTierKey, OutLootTier, Criteria, ForcedLootTier))
		return true;

	if (PickLootTierForCriteria(OutLootTierKey, OutLootTier, Relaxed, ForcedLootTier))
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroup=%s matched nothing under the world level, quota and tag filters, used a relaxed pick", Criteria.TierGroup);
		return true;
	}

	if (ForcedLootTier != -1 && PickLootTierForCriteria(OutLootTierKey, OutLootTier, Relaxed, -1))
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroup=%s has no rows at or below ForcedLootTier=%d, used any tier instead", Criteria.TierGroup, ForcedLootTier);
		return true;
	}

	return false;
}

bool UFortLootTier::PickLootTierKeyAndLootTierFromTierGroup(FName& OutLootTierKey, int32& OutLootTier, FName TierGroupName, uint32 WorldLevel, int32 ForcedLootTier)
{
	FFortLootTierDataCriteria Criteria;
	Criteria.TierGroup = TierGroupName;
	Criteria.WorldLevel = static_cast<int32>(WorldLevel);

	FFortLootTierDataCriteria Relaxed = Criteria;
	Relaxed.WorldLevel = -1;
	Relaxed.QuotaLevel = ELootQuotaLevel::NumLevels;
	Relaxed.bIgnoreRequiredTags = true;

	if (PickLootTierWithFallbacks(OutLootTierKey, OutLootTier, Criteria, Relaxed, ForcedLootTier))
		return true;

	UFortGameData::InvalidateLootTables();

	if (PickLootTierWithFallbacks(OutLootTierKey, OutLootTier, Criteria, Relaxed, ForcedLootTier))
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroup=%s only resolved after re-reading the loot tables, the cached ones had gone stale", TierGroupName);
		return true;
	}

	UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroup=%s has no FFortLootTierData rows in any of the %zu resolved tables",
		TierGroupName, UFortGameData::GetLootTierDataTables().size());
	LogTierGroupLookup(Relaxed);

	return false;
}

FFortLootTierData* UFortLootTier::FindLootTierDataRow(FName LootTierKey)
{
	const int32 RowKey = MakeRowKey(LootTierKey);

	for (UDataTable* Table : UFortGameData::GetLootTierDataTables())
	{
		if (!Table)
			continue;

		const FLootTierTableIndex& Index = GetLootTierTableIndex(Table);
		const auto Row = Index.ByRowName.find(RowKey);
		if (Row != Index.ByRowName.end())
			return Row->second;
	}

	return nullptr;
}

PRISM_NAMESPACE_END
