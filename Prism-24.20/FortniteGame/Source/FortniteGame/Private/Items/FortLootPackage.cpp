#include "pch.h"

#include "Items/FortLootPackage.h"
#include "Items/FortLootTier.h"
#include "Items/FortInventory.h"
#include "FortGameData.h"

#include <Templates/Casts.h>

#include <cstdlib>
#include <unordered_map>

PRISM_NAMESPACE_START

/** Below this a total weight is treated as zero, so the roll falls back to an unweighted pick. */
static const float GMinimumUsableWeight = 0.0000000099999999f;

/** A LootPackageCall chain longer than this is a cycle in the table, not a deep package. */
static const int32 GMaxLootPackageCallDepth = 16;

/** An FName is one index in this build, so it is its own map key. */
static int32 MakeRowKey(FName Name)
{
	return Name.ComparisonIndex;
}

/** One table's rows grouped by the package id they belong to, which is the only way they get looked up. */
struct FLootPackageTableIndex
{
	std::unordered_map<int32, std::vector<FFortLootPackageData*>> ByPackageID;
};

static bool HasLootTablesChanged(uint32& CachedGeneration)
{
	const uint32 Generation = UFortGameData::GetLootTableGeneration();
	if (Generation == CachedGeneration)
		return false;

	CachedGeneration = Generation;
	return true;
}

static const FLootPackageTableIndex& GetLootPackageTableIndex(UDataTable* Table)
{
	static std::unordered_map<UDataTable*, FLootPackageTableIndex> Cache;
	static uint32 CachedGeneration = 0;

	if (HasLootTablesChanged(CachedGeneration))
		Cache.clear();

	const auto It = Cache.find(Table);
	if (It != Cache.end())
		return It->second;

	FLootPackageTableIndex Index;
	for (auto& Row : Table->RowMap)
	{
		FFortLootPackageData* PackageData = reinterpret_cast<FFortLootPackageData*>(Row.Value);
		if (!PackageData)
			continue;

		Index.ByPackageID[MakeRowKey(PackageData->LootPackageID)].push_back(PackageData);
	}

	UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": indexed %zu loot package ids for %s", Index.ByPackageID.size(), Table->GetName().c_str());

	return Cache.emplace(Table, std::move(Index)).first->second;
}

/**
 * LootPackageCall as an FName.
 *
 * The column is an FString, and it is read once per candidate row on every drop, so the conversion
 * is cached rather than repeated. Cleared with the tables, since the rows go with them.
 */
static FName GetLootPackageCallName(FFortLootPackageData* PackageData)
{
	static std::unordered_map<FFortLootPackageData*, FName> Cache;
	static uint32 CachedGeneration = 0;

	if (HasLootTablesChanged(CachedGeneration))
		Cache.clear();

	const auto It = Cache.find(PackageData);
	if (It != Cache.end())
		return It->second;

	FName CallName;
	const FString& LootPackageCall = PackageData->LootPackageCall;
	if (!LootPackageCall.IsEmpty())
		CallName = FName(*LootPackageCall);

	Cache.emplace(PackageData, CallName);
	return CallName;
}

bool UFortLootPackage::PassesTagRequirement(const FFortLootPackageData* PackageData, const FGameplayTagContainer* AvailableTags)
{
	const FName Required = PackageData->RequiredTag;
	if (Required.IsNone())
		return true;

	if (!AvailableTags || AvailableTags->GameplayTags.Num() == 0)
		return false;

	FGameplayTag RequiredTag{};
	RequiredTag.TagName = Required;

	return ContainerHasTag(*AvailableTags, RequiredTag);
}

bool UFortLootPackage::IsValidLootPackage(FFortLootPackageData* PackageData, const FFortLootPackageDataCriteria& Criteria)
{
	if (!PackageData)
		return false;

	if (Criteria.LootPackageID != PackageData->LootPackageID)
		return false;

	if (Criteria.LootPackageCategory != -1 && Criteria.LootPackageCategory != PackageData->LootPackageCategory)
		return false;

	if (!Criteria.bIgnoreRequiredTags && !PassesTagRequirement(PackageData, Criteria.GameplayTags))
		return false;

	if (Criteria.WorldLevel >= 0)
	{
		const int32 MaxWorldLevel = PackageData->MaxWorldLevel;
		if (MaxWorldLevel >= 0 && Criteria.WorldLevel > MaxWorldLevel)
			return false;

		const int32 MinWorldLevel = PackageData->MinWorldLevel;
		if (MinWorldLevel >= 0 && Criteria.WorldLevel < MinWorldLevel)
			return false;
	}

	return true;
}

