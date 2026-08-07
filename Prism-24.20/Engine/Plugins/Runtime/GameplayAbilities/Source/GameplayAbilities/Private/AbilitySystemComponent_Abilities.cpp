#include "pch.h"

#include <GameplayAbilities/Source/GameplayAbilities/Public/AbilitySystemComponent.h>

PRISM_NAMESPACE_START

#define ABILITY_LOG(Verbosity, Format, ...) \
{ \
	UE_LOG(LogAbilitySystem, Verbosity, Format, ##__VA_ARGS__); \
}

/**
 * Verbose is off in a shipping engine log and this module's UE_LOG has no verbosity filter, so the
 * lines the engine logs at Verbose go through the debug macro instead of printing on every call.
 */
#define ABILITY_VLOG(Format, ...) UE_LOG_DEBUG(LogAbilitySystem, Verbose, Format, ##__VA_ARGS__)

/** *GetNameSafe(Object) - the object's name, or "None" when there is no object. */
static std::string GetNameSafe(const UObject* Object)
{
	return Object ? Object->GetName() : std::string("None");
}

/**
 * FGameplayTagContainer::ToStringSimple - the tag names separated by ", ".
 * The container is only ever read here for a log line, so this stays with the caller.
 */
static std::string ToStringSimple(const FGameplayTagContainer& Tags)
{
	std::string Result;
	for (const FGameplayTag& Tag : Tags.GameplayTags)
	{
		if (!Result.empty())
		{
			Result += ", ";
		}

		Result += Tag.TagName.ToString();
	}

	return Result;
}

void UAbilitySystemComponent::ConsumeAllReplicatedData(FGameplayAbilitySpecHandle AbilityHandle, FPredictionKey AbilityOriginalPredictionKey)
{
	FAbilityReplicatedDataCache* CachedData = GetAbilityTargetDataMap().Find(FGameplayAbilitySpecHandleAndPredictionKey{ AbilityHandle, AbilityOriginalPredictionKey.Current });
	if (CachedData)
	{
		CachedData->Reset();
	}
}

void UAbilitySystemComponent::MarkAbilitySpecDirty(FGameplayAbilitySpec& Spec, bool WasAddOrRemove)
{
	if (IsOwnerActorAuthoritative())
	{
		if (!(Spec.Ability && Spec.Ability->NetExecutionPolicy == EGameplayAbilityNetExecutionPolicy::ServerOnly && !WasAddOrRemove))
		{
			bIsNetDirty = true;
			ActivatableAbilities.MarkItemDirty(Spec);
		}
		// TODO: Broadcast
		// AbilitySpecDirtiedCallbacks.Broadcast(Spec);
	}
	else
	{
		ActivatableAbilities.MarkArrayDirty();
	}
}

FGameplayAbilitySpec* UAbilitySystemComponent::FindAbilitySpecFromHandle(FGameplayAbilitySpecHandle Handle)
{
	for (FGameplayAbilitySpec& Spec : ActivatableAbilities.Items)
	{
		if (Spec.Handle == Handle)
		{
			return &Spec;
		}
	}

	return nullptr;
}

void UAbilitySystemComponent::InternalServerTryActivateAbility(FGameplayAbilitySpecHandle Handle, bool InputPressed, const FPredictionKey& PredictionKey, const FGameplayEventData* TriggerEventData)
{
	// The DenyClientActivation cheat above this in the engine is !UE_BUILD_SHIPPING only, so it is
	// not reproduced here.
	ABILITYLIST_SCOPE_LOCK();

	FGameplayAbilitySpec* Spec = FindAbilitySpecFromHandle(Handle);
	if (!Spec)
	{
		// Can potentially happen in race conditions where client tries to activate ability that is removed server side before it is received.
		ABILITY_LOG(Display, "InternalServerTryActivateAbility: Rejecting ClientActivation of ability with invalid SpecHandle!");
		ClientActivateAbilityFailed(Handle, PredictionKey.Current);
		return;
	}

	const UGameplayAbility* AbilityToActivate = Spec->Ability;

	if (!AbilityToActivate)
	{
		ABILITY_LOG(Error, "InternalServerTryActivateAbility: Rejecting ClientActivation of unconfigured spec ability!");
		ClientActivateAbilityFailed(Handle, PredictionKey.Current);
		return;
	}

	/*
	 * UE 5.8 rejects a client activating an ability whose NetSecurityPolicy is ServerOnlyExecution
	 * or ServerOnly here. That check post-dates this build - 24.20 is 5.1, and the shape of this
	 * function in 5.1 (which the working reference server also mirrors) has no such branch - so it
	 * is deliberately not reproduced. It is the only branch that could newly refuse an activation
	 * that the engine would have accepted, which is not a risk worth taking on an unverifiable
	 * version difference.
	 */

	// Consume any pending target info, to clear out cancels from old executions
	ConsumeAllReplicatedData(Handle, PredictionKey);

	FScopedPredictionWindow ScopedPredictionWindow(this, PredictionKey);

	if (!IsAbilityActorInfoValid())
	{
		// ensure(AbilityActorInfo.IsValid()) - the engine carries on regardless, and so does this.
		ABILITY_LOG(Error, "InternalServerTryActivateAbility: AbilityActorInfo is not valid.");
	}

	UGameplayAbility* InstancedAbility = nullptr;
	Spec->InputPressed = true;

	// Attempt to activate the ability (server side) and tell the client if it succeeded or failed.
	if (InternalTryActivateAbility(Handle, PredictionKey, &InstancedAbility, TriggerEventData))
	{
		// TryActivateAbility handles notifying the client of success, but let's still log it
		// Diagnostic: the engine logs this at Verbose. It is at Display while the melee target-data
		// path is being chased, so that a silent log means "never called" rather than "succeeded".
		ABILITY_LOG(Display, "InternalServerTryActivateAbility: Accepted ClientActivation of %s. Handle %d, PredictionKey %d (base %d, stale %d).",
			GetNameSafe(Spec->Ability).c_str(), Handle.Handle, PredictionKey.Current, PredictionKey.Base, PredictionKey.bIsStale ? 1 : 0);
	}
	else
	{
		ABILITY_LOG(Display, "InternalServerTryActivateAbility: Rejecting ClientActivation of %s. Handle %d, PredictionKey %d. InternalTryActivateAbility failed: %s",
			GetNameSafe(Spec->Ability).c_str(), Handle.Handle, PredictionKey.Current, ToStringSimple(GetInternalTryActivateAbilityFailureTags()).c_str());

		ClientActivateAbilityFailed(Handle, PredictionKey.Current);
		Spec->InputPressed = false;

		MarkAbilitySpecDirty(*Spec);
	}
}

/**
 * DIAGNOSTIC ONLY - see the declaration. Logs what the client actually sent and whether anything on
 * the server is listening for it, then hands straight over to the engine.
 */
void UAbilitySystemComponent::ServerSetReplicatedTargetData_Implementation(FGameplayAbilitySpecHandle AbilityHandle, const FPredictionKey& AbilityOriginalPredictionKey,
	const FGameplayAbilityTargetDataHandle& ReplicatedTargetDataHandle, FGameplayTag ApplicationTag, const FPredictionKey& CurrentPredictionKey)
{
	const FAbilityReplicatedDataCache* CachedData = GetAbilityTargetDataMap().Find(
		FGameplayAbilitySpecHandleAndPredictionKey{ AbilityHandle, AbilityOriginalPredictionKey.Current });

	// Silent when the path is healthy. This only speaks up on the same condition the engine logs
	// "is overriding pending replicated target data" on, and says why: a listener count of zero
	// means no ability task on the server is bound to this handle and prediction key.
	if (CachedData && CachedData->GetTargetDataNum() > 0)
	{
		ABILITY_LOG(Warning, "ServerSetReplicatedTargetData on %s: Handle %d, OriginalPredictionKey %d, CurrentPredictionKey %d, incoming data %d, already pending %d, listeners %d.",
			Class ? Class->GetName().c_str() : "<no class>",
			AbilityHandle.Handle,
			AbilityOriginalPredictionKey.Current,
			CurrentPredictionKey.Current,
			GetTargetDataNum(ReplicatedTargetDataHandle),
			CachedData->GetTargetDataNum(),
			CachedData->GetTargetSetListenerNum());
	}

	ServerSetReplicatedTargetData_Implementation_Original(this, AbilityHandle, AbilityOriginalPredictionKey, ReplicatedTargetDataHandle, ApplicationTag, CurrentPredictionKey);
}

void UAbilitySystemComponent::Initialize()
{
	/*
	 * One override object per class, and never one object reused.
	 *
	 * FScriptFunctionOverride keeps a single Original and so refuses a second install outright -
	 * InstallAtVirtualIndex opens with `if (IsInstalled() || ...) return false`. Three calls on the
	 * macro's single _Override instance therefore patched UAbilitySystemComponent and silently did
	 * nothing for the two Fortnite classes, which are the pair that matters: an Athena player's
	 * component is a UFortAbilitySystemComponentAthena, so its slot 0x115 kept the engine's empty
	 * WITH_SERVER_CODE stub (0xDB0CF0) and no client-initiated ability activated server-side.
	 *
	 * All three classes hold that same stub at 0x115, so sharing one _Original between them is
	 * safe. That is not true of every slot - 175 below differs between the engine and Fortnite
	 * classes - so check the vtables before reusing this shape elsewhere.
	 */
	static FInternalServerTryActivateAbilityScriptOverride FortOverride;
	static FInternalServerTryActivateAbilityScriptOverride FortAthenaOverride;

	struct FVirtualInstall
	{
		const char* ClassName;
		FInternalServerTryActivateAbilityScriptOverride& Override;
	};

	const FVirtualInstall Installs[] =
	{
		{ "AbilitySystemComponent",           InternalServerTryActivateAbility_Override },
		{ "FortAbilitySystemComponent",       FortOverride },
		{ "FortAbilitySystemComponentAthena", FortAthenaOverride },
	};

	for (const FVirtualInstall& Install : Installs)
	{
		// A refused install is otherwise indistinguishable from an override that is in place but
		// never called, which is how this went unnoticed.
		if (!Install.Override.InstallAtVirtualIndex(Install.ClassName, 0x115))
		{
			ABILITY_LOG(Error, "Initialize: failed to install InternalServerTryActivateAbility on %s at slot 0x115.", Install.ClassName);
		}
		else
		{
			ABILITY_LOG(Display, "Initialize: installed InternalServerTryActivateAbility on %s at slot 0x115.", Install.ClassName);
		}
	}

	/*
	 * DIAGNOSTIC ONLY. Slot 175 is not shared the way 0x115 is: UAbilitySystemComponent holds the
	 * engine's implementation at 0x8597980, while both Fortnite classes hold an override at
	 * 0x9680F18 that brackets a static call to it with a flag at +0x1700. One override object keeps
	 * a single Original, so this is installed on the Athena component alone - the one an Athena
	 * player's pawn actually uses - rather than chaining two of three classes to the wrong function.
	 */
	if (!ServerSetReplicatedTargetData_Implementation_Override.InstallAtVirtualIndex("FortAbilitySystemComponentAthena", 175))
	{
		ABILITY_LOG(Error, "Initialize: failed to install ServerSetReplicatedTargetData on FortAbilitySystemComponentAthena at slot 175.");
	}
}

IMPLEMENT_MODULE(UAbilitySystemComponent);

PRISM_NAMESPACE_END