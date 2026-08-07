#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UAbilitySystemComponent;

/*-----------------------------------------------------------------------------
	Non-reflected members of UAbilitySystemComponent

	None of these are UPROPERTYs, so the generated headers do not name them and
	they are reached by offset. Every offset below was read out of the shipping
	binary and cross-checked against the reflected members either side of it in
	GameplayAbilities_classes.hpp, which agree exactly up to 0x860.
-----------------------------------------------------------------------------*/
namespace AbilitySystemComponentLayout
{
	/**
	 * FGameplayTagContainer InternalTryActivateAbilityFailureTags.
	 * Sits in the generated Pad_2E0[0x20], between IncomingDuration (0x2DC) and ClientDebugStrings (0x300).
	 */
	inline constexpr int32 InternalTryActivateAbilityFailureTags = 0x2E0;

	/**
	 * TSharedPtr<FGameplayAbilityActorInfo> AbilityActorInfo.
	 * The generated Pad_3D0[0x10] between AvatarActor (0x3C8) and ActivatableAbilities (0x3E0).
	 */
	inline constexpr int32 AbilityActorInfo = 0x3D0;

	/**
	 * FGameplayAbilityReplicatedDataContainer AbilityTargetDataMap.
	 * The first half of the generated Pad_500[0x30]: ActivatableAbilities ends at 0x500 and
	 * AllReplicatedInstancedAbilities begins at 0x530, leaving the container (0x20) plus
	 * GameplayEventTagContainerDelegates (0x10). ServerSetReplicatedTargetDataCancelled_Implementation
	 * calls FindOrAdd as `sub_858840C(this + 1280, ...)`, and 1280 == 0x500.
	 */
	inline constexpr int32 AbilityTargetDataMap = 0x500;

	/**
	 * int32 AbilityScopeLockCount, the depth of the ability list lock.
	 * ClearAllAbilities increments `*(int32*)(this + 1344)` on entry and DecrementAbilityListLock
	 * decrements the same word, so the count is at 0x540 - the start of the generated Pad_540.
	 */
	inline constexpr int32 AbilityScopeLockCount = 0x540;
}

/**
 * The per-(ability, prediction key) cache of data a client replicated up for an activation:
 * target data, the confirm/cancel flags, the generic replicated events and the prediction key.
 *
 * Not a reflected type. Only Reset() is needed here, and it clears twelve generic event entries
 * as well as the target data, so the engine's own implementation is called rather than repeated.
 */
struct FAbilityReplicatedDataCache
{
	void Reset()
	{
		static void (*Function)(FAbilityReplicatedDataCache*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x85962C0);
		Function(this);
	}

	/**
	 * TargetData.Data.Num(). TargetData is the FGameplayAbilityTargetDataHandle at 0x8, whose array
	 * uses a TInlineAllocator<1>, putting its count at 0x20 into the handle - which is the same
	 * `*(int*)(Cache + 40)` the engine tests before it logs "is overriding pending replicated
	 * target data".
	 */
	int32 GetTargetDataNum() const { return ReadInt32(0x28); }

	/**
	 * TargetSetDelegate.InvocationList.Num(). The delegate is at 0x38 and a multicast delegate
	 * begins with its invocation list, so the count is at 0x40. Zero means nothing on the server is
	 * waiting for this data - i.e. no ability task is bound under this handle and prediction key.
	 */
	int32 GetTargetSetListenerNum() const { return ReadInt32(0x40); }

private:
	int32 ReadInt32(int32 Offset) const { return *reinterpret_cast<const int32*>(reinterpret_cast<const uint8*>(this) + Offset); }
};

/** FGameplayAbilityTargetDataHandle::Data.Num() - see FAbilityReplicatedDataCache::GetTargetDataNum. */
inline int32 GetTargetDataNum(const FGameplayAbilityTargetDataHandle& Handle)
{
	return *reinterpret_cast<const int32*>(reinterpret_cast<const uint8*>(&Handle) + 0x20);
}

/**
 * UAbilitySystemComponent::AbilityTargetDataMap - despite the name, a linear array of in-use
 * entries keyed by ability handle plus the prediction key the activation was created under,
 * with a free list behind it. Entries are recycled rather than destroyed, so the cache an entry
 * owns outlives any Reset() of it.
 */