void UFortLootPackage::CollectLootPackageDatas(std::vector<FFortLootPackageData*>& OutLootPackageDatas, float& OutWeight, const std::vector<UDataTable*>& LootPackageDataTables, const FFortLootPackageDataCriteria& Criteria)
{
	const int32 PackageKey = MakeRowKey(Criteria.LootPackageID);

	for (UDataTable* Table : LootPackageDataTables)
	{
		if (!Table)
			continue;

		const FLootPackageTableIndex& Index = GetLootPackageTableIndex(Table);
		const auto Rows = Index.ByPackageID.find(PackageKey);
		if (Rows == Index.ByPackageID.end())
			continue;

		OutLootPackageDatas.reserve(OutLootPackageDatas.size() + Rows->second.size());

		for (FFortLootPackageData* PackageData : Rows->second)
		{
			if (!IsValidLootPackage(PackageData, Criteria))
				continue;

			OutLootPackageDatas.push_back(PackageData);
			OutWeight += PackageData->Weight;
		}
	}
}

void UFortLootPackage::ChooseLootPackageDatas(std::vector<FFortLootPackageData*>& OutLootPackageDatas, float& OutWeight, const FFortLootPackageDataCriteria& Criteria)
{
	CollectLootPackageDatas(OutLootPackageDatas, OutWeight, UFortGameData::GetLootPackageDataTables(), Criteria);
}

static FFortLootPackageData* SelectLootPackageData(const std::vector<FFortLootPackageData*>& LootPackageDatas, float TotalWeight)
{
	if (LootPackageDatas.empty())
		return nullptr;

	if (TotalWeight > GMinimumUsableWeight)
	{
		float Roll = UFortLootTier::RandomFraction() * TotalWeight;

		for (FFortLootPackageData* PackageData : LootPackageDatas)
		{
			const float Weight = PackageData->Weight;
			if (Roll <= Weight)
				return PackageData;

			Roll -= Weight;
		}
	}

	return LootPackageDatas[static_cast<size_t>(std::rand()) % LootPackageDatas.size()];
}

/**
 * MinLevel/MaxLevel as the definition declares them; a negative MaxLevel means "no ceiling".
 *
 * Only UFortAccountItemDefinition carries the pair in 24.20 - it is where item levelling lives, and
 * that is the cosmetic and persistent side of the item tree. Everything a loot package drops is a
 * UFortWorldItemDefinition, which has neither, so the clamp is to zero and dropped items arrive at
 * level 0. That is what the engine does with them too.
 */
static int32 ClampItemLevel(UFortItemDefinition* ItemDefinition, int32 InLevel)
{
	UFortAccountItemDefinition* AccountItemDefinition = Cast<UFortAccountItemDefinition>(ItemDefinition);

	int32 Clamped = AccountItemDefinition ? AccountItemDefinition->MinLevel : 0;
	if (Clamped < InLevel)
		Clamped = InLevel;

	const int32 MaxLevel = AccountItemDefinition ? AccountItemDefinition->MaxLevel : 0;
	if (MaxLevel < 0)
		return Clamped;

	return Clamped < MaxLevel ? Clamped : MaxLevel;
}

/**
 * FFortItemEntry::ControlOverride for a freshly created entry.
 *
 * A checksum over the definition's slot count, which is what the engine stores here. FCrc::MemCrc32
 * is inlined everywhere in this binary and has no address to call, so this is an ordinary reflected
 * CRC-32 rather than the engine's own table - the value is self-consistent across this module, not
 * byte-identical to what the engine would have written. Nothing compares the two.
 */
static int32 GetControlValue(UFortItemDefinition* ItemDefinition)
{
	UFortWorldItemDefinition* WorldItemDefinition = Cast<UFortWorldItemDefinition>(ItemDefinition);
	if (!WorldItemDefinition)
		return 0;

	const int32 SlotsToTake = static_cast<int32>(WorldItemDefinition->NumberOfSlotsToTake);
	const uint8* Bytes = reinterpret_cast<const uint8*>(&SlotsToTake);

	uint32 Crc = 0xFFFFFFFFu;
	for (size_t Index = 0; Index < sizeof(SlotsToTake); ++Index)
	{
		Crc ^= Bytes[Index];
		for (int32 Bit = 0; Bit < 8; ++Bit)
			Crc = (Crc >> 1) ^ (0xEDB88320u & (0u - (Crc & 1u)));
	}

	return static_cast<int32>(~Crc);
}

/**
 * How much ammo a dropped weapon arrives loaded with.
 *
 * Read straight out of the weapon's stat row rather than through GetWeaponStatsRow, which would be
 * a ProcessEvent call copying a 0x2xx-byte struct per drop. Cached per definition, since the row is
 * a property of the asset and does not change while it is loaded.
 */
