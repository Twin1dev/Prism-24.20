#include "pch.h"

#include "FortKismetLibrary.h"
#include "Items/FortLootTier.h"
#include "Items/FortLootPackage.h"
#include "UObject/Stack.h"

#include <vector>

PRISM_NAMESPACE_START

/**
 * How many times a tier group is re-rolled before giving up.
 *
 * A tier key can resolve to a loot package that drops nothing - the package row exists but its
 * items filter out - so the roll is retried with the keys already tried excluded. Four is what the
 * engine allows itself.
 */
static const int32 GMaxLootTierAttempts = 4;

bool UFortKismetLibrary::PickLootDrops(UObject* WorldContextObject, TArray<FFortItemEntry>& OutLootToDrop, FName TierGroupName, int32 WorldLevel, int32 ForcedLootTier)
{
	(void)WorldContextObject;

	UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": TierGroupName=%s WorldLevel=%d ForcedLootTier=%d", TierGroupName, WorldLevel, ForcedLootTier);

	if (TierGroupName.IsNone())
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroupName is None, nothing to pick");
		return false;
	}

	FGameplayTagContainer StaticGameplayTags{};
	std::vector<FName> AttemptedTierKeys;

	for (int32 Attempt = 0; Attempt < GMaxLootTierAttempts; ++Attempt)
	{
		FName LootTierKey;
		int32 LootTier = -1;

		if (!UFortLootTier::PickLootTierKeyAndLootTierFromTierGroup(LootTierKey, LootTier, TierGroupName, static_cast<uint32>(WorldLevel), ForcedLootTier))
			break;

		bool bAlreadyAttempted = false;
		for (const FName& Key : AttemptedTierKeys)
			bAlreadyAttempted |= Key == LootTierKey;

		if (bAlreadyAttempted)
			continue;

		AttemptedTierKeys.push_back(LootTierKey);

		UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": attempt %d resolved LootTierKey=%s LootTier=%d", Attempt, LootTierKey, LootTier);

		if (UFortLootPackage::PickLootDrops(OutLootToDrop, static_cast<uint32>(WorldLevel), LootTierKey, &StaticGameplayTags))
		{
			UE_LOG_DEBUG(LogPrism, Log, __FUNCTION__": TierGroupName=%s produced %d entr(ies)", TierGroupName, OutLootToDrop.Num());
			return true;
		}
	}

	UE_LOG(LogPrism, Warning, __FUNCTION__": TierGroupName=%s produced no loot after %zu tier key(s)", TierGroupName, AttemptedTierKeys.size());

	return false;
}

/*-----------------------------------------------------------------------------
	Installing it
-----------------------------------------------------------------------------*/

/**
 * void UFortKismetLibrary::execPickLootDrops(UObject* Context, FFrame& Stack, void* const Result)
 *
 * 0xA10BC68 in
 *   C:\NewDumps\24.20\24.20\FortniteClient-Win64-Shipping_dump_SCY.exe.i64
 * (++Fortnite+Release-24.20-CL-25156858). Found from the "PickLootDrops" string at 0xE160C38: its
 * data cross-reference at 0xC89BBC0 is the FNameNativePtrPair the class registers itself with, and
 * the exec function is the qword after the name pointer. The database carries the mangled symbol
 * _ZN18UFortKismetLibrary17execPickLootDropsEv, which confirms it.
 *
 * The important thing about that thunk is what it does *not* do. It steps all five parameters off
 * the frame exactly as a generated thunk should - WorldContextObject, the TArray by reference, the
 * tier group name and the two ints - and then, with every one of them unpacked and sitting in
 * locals, it writes false into the result and returns without calling anything:
 *
 *     mov  [rdi], sil        ; sil is 0, from the xor esi, esi at the top
 *     call sub_1972AFC       ; the local TArray's destructor
 *     retn
 *
 * The C++ implementation behind it is inside WITH_SERVER_CODE and was compiled out of the client
 * executable, so there is no call site to redirect and no virtual slot to swap: the reimplementation
 * has to *be* the exec function. That is why this file carries its own parameter unpacking rather
 * than using FScriptFunctionOverride, which resolves a thunk to the implementation it calls and
 * would find nothing here.
 */
static const uint64 GExecPickLootDropsRva = 0x0A10BC68;

static UE::Interception::FNativeFunctionOverride GExecPickLootDropsOverride;

/**
 * The replacement exec function, shaped exactly like the thunk it displaces.
 *
 * Nothing chains to the original: the original returns false and drops the parameters on the floor,
 * so there is nothing there to fall back to.
 */
static void ExecPickLootDrops(UObject* Context, FFrame& Stack, void* const Result)
{
	(void)Context;

	UObject* WorldContextObject = nullptr;
	Stack.StepCompiledIn(&WorldContextObject);

	// Only used when the step did not resolve to the caller's array, which is the ProcessEvent
	// shape; from Blueprint the reference comes back through MostRecentPropertyAddress.
	TArray<FFortItemEntry> LocalLootToDrop{};
	TArray<FFortItemEntry>& OutLootToDrop = Stack.StepCompiledInRef<TArray<FFortItemEntry>>(&LocalLootToDrop);

	FName TierGroupName{};
	Stack.StepCompiledIn(&TierGroupName);

	int32 WorldLevel = 0;
	Stack.StepCompiledIn(&WorldLevel);

	int32 ForcedLootTier = 0;
	Stack.StepCompiledIn(&ForcedLootTier);

	Stack.Finish();

	*static_cast<bool*>(Result) = UFortKismetLibrary::PickLootDrops(WorldContextObject, OutLootToDrop, TierGroupName, WorldLevel, ForcedLootTier);
}

void UFortKismetLibrary::Initialize()
{
	if (!GExecPickLootDropsOverride.InstallAtRelativeAddress(GExecPickLootDropsRva, &ExecPickLootDrops).IsValid())
	{
		UE_LOG(LogPrism, Warning, __FUNCTION__": could not detour execPickLootDrops at 0x%llX, loot will not drop", GExecPickLootDropsRva);
		return;
	}

	UE_LOG(LogPrism, Log, __FUNCTION__": execPickLootDrops detoured at 0x%llX", GExecPickLootDropsRva);
}

IMPLEMENT_MODULE(UFortKismetLibrary);

PRISM_NAMESPACE_END