struct FGameplayAbilityReplicatedDataContainer
{
	/** TSharedPtr/TSharedRef<FAbilityReplicatedDataCache> - the object and its reference controller. */
	struct FSharedDataRef
	{
		FAbilityReplicatedDataCache* Object;
		void*                        ReferenceController;
	};

	/** TPair<FGameplayAbilitySpecHandleAndPredictionKey, TSharedRef<FAbilityReplicatedDataCache>>. */
	struct FKeyDataPair
	{
		FGameplayAbilitySpecHandleAndPredictionKey Key;
		FSharedDataRef                             Value;
	};

	/**
	 * The engine hands back a TSharedPtr; the array entry holds a reference for as long as the
	 * entry is in use and Reset() does not remove it, so the raw pointer is all a caller needs.
	 */
	FAbilityReplicatedDataCache* Find(const FGameplayAbilitySpecHandleAndPredictionKey& Key) const
	{
		for (const FKeyDataPair& Pair : InUseData)
		{
			if (Pair.Key.AbilityHandle == Key.AbilityHandle && Pair.Key.PredictionKeyAtCreation == Key.PredictionKeyAtCreation)
			{
				return Pair.Value.Object;
			}
		}

		return nullptr;
	}

	TArray<FKeyDataPair>  InUseData;
	TArray<FSharedDataRef> FreeData;
};
static_assert(sizeof(FGameplayAbilityReplicatedDataContainer::FKeyDataPair) == 0x18,
	"FGameplayAbilityReplicatedDataContainer entries are 0x18 bytes in the shipping binary; Find would walk the array with the wrong stride.");
static_assert(sizeof(FGameplayAbilityReplicatedDataContainer) == 0x20,
	"FGameplayAbilityReplicatedDataContainer must be 0x20, or it overlaps GameplayEventTagContainerDelegates at 0x520.");

/**
 * Swaps the component's scoped prediction key for the one a client sent, and puts the previous
 * key back at end of scope - replicating the key to the client on the way out when it was set.
 *
 * The engine's constructor and destructor are called on a raw 0x40 buffer rather than the members
 * being reproduced here: the destructor reaches into ReplicatedPredictionKeyMap, and the point of
 * the type is that the two halves stay in step.
 */
struct FScopedPredictionWindow
{
	explicit FScopedPredictionWindow(UAbilitySystemComponent* AbilitySystemComponent, const FPredictionKey& InPredictionKey, bool bInSetReplicatedPredictionKey = true)
	{
		static void (*Constructor)(void*, UAbilitySystemComponent*, const FPredictionKey&, bool) = decltype(Constructor)(InSDKUtils::GetImageBase() + 0x85A7634);
		Constructor(Storage, AbilitySystemComponent, InPredictionKey, bInSetReplicatedPredictionKey);
	}

	~FScopedPredictionWindow()
	{
		static void (*Destructor)(void*) = decltype(Destructor)(InSDKUtils::GetImageBase() + 0x22922C4);
		Destructor(Storage);
	}

	FScopedPredictionWindow(const FScopedPredictionWindow&) = delete;
	FScopedPredictionWindow& operator=(const FScopedPredictionWindow&) = delete;

private:
	alignas(8) uint8 Storage[0x40];
};

