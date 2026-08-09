#pragma once

/*=============================================================================
	FortKismetLibrary.h: The loot entry point the rest of the game calls.

	UFortKismetLibrary::PickLootDrops is what a chest, a floor spawner, a
	supply drop and every Blueprint that drops loot all go through. Its
	implementation is not in this binary (see FortKismetLibrary.cpp), so this
	is both the reimplementation and the thing that puts it back behind the
	name everything already calls.
=============================================================================*/

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

/**
 * The Prism half of UFortKismetLibrary.
 *
 * SDK::UFortKismetLibrary is 'final' and this adds only statics, so there is nothing to extend and
 * no object to extend it on. Note that the name shadows the generated one for any translation unit
 * that includes this header: reach the generated statics as SDK::UFortKismetLibrary::Whatever there.
 */
class UFortKismetLibrary
{
public:
	/**
	 * Fills OutLootToDrop with what TierGroupName drops at this world level.
	 *
	 * ForcedLootTier of -1 rolls the tier as well; anything else is a ceiling, and the highest tier
	 * at or below it that has rows is used. Returns false, having added nothing, when the tier group
	 * resolves to no rows at all.
	 */
	static bool PickLootDrops(UObject* WorldContextObject, TArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32 WorldLevel, int32 ForcedLootTier);

	static void Initialize();
};

PRISM_NAMESPACE_END