static int32 GetLoadedAmmo(UFortItemDefinition* ItemDefinition)
{
	UFortWeaponItemDefinition* WeaponDefinition = Cast<UFortWeaponItemDefinition>(ItemDefinition);
	if (!WeaponDefinition)
		return 0;

	static std::unordered_map<UFortWeaponItemDefinition*, int32> Cache;

	const auto It = Cache.find(WeaponDefinition);
	if (It != Cache.end())
		return It->second;

	int32 LoadedAmmo = 0;

	const FDataTableRowHandle& StatHandle = WeaponDefinition->WeaponStatHandle;
	if (StatHandle.DataTable)
	{
		for (auto& Row : StatHandle.DataTable->RowMap)
		{
			if (Row.Key != StatHandle.RowName)
				continue;

			if (FFortBaseWeaponStats* WeaponStats = reinterpret_cast<FFortBaseWeaponStats*>(Row.Value))
				LoadedAmmo = WeaponStats->ClipSize;

			break;
		}
	}

	Cache.emplace(WeaponDefinition, LoadedAmmo);
	return LoadedAmmo;
}

/** Splits TotalCount into as many stacks as the definition's max stack size allows. */
static void AppendItemEntries(TArray<FFortItemEntry>& OutLootToDrop, UFortItemDefinition* ItemDefinition, int32 TotalCount, int32 ItemLevel)
{
	const int32 MaxStackSize = GetMaxStackSize(ItemDefinition);
	if (MaxStackSize <= 0)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": %s reports MaxStackSize=%d, refusing to split %d item(s)",
			ItemDefinition->GetName().c_str(), MaxStackSize, TotalCount);
		return;
	}

	const int32 ClampedLevel = ClampItemLevel(ItemDefinition, ItemLevel);
	const int32 ControlValue = GetControlValue(ItemDefinition);
	const int32 LoadedAmmo = GetLoadedAmmo(ItemDefinition);

	while (TotalCount > 0)
	{
		const int32 StackCount = TotalCount < MaxStackSize ? TotalCount : MaxStackSize;

		/*
		 * Built here and copied in, rather than added defaulted and filled in place: FFortItemEntry
		 * is not trivially default constructible - it holds arrays and strings - so AddDefaulted
		 * default-initialises it, which runs those members' constructors and leaves every scalar
		 * next to them holding whatever the buffer had. The braces value-initialise, which zeroes
		 * the whole entry first, so the fields this does not set are zero rather than garbage.
		 */
		FFortItemEntry NewEntry{};
		NewEntry.ItemDefinition = ItemDefinition;
		NewEntry.Count = StackCount;
		NewEntry.Level = ClampedLevel;
		NewEntry.LoadedAmmo = LoadedAmmo;
		NewEntry.ControlOverride = ControlValue;

		OutLootToDrop.Add(NewEntry);

		TotalCount -= StackCount;
	}
}

static void PickLootDropsFromLootPackageInternal(TArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32 WorldLevel, int32 LootPackageCategory, const FGameplayTagContainer* RequiredTags, int32 Depth)
{
	if (Depth >= GMaxLootPackageCallDepth)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": LootPackageCall chain exceeded %d levels at %s, stopping to avoid a cycle", GMaxLootPackageCallDepth, LootPackageName);
		return;
	}

	FFortLootPackageDataCriteria Criteria;
	Criteria.LootPackageID = LootPackageName;
	Criteria.LootPackageCategory = LootPackageCategory;
	Criteria.GameplayTags = RequiredTags;
	Criteria.WorldLevel = WorldLevel;

	std::vector<FFortLootPackageData*> LootPackageDatas;
	float TotalWeight = 0.0f;
	UFortLootPackage::ChooseLootPackageDatas(LootPackageDatas, TotalWeight, Criteria);

	if (LootPackageDatas.empty())
	{
		Criteria.WorldLevel = -1;
		Criteria.bIgnoreRequiredTags = true;
		UFortLootPackage::ChooseLootPackageDatas(LootPackageDatas, TotalWeight, Criteria);

		if (LootPackageDatas.empty())
		{
			UE_LOG(LogPrism, Warning, __FUNCTION__": no FFortLootPackageData rows matched LootPackageID=%s LootPackageCategory=%d WorldLevel=%d",
				LootPackageName, LootPackageCategory, WorldLevel);
			return;
		}

		UE_LOG(LogPrism, Warning, __FUNCTION__": LootPackageID=%s only matched after dropping the world level and tag filters", LootPackageName);
	}

	FFortLootPackageData* Selected = SelectLootPackageData(LootPackageDatas, TotalWeight);
	if (!Selected)
		return;

	const FName LootPackageCall = GetLootPackageCallName(Selected);
	if (!LootPackageCall.IsNone())
	{
		const int32 CallCount = Selected->Count;
		for (int32 Index = 0; Index < CallCount; ++Index)
			PickLootDropsFromLootPackageInternal(OutLootToDrop, LootPackageCall, WorldLevel, -1, RequiredTags, Depth + 1);

		return;
	}

	UFortItemDefinition* ItemDefinition = Selected->ItemDefinition.LoadSynchronous();
	if (!ItemDefinition)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": LootPackageID=%s has neither a LootPackageCall nor a loadable ItemDefinition", LootPackageName);
		return;
	}

	AppendItemEntries(OutLootToDrop, ItemDefinition, Selected->Count, 0);
}

