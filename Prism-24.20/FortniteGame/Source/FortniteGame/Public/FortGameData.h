#pragma once

/*=============================================================================
	FortGameData.h: Where the loot tables come from.

	UFortLootTier and UFortLootPackage both need the same thing - the set of
	UDataTables the current playlist and game data declare - so resolving them
	lives here rather than in either of them. The tables are cached, validated
	against the row struct they are supposed to hold, and re-resolved when the
	cache goes stale.
=============================================================================*/

#include "CoreGlobals.h"

#include <vector>

PRISM_NAMESPACE_START

/**
 * Static access to the loot tables.
 *
 * Named after the asset it mostly reads. Nothing here holds state that belongs
 * to an instance, so unlike the rest of the module this is not an extension of
 * the generated class - SDK::UFortGameData is 'final', and there is nothing to
 * extend it with anyway.
 */
class UFortGameData
{
public:
	/** The game data assets the asset manager loaded, or null before it has. */
	static SDK::UFortGameData* GetCachedInstance();
	static SDK::UGameDataBR* GetCachedInstanceBR();

	static const std::vector<UDataTable*>& GetLootTierDataTables();
	static const std::vector<UDataTable*>& GetLootPackageDataTables();

	/**
	 * Bumped every time the resolved tables change.
	 *
	 * UFortLootTier and UFortLootPackage both index their tables by row name and cache the index;
	 * this is how they know to throw the index away.
	 */
	static uint32 GetLootTableGeneration();
	static void InvalidateLootTables();

	static struct FFortLootTierData* FindLootTierDataRow(FName LootTierKey);
	static bool CheckLootPackageDataTables();
};

PRISM_NAMESPACE_END