class UAbilitySystemComponent : public UE_EXTENDS_SDK_CLASS(UAbilitySystemComponent)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UAbilitySystemComponent);

	bool IsOwnerActorAuthoritative() const
	{
		return !bCachedIsNetSimulated;
	}

	bool InternalTryActivateAbility(FGameplayAbilitySpecHandle AbilityToActivate, FPredictionKey InPredictionKey = FPredictionKey(), UGameplayAbility * *OutInstancedAbility = nullptr, const FGameplayEventData * TriggerEventData = nullptr)
	{
		static bool (*Function)(UAbilitySystemComponent*, FGameplayAbilitySpecHandle, FPredictionKey, UGameplayAbility**, void*, const FGameplayEventData*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x858DF4C);
		return Function(this, AbilityToActivate, InPredictionKey, OutInstancedAbility, nullptr, TriggerEventData);;
	}

	/*-------------------------------------------------------------------------
		Non-reflected members - see AbilitySystemComponentLayout above
	-------------------------------------------------------------------------*/

	FGameplayAbilityReplicatedDataContainer& GetAbilityTargetDataMap() const
	{
		return GetMemberAtOffset<FGameplayAbilityReplicatedDataContainer>(AbilitySystemComponentLayout::AbilityTargetDataMap);
	}

	/** The tags InternalTryActivateAbility left behind describing why an activation was refused. */
	const FGameplayTagContainer& GetInternalTryActivateAbilityFailureTags() const
	{
		return GetMemberAtOffset<FGameplayTagContainer>(AbilitySystemComponentLayout::InternalTryActivateAbilityFailureTags);
	}

	/** TSharedPtr<FGameplayAbilityActorInfo>::IsValid - the shared pointer's object, nothing more. */
	bool IsAbilityActorInfoValid() const
	{
		return GetMemberAtOffset<void*>(AbilitySystemComponentLayout::AbilityActorInfo) != nullptr;
	}

	/*-------------------------------------------------------------------------
		Ability list lock

		Held across anything that walks ActivatableAbilities.Items and may run
		user code, because adds and removes reallocate that array. While the
		count is non-zero the component defers them; the last decrement is what
		applies them, so it is the engine's own implementation that is called.
	-------------------------------------------------------------------------*/

	void IncrementAbilityListLock()
	{
		++GetMemberAtOffset<int32>(AbilitySystemComponentLayout::AbilityScopeLockCount);
	}

	void DecrementAbilityListLock()
	{
		static void (*Function)(UAbilitySystemComponent*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x138C1B0);
		Function(this);
	}

	void ConsumeAllReplicatedData(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey);
	void MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec, bool WasAddOrRemove = false);
	FGameplayAbilitySpec* FindAbilitySpecFromHandle(FGameplayAbilitySpecHandle Handle);

	void InternalServerTryActivateAbility(FGameplayAbilitySpecHandle Handle, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(InternalServerTryActivateAbility);

	/**
	 * DIAGNOSTIC ONLY - chains straight to the engine after logging, and can be deleted along with
	 * its install in Initialize().
	 *
	 * This is the one function in the melee path that is known to be running, so it is where the
	 * question "which prediction key is the client sending, and is anything on the server bound to
	 * it" can actually be answered. Vtable slot 175, read out of execServerSetReplicatedTargetData
	 * (`call qword ptr [rax+578h]`).
	 */
	void ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle, const FPredictionKey& AbilityOriginalPredictionKey,
		const FGameplayAbilityTargetDataHandle& ReplicatedTargetDataHandle, FGameplayTag ApplicationTag, const FPredictionKey& CurrentPredictionKey);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerSetReplicatedTargetData_Implementation);

	static void Initialize();

private:
	template<typename MemberType>
	MemberType& GetMemberAtOffset(int32 Offset) const
	{
		return *reinterpret_cast<MemberType*>(reinterpret_cast<uint8*>(const_cast<UAbilitySystemComponent*>(this)) + Offset);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(UAbilitySystemComponent);
UE_DECLARE_PRISM_TYPE(UAbilitySystemComponent);

/** Holds the ability list lock for the enclosing scope. Use through ABILITYLIST_SCOPE_LOCK. */
struct FScopedAbilityListLock
{
	explicit FScopedAbilityListLock(UAbilitySystemComponent& InAbilitySystemComponent)
		: AbilitySystemComponent(InAbilitySystemComponent)
	{
		AbilitySystemComponent.IncrementAbilityListLock();
	}

	~FScopedAbilityListLock()
	{
		AbilitySystemComponent.DecrementAbilityListLock();
	}

	FScopedAbilityListLock(const FScopedAbilityListLock&) = delete;
	FScopedAbilityListLock& operator=(const FScopedAbilityListLock&) = delete;

private:
	UAbilitySystemComponent& AbilitySystemComponent;
};

#define ABILITYLIST_SCOPE_LOCK() FScopedAbilityListLock ActiveScopeLock(*this)

PRISM_NAMESPACE_END
