#include "pch.h"

#include "FortGameData.h"
#include "Items/FortLootTier.h"
#include "Items/FortLootPackage.h"

#include <algorithm>
#include <chrono>

PRISM_NAMESPACE_START

/** How long to wait before trying again once a resolve came back empty. */
static const int32 GLootTableRetrySeconds = 30;

static uint32 GLootTableGeneration = 0;

/**
 * True when Object is still the object the object array has at its index.
 *
 * A cached UDataTable* that has been garbage collected reads as a plausible pointer right up until
 * it is dereferenced, so the check that matters is whether the global object array still agrees
 * that this address is that object.
 */
static bool IsLiveObject(UObject* Object)
{
	if (!Object || !Object->IsValid())
		return false;

	if (!UObject::GObjects)
		return true;

	const int32 InternalIndex = Object->Index;
	if (InternalIndex < 0 || InternalIndex >= UObject::GObjects->Num())
		return false;

	return UObject::GObjects->GetByIndex(InternalIndex) == Object;
}

static bool AreLootTablesUsable(const std::vector<UDataTable*>& Tables, UScriptStruct* RowStruct, bool bLogRejection)
{
	if (Tables.empty() || !RowStruct)
		return false;

	for (UDataTable* Table : Tables)
	{
		if (!IsLiveObject(Table))
		{
			if (bLogRejection)
				UE_LOG(LogPrism, Warning, __FUNCTION__": dropping cached tables, %p is no longer a live UObject", Table);
			return false;
		}

		if (Table->RowStruct != RowStruct)
		{
			if (bLogRejection)
				UE_LOG(LogPrism, Warning, __FUNCTION__": dropping cached tables, %p RowStruct is %p, expected %p", Table, Table->RowStruct, RowStruct);
			return false;
		}
	}

	return true;
}

static void AppendLootTable(const TSoftObjectPtr<UDataTable>& SoftTable, bool bAllowSynchronousLoad, std::vector<UDataTable*>& OutTables)
{
	UDataTable* Table = SoftTable.Get();
	if (!Table && bAllowSynchronousLoad)
		Table = SoftTable.LoadSynchronous();

	if (!Table)
		return;

	if (std::find(OutTables.begin(), OutTables.end(), Table) == OutTables.end())
		OutTables.push_back(Table);
}

static void AppendLootTables(const TArray<TSoftObjectPtr<UDataTable>>& SoftTables, bool bAllowSynchronousLoad, std::vector<UDataTable*>& OutTables)
{
	for (int32 Index = 0; Index < SoftTables.Num(); ++Index)
		AppendLootTable(SoftTables[Index], bAllowSynchronousLoad, OutTables);
}

/**
 * The tables the game data assets declare.
 *
 * In 24.20 only UGameDataBR carries them - UFortGameData kept LootQuotaData and LootLevelData but
 * lost LootTierDataTables and LootPackageDataTables, so there is nothing to read off it here. It is
 * still resolved, and logged once, because it is the asset the equivalent lookup reads on older
 * builds and its absence is worth being able to see.
 */
static void CollectDeclaredLootTables(bool bTierTables, bool bAllowSynchronousLoad, std::vector<UDataTable*>& OutTables)
{
	SDK::UFortGameData* FortGameData = UFortGameData::GetCachedInstance();
	SDK::UGameDataBR* GameDataBR = UFortGameData::GetCachedInstanceBR();

	static bool bLoggedSources = false;
	if (!bLoggedSources)
	{
		bLoggedSources = true;
		UE_LOG(LogPrism, Log, __FUNCTION__": UFortGameData=%p UGameDataBR=%p", FortGameData, GameDataBR);
	}

	if (GameDataBR)
	{
		AppendLootTables(bTierTables ? GameDataBR->LootTierDataTablesBR : GameDataBR->LootPackageDataTablesBR,
			bAllowSynchronousLoad, OutTables);
	}
}

/** The playlist's own pair of tables, which is what a Battle Royale match actually drops from. */
static void CollectPlaylistLootTable(bool bTierTables, bool bAllowSynchronousLoad, std::vector<UDataTable*>& OutTables)
{
	UWorld* World = UWorld::GetWorld();
	if (!World || !World->GameState)
		return;

	UFortPlaylistAthena* Playlist = World->GameState->CurrentPlaylistInfo.BasePlaylist;
	if (!Playlist)
		return;

	AppendLootTable(bTierTables ? Playlist->LootTierData : Playlist->LootPackages, bAllowSynchronousLoad, OutTables);
}