void UFortLootPackage::PickLootDropsFromLootPackage(TArray<FFortItemEntry>& OutLootToDrop, FName LootPackageName, int32 WorldLevel, int32 LootPackageCategory, const FGameplayTagContainer* RequiredTags)
{
	PickLootDropsFromLootPackageInternal(OutLootToDrop, LootPackageName, WorldLevel, LootPackageCategory, RequiredTags, 0);
}

/** NumLootPackageDrops is fractional; the remainder is the chance of one more drop. */
static int32 ComputeNumberOfLootDrops(float NumLootPackageDrops)
{
	if (NumLootPackageDrops < 1.0f)
		return 1;

	int32 NumberOfLootDrops = static_cast<int32>(NumLootPackageDrops + NumLootPackageDrops - 0.5f) >> 1;

	const float Fractional = NumLootPackageDrops - static_cast<float>(NumberOfLootDrops);
	if (Fractional > 0.0000099999997f && Fractional >= UFortLootTier::RandomFraction())
		++NumberOfLootDrops;

	return NumberOfLootDrops;
}

/** Folds duplicate entries of anything that refuses to occupy two inventory slots. */
static void MergeUnstackableEntries(TArray<FFortItemEntry>& OutLootToDrop)
{
	for (int32 i = 0; i < OutLootToDrop.Num(); ++i)
	{
		UFortItemDefinition* ItemDefinition = OutLootToDrop[i].ItemDefinition;
		if (!ItemDefinition || ItemDefinition->bAllowMultipleStacks)
			continue;

		for (int32 j = OutLootToDrop.Num() - 1; j > i; --j)
		{
			if (OutLootToDrop[j].ItemDefinition != ItemDefinition)
				continue;

			const int32 MergedCount = OutLootToDrop[i].Count + OutLootToDrop[j].Count;
			OutLootToDrop[i].Count = MergedCount < 0 ? 0 : MergedCount;
			OutLootToDrop.RemoveAt(j);
		}
	}
}

bool UFortLootPackage::PickLootDrops(TArray<FFortItemEntry>& OutLootToDrop, uint32 WorldLevel, FName LootTierKey, const FGameplayTagContainer* StaticGameplayTags)
{
	const int32 RangeStart = OutLootToDrop.Num();

	FFortLootTierData* LootTierData = UFortLootTier::FindLootTierDataRow(LootTierKey);
	if (!LootTierData)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": LootTierKey=%s has no FFortLootTierData row in any resolved table", LootTierKey);
		return false;
	}

	if (UFortGameData::GetLootPackageDataTables().empty())
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": no LootPackageData tables resolved, cannot drop anything for LootTierKey=%s", LootTierKey);
		return false;
	}

	const FName LootPackageName = LootTierData->LootPackage;
	const float NumLootPackageDrops = LootTierData->NumLootPackageDrops;

	UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": LootTierKey=%s LootPackage=%s NumLootPackageDrops=%f", LootTierKey, LootPackageName, NumLootPackageDrops);

	if (LootPackageName.IsNone() || NumLootPackageDrops <= 0.0f)
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": LootTierKey=%s has no usable LootPackage (LootPackage=%s NumLootPackageDrops=%f)",
			LootTierKey, LootPackageName, NumLootPackageDrops);
		return false;
	}

	const int32 NumberOfLootDrops = ComputeNumberOfLootDrops(NumLootPackageDrops);

	std::vector<std::pair<int32, int32>> LootMap;
	UFortLootTier::ProcessLootDrops(LootTierData, NumberOfLootDrops, LootMap);

	UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": NumberOfLootDrops=%d spread over %zu categories", NumberOfLootDrops, LootMap.size());

	for (const auto& Category : LootMap)
		for (int32 Drop = 0; Drop < Category.second; ++Drop)
			PickLootDropsFromLootPackage(OutLootToDrop, LootPackageName, static_cast<int32>(WorldLevel), Category.first, StaticGameplayTags);

	const bool bResult = OutLootToDrop.Num() > RangeStart;

	MergeUnstackableEntries(OutLootToDrop);

	UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": result=%s, %d entr(ies) after merging", bResult ? "true" : "false", OutLootToDrop.Num() - RangeStart);

	return bResult;
}

PRISM_NAMESPACE_END