static void CollectLootTables(bool bTierTables, std::vector<UDataTable*>& OutTables)
{
	CollectDeclaredLootTables(bTierTables, false, OutTables);
	CollectPlaylistLootTable(bTierTables, false, OutTables);

	if (!OutTables.empty())
		return;

	UE_LOG(LogPrism, Warning, __FUNCTION__": no %s tables are loaded, falling back to synchronous asset loads",
		bTierTables ? "LootTierData" : "LootPackageData");

	CollectDeclaredLootTables(bTierTables, true, OutTables);
	CollectPlaylistLootTable(bTierTables, true, OutTables);
}

struct FLootTableCache
{
	std::vector<UDataTable*> Tables;
	std::chrono::steady_clock::time_point NextResolveTime;
};

static FLootTableCache GTierCache;
static FLootTableCache GPackageCache;

/**
 * The UScriptStruct a table of this kind has to hold its rows as.
 *
 * The generated structs carry no StaticStruct() of their own - they are plain layouts, not
 * reflected types - so the script struct is looked up by name once and remembered. It is what
 * catches a table that resolved to the wrong asset before its rows are reinterpreted as the
 * wrong type.
 */
static UScriptStruct* GetLootRowStruct(bool bTierTables)
{
	static UScriptStruct* TierRowStruct = nullptr;
	static UScriptStruct* PackageRowStruct = nullptr;

	UScriptStruct*& RowStruct = bTierTables ? TierRowStruct : PackageRowStruct;
	if (!RowStruct)
	{
		RowStruct = UObject::FindObjectFast<UScriptStruct>(
			bTierTables ? "FortLootTierData" : "FortLootPackageData", EClassCastFlags::ScriptStruct);
	}

	return RowStruct;
}

static const std::vector<UDataTable*>& GetCachedLootTables(bool bTierTables)
{
	UScriptStruct* RowStruct = GetLootRowStruct(bTierTables);

	FLootTableCache& Cache = bTierTables ? GTierCache : GPackageCache;
	std::vector<UDataTable*>& Tables = Cache.Tables;

	if (AreLootTablesUsable(Tables, RowStruct, false))
		return Tables;

	const auto Now = std::chrono::steady_clock::now();
	if (Now < Cache.NextResolveTime)
		return Tables;

	AreLootTablesUsable(Tables, RowStruct, true);

	Tables.clear();
	CollectLootTables(bTierTables, Tables);

	if (Tables.empty())
	{
		Cache.NextResolveTime = Now + std::chrono::seconds(GLootTableRetrySeconds);
		UE_LOG(LogPrism, Warning, __FUNCTION__": no %s tables resolved, not retrying for %d seconds",
			bTierTables ? "LootTierData" : "LootPackageData", GLootTableRetrySeconds);
		return Tables;
	}

	Cache.NextResolveTime = std::chrono::steady_clock::time_point();
	++GLootTableGeneration;

	std::string Names;
	for (UDataTable* Table : Tables)
	{
		if (!Names.empty())
			Names += ", ";
		Names += Table->GetName();
	}

	UE_LOG(LogPrism, Log, __FUNCTION__": resolved %zu %s table(s) [%s], generation=%u",
		Tables.size(), bTierTables ? "LootTierData" : "LootPackageData", Names.c_str(), GLootTableGeneration);

	return Tables;
}

SDK::UFortGameData* UFortGameData::GetCachedInstance()
{
	SDK::UGameEngine* Engine = UFortEngine::GetEngine();
	if (!Engine || !Engine->AssetManager)
		return nullptr;

	return Engine->AssetManager->GameDataCommon;
}

SDK::UGameDataBR* UFortGameData::GetCachedInstanceBR()
{
	SDK::UGameEngine* Engine = UFortEngine::GetEngine();
	if (!Engine || !Engine->AssetManager)
		return nullptr;

	return Engine->AssetManager->GameDataBR;
}

const std::vector<UDataTable*>& UFortGameData::GetLootTierDataTables()
{
	return GetCachedLootTables(true);
}

const std::vector<UDataTable*>& UFortGameData::GetLootPackageDataTables()
{
	return GetCachedLootTables(false);
}

uint32 UFortGameData::GetLootTableGeneration()
{
	return GLootTableGeneration;
}

void UFortGameData::InvalidateLootTables()
{
	GTierCache.Tables.clear();
	GPackageCache.Tables.clear();
	++GLootTableGeneration;

	UE_LOG(LogPrism, Log, __FUNCTION__": loot tables invalidated, generation=%u", GLootTableGeneration);
}

FFortLootTierData* UFortGameData::FindLootTierDataRow(FName LootTierKey)
{
	return UFortLootTier::FindLootTierDataRow(LootTierKey);
}

bool UFortGameData::CheckLootPackageDataTables()
{
	return !GetLootPackageDataTables().empty();
}

PRISM_NAMESPACE_END
