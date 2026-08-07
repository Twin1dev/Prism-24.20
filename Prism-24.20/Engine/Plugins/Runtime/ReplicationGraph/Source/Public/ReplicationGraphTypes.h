#pragma once

/*=============================================================================
	ReplicationGraphTypes.h

	The associative data the Replication Graph keeps about actors, the list
	types it pushes around, and the parameter structures the gather phase runs
	on. Laid out as UE 5.1 lays them out - which is the version Fortnite
	Release-24.20 (CL-25156858) is built from - not as later versions do.

	Two differences from the engine's file are deliberate and worth stating up
	front, because they are forced by running inside a shipped binary rather
	than being choices:

	  * The graph's nodes are plain C++ objects here, not UObjects. Extending a
	    generated SDK class cannot add data members - the object's storage is
	    exactly the size the game's UClass says it is - and the offsets Epic's
	    members actually live at cannot be recovered, because the bodies that
	    would reference them were compiled out of the client. Class names,
	    hierarchy and responsibilities are unchanged; only their allocation is.

	  * FActorRepListRefView is a TArray, matching UE 5.1 (which replaced the
	    pooled, ref-counted FActorRepList of earlier versions). This is
	    corroborated by the shipped class sizes: UReplicationGraphNode_ActorList
	    is 0xD0, which is 0x50 of UReplicationGraphNode plus a 0x10 list plus a
	    0x70 FStreamingLevelActorListCollection - and only works out if the view
	    is 16 bytes.
=============================================================================*/

#include "CoreGlobals.h"

#include "Containers/Array.h"
#include "Containers/Map.h"
#include "Delegates/DelegateCombinations.h"
#include "Engine/NetConnection.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"
#include "Templates/SharedPointer.h"
#include "UObject/ObjectKey.h"

PRISM_NAMESPACE_START

class UActorChannel;
class UNetConnection;
class UNetReplicationGraphConnection;
class UReplicationGraph;
class UReplicationGraphNode;

struct FConnectionGatherActorListParameters;
struct FGlobalActorReplicationInfo;

/** How many viewers a single connection (plus its split-screen children) is expected to have. */
#define REPGRAPH_VIEWERS_PER_CONNECTION 4

// --------------------------------------------------------------------------------------------------------------------------------------------
// Actor Replication List Types
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * Raw AActor* is used for the lists. The engine keeps this behind a typedef so
 * it can be swapped for an id or a weak pointer later; that indirection is kept
 * so the surrounding code reads the same.
 */
typedef AActor* FActorRepListType;

inline std::string GetActorRepListTypeDebugString(const FActorRepListType& In) { return In ? In->GetName() : std::string("None"); }
inline UClass* GetActorRepListTypeClass(const FActorRepListType& In) { return In->Class; }

/**
 * Generic flags describing an actor list. Currently only "Default" versus
 * "FastShared". These are unsorted lists returned from the graph, merged
 * together, sorted, and replicated until the bandwidth limit is hit.
 */
enum class EActorRepListTypeFlags : uint8
{
	Default = 0,
	FastShared = 1,
	Max, // Always keep last
};

/** Tests that an actor may be replicated at all: not pending kill, not being destroyed. Says nothing about wanting to. */
inline bool IsActorValidForReplication(const AActor* Actor)
{
	return Actor != nullptr
		&& !Actor->IsActorBeingDestroyed()
		&& (static_cast<uint32>(Actor->Flags) & (static_cast<uint32>(EObjectFlags::BeginDestroyed) | static_cast<uint32>(EObjectFlags::FinishDestroyed))) == 0;
}

/** Tests that an actor may be gathered from the graph and considered for replication this frame. */
inline bool IsActorValidForReplicationGather(const FActorRepListType& In)
{
	const AActor* Actor = In;

	if (!Actor)
	{
		return false;
	}

	if (!IsActorValidForReplication(Actor))
	{
		return false;
	}

	if (Actor->GetIsReplicated() == false)
	{
		return false;
	}

	if (Actor->GetTearOff())
	{
		return false;
	}

	if (Actor->NetDormancy == ENetDormancy::DORM_Initial && Actor->IsNetStartupActor())
	{
		return false;
	}

	return true;
}

/**
 * Holds a list of replicated actors that can be added to and removed from.
 *
 * The engine hands these out by reference during the gather phase; nothing
 * copies the contents except where a copy is explicitly asked for.
 */
struct FActorRepListRefView
{
	FActorRepListRefView() = default;

	/** Empties the list without giving back the memory. Grows the reservation if a bigger one is asked for. */
	void Reset(int32 ExpectedMaxSize = 0) { RepList.Reset(ExpectedMaxSize); }

	void Reserve(int32 Size) { RepList.Reserve(Size); }

	bool ConditionalAdd(const FActorRepListType& NewElement)
	{
		if (IsActorValidForReplicationGather(NewElement))
		{
			Add(NewElement);
			return true;
		}
		return false;
	}

	void Add(const FActorRepListType& NewElement) { RepList.Add(NewElement); }

	/** Removes the element quickly but breaks the list order. */
	bool RemoveFast(const FActorRepListType& ElementToRemove, EAllowShrinking AllowShrink = EAllowShrinking::Default)
	{
		return RepList.RemoveSingleSwap(ElementToRemove, AllowShrink) > 0;
	}

	/** Removes the element and keeps the order intact. Not recommended for large lists. */
	bool RemoveSlow(const FActorRepListType& ElementToRemove) { return RepList.RemoveSingle(ElementToRemove) > 0; }

	void RemoveAtSwap(int32 Index) { RepList.RemoveAtSwap(Index); }

	void CopyContentsFrom(const FActorRepListRefView& Source) { RepList = Source.RepList; }
	void AppendContentsFrom(const FActorRepListRefView& Source) { RepList.Append(Source.RepList); }

	void AppendToTArray(TArray<FActorRepListType>& OutArray) const { OutArray.Append(RepList); }
	void AppendToTSet(TSet<FActorRepListType>& OutSet) const { OutSet.Append(RepList); }

	FActorRepListType& operator[](int32 Index) { return RepList[Index]; }
	const FActorRepListType& operator[](int32 Index) const { return RepList[Index]; }

	TArray<FActorRepListType>::RangedForIteratorType begin() { return RepList.begin(); }
	TArray<FActorRepListType>::RangedForConstIteratorType begin() const { return RepList.begin(); }
	TArray<FActorRepListType>::RangedForIteratorType end() { return RepList.end(); }
	TArray<FActorRepListType>::RangedForConstIteratorType end() const { return RepList.end(); }

	bool IsEmpty() const { return RepList.Num() <= 0; }
	int32 Num() const { return RepList.Num(); }

	/** Empties the list and gives back the memory it held. */
	void TearDown() { RepList.Empty(); }

	int32 IndexOf(const FActorRepListType& Value) const { return RepList.IndexOfByKey(Value); }
	bool Contains(const FActorRepListType& Value) const { return RepList.Contains(Value); }

	std::string BuildDebugString() const;

private:

	TArray<FActorRepListType> RepList;
};

/**
 * An FActorRepListRefView is one TArray, so its bytes fully describe it and a container may
 * relocate it with a memmove. It has to say so: the default answer is std::is_trivially_copyable,
 * which a TArray fails on account of its destructor. FActorRepListRefView is a map value in
 * several places (the per-node dormant lists, the always-relevant streaming level lists), and
 * without this those maps move-construct and destruct every element on every growth.
 */
PRISM_NAMESPACE_END
namespace UC
{
	template<> struct TIsTriviallyRelocatable<PRISM_NAMESPACE_NAME::FActorRepListRefView>
	{
		static constexpr bool Value = true;
	};
}
PRISM_NAMESPACE_START

/** Read-only window onto an FActorRepListRefView, held for the length of a gather. */
struct FActorRepListConstView
{
	explicit FActorRepListConstView(const FActorRepListRefView& InListReferenced) : ListReferenced(&InListReferenced) { }

	FActorRepListType operator[](int32 Index) const { return (*ListReferenced)[Index]; }

	int32 Num() const { return ListReferenced->Num(); }

	TArray<FActorRepListType>::RangedForConstIteratorType begin() const { return ListReferenced->begin(); }
	TArray<FActorRepListType>::RangedForConstIteratorType end() const { return ListReferenced->end(); }

private:

	const FActorRepListRefView* ListReferenced;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Gathering Parameters
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * "The list of gathered actors". This is what is pushed down the graph; nodes
 * add their lists to it and the driver merges and prioritizes what comes back.
 */
struct FGatheredReplicationActorLists
{
	void AddReplicationActorList(const FActorRepListRefView& List, EActorRepListTypeFlags Flags = EActorRepListTypeFlags::Default)
	{
		if (List.Num() > 0)
		{
			OutLists[static_cast<uint32>(Flags)].Emplace(FActorRepListConstView(List));
			CachedNum++;
		}
	}

	void Reset()
	{
		for (uint32 Index = static_cast<uint32>(EActorRepListTypeFlags::Default); Index < static_cast<uint32>(EActorRepListTypeFlags::Max); ++Index)
		{
			OutLists[Index].Reset();
		}
		CachedNum = 0;
	}

	int32 NumLists() const { return CachedNum; }

	const TArray<FActorRepListConstView>& GetLists(EActorRepListTypeFlags ListFlags) const { return OutLists[static_cast<uint32>(ListFlags)]; }

	bool ContainsLists(EActorRepListTypeFlags Flags) const { return OutLists[static_cast<uint32>(Flags)].Num() > 0; }

private:

	TStaticArray<TArray<FActorRepListConstView>, static_cast<uint32>(EActorRepListTypeFlags::Max)> OutLists;
	int32 CachedNum = 0;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// NewReplicatedActorInfo
// --------------------------------------------------------------------------------------------------------------------------------------------

/** How a new actor is pushed into the graph. "New" means new to the graph, not newly spawned. */
struct FNewReplicatedActorInfo
{
	explicit FNewReplicatedActorInfo(const FActorRepListType& InActor)
		: Actor(InActor)
		, Class(InActor->Class)
	{
		StreamingLevelName = GetStreamingLevelNameOfActor(Actor);
	}

	explicit FNewReplicatedActorInfo(const FActorRepListType& InActor, FName OverrideLevelName)
		: Actor(InActor)
		, StreamingLevelName(OverrideLevelName)
		, Class(InActor->Class)
	{
	}

	AActor* GetActor() const { return Actor; }

	/** NAME_None for actors in the persistent level; otherwise the package the level was streamed from. */
	static FName GetStreamingLevelNameOfActor(const AActor* Actor);

	FActorRepListType Actor;
	FName StreamingLevelName;
	UClass* Class;
};

/** Used to move an actor between levels in the graph when it is renamed/reoutered. */
struct FRenamedReplicatedActorInfo
{
	explicit FRenamedReplicatedActorInfo(const FActorRepListType& InActor, FName InPreviousStreamingLevelName)
		: NewActorInfo(InActor)
		, OldActorInfo(InActor, InPreviousStreamingLevelName)
	{
	}

	FNewReplicatedActorInfo NewActorInfo;
	FNewReplicatedActorInfo OldActorInfo;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Level based actor lists
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Holds lists of actors grouped by the streaming level they came from. */
struct FStreamingLevelActorListCollection
{
	void AddActor(const FNewReplicatedActorInfo& ActorInfo);

	bool RemoveActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound, UReplicationGraphNode* Outer = nullptr);
	bool RemoveActorFast(const FNewReplicatedActorInfo& ActorInfo, UReplicationGraphNode* Outer = nullptr);

	/** Moves the actor out of the level explicitly named, used when an actor's level changes. */
	bool RemoveActorFromLevelFast(AActor* Actor, FName LevelName);

	void Reset();

	/** Adds the lists for every level this connection can see to the gather. */
	void Gather(const FConnectionGatherActorListParameters& Params) const;
	void Gather(const UNetReplicationGraphConnection& ConnectionManager, FGatheredReplicationActorLists& OutGatheredList) const;

	/** Adds every list held, regardless of visibility. */
	void AppendAllLists(FGatheredReplicationActorLists& OutGatheredList) const;

	bool Contains(const FNewReplicatedActorInfo& ActorInfo) const;

	void DeepCopyFrom(const FStreamingLevelActorListCollection& Source);
	void GetAll_Debug(TArray<FActorRepListType>& OutArray) const;

	int32 NumLevels() const { return StreamingLevelLists.Num(); }

	void TearDown();

	struct FStreamingLevelActors
	{
		FStreamingLevelActors(FName InName) : StreamingLevelName(InName) { }

		FName StreamingLevelName;
		FActorRepListRefView ReplicationActorList;

		bool operator==(const FName& InName) const { return InName == StreamingLevelName; }
	};

	static const int32 NumInlineAllocations = 4;
	TArray<FStreamingLevelActors, TInlineAllocator<NumInlineAllocations>> StreamingLevelLists;
};

/** Holds a list of actors grouped by the permanent level or by their streaming level. */
struct FLevelBasedActorList
{
	void AddNetworkActor(AActor* NetActor);
	bool RemoveNetworkActor(const FNewReplicatedActorInfo& NetActor);

	bool RemoveNetworkActor(AActor* NetActor)
	{
		FNewReplicatedActorInfo ActorInfo(NetActor);
		return RemoveNetworkActor(ActorInfo);
	}

	void Reset();

	bool Contains(AActor* NetActor) const;

	/** True when nothing has ever been added, or everything added has since been removed. */
	bool IsEmpty() const;

	void Gather(const FConnectionGatherActorListParameters& Params) const;
	void Gather(const UNetReplicationGraphConnection& ConnectionManager, FGatheredReplicationActorLists& OutGatheredList) const;

	void AppendAllLists(FGatheredReplicationActorLists& OutGatheredList) const;

	void GetAllActors(TArray<AActor*>& OutAllActors) const;

	/** Moves the actor to the list for its new level. */
	void UpdateActorLevel(AActor* NetActor, FName PreviousLevelName);

private:

	FActorRepListRefView PermanentLevelActors;
	FStreamingLevelActorListCollection StreamingLevelActors;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Per Class / Actor Global / PerConnection Data Structs
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Per-class data about how actors of that class replicate. */
struct FClassReplicationInfo
{
	float DistancePriorityScale = 1.f;
	float StarvationPriorityScale = 1.f;
	float AccumulatedNetPriorityBias = 0.f;

	uint16 ReplicationPeriodFrame = 1;
	uint16 FastPath_ReplicationPeriodFrame = 1;
	uint16 ActorChannelFrameTimeout = 4;

	TFunction<bool(AActor*)> FastSharedReplicationFunc = nullptr;
	FName FastSharedReplicationFuncName = FName();

	void SetCullDistanceSquared(float InCullDistanceSquared)
	{
		CullDistanceSquared = InCullDistanceSquared;
		CullDistance = FMath::Sqrt(CullDistanceSquared);
	}

	float GetCullDistance() const { return CullDistance; }
	float GetCullDistanceSquared() const { return CullDistanceSquared; }

	std::string BuildDebugStringDelta() const;

private:

	float CullDistance = 0.0f;
	float CullDistanceSquared = 0.f;
};

/**
 * Scalars, an FName, and a TFunction that is one heap pointer - so the bytes of an
 * FClassReplicationInfo fully describe it and a container may relocate it with a memmove.
 *
 * This one matters more than most. TClassMap<FClassReplicationInfo> is the per-class settings
 * map, it is grown a few thousand entries at a time during InitGlobalActorClassSettings, and its
 * values are copied out again by the class hierarchy walk. See the note on
 * TSparseArray::AddUninitialized for what goes wrong when a value here is not relocatable.
 */
PRISM_NAMESPACE_END
namespace UC
{
	template<> struct TIsTriviallyRelocatable<PRISM_NAMESPACE_NAME::FClassReplicationInfo>
	{
		static constexpr bool Value = true;
	};
}
PRISM_NAMESPACE_START

/** Cached bunch and bookkeeping for the FastShared path. */
struct FFastSharedReplicationInfo
{
	uint32 LastAttemptBuildFrameNum = 0;	// The last frame FastSharedReplicationFunc was called on
	uint32 LastBunchBuildFrameNum = 0;		// The last frame a new bunch was actually created
};

DECLARE_MULTICAST_DELEGATE_FourParams(FNotifyActorChangeDormancy, FActorRepListType, FGlobalActorReplicationInfo&, ENetDormancy /*NewValue*/, ENetDormancy /*OldValue*/);
DECLARE_MULTICAST_DELEGATE_TwoParams(FNotifyActorFlushDormancy, FActorRepListType, FGlobalActorReplicationInfo&);
DECLARE_MULTICAST_DELEGATE_TwoParams(FNotifyActorForceNetUpdate, FActorRepListType, FGlobalActorReplicationInfo&);

struct FGlobalActorReplicationEvents
{
	FNotifyActorChangeDormancy DormancyChange;
	FNotifyActorFlushDormancy DormancyFlush;	// Cleared after broadcasting
	FNotifyActorForceNetUpdate ForceNetUpdate;
};

/** Per-actor data that is global to the whole graph. */
struct FGlobalActorReplicationInfo
{
	FGlobalActorReplicationInfo(FClassReplicationInfo& ClassInfo)
		: LastPreReplicationFrame(0)
		, WorldLocation(0.0, 0.0, 0.0)
		, bWantsToBeDormant(false)
		, bSwapRolesOnReplicate(false)
		, bWasWorldLocClamped(false)
		, Settings(ClassInfo)
	{
	}

	// -----------------------------------------------------------
	//	Dynamic state
	// -----------------------------------------------------------

	/** The last frame AActor::PreReplication was called, so it is called once per frame at most. */
	uint32 LastPreReplicationFrame = 0;

	/** The last frame game code called ForceNetUpdate on this actor. */
	uint32 ForceNetUpdateFrame = 0;

	/** Cached world location of the actor, refreshed by the spatialization node. */
	FVector WorldLocation;

	/** Mirrors AActor::NetDormancy > DORM_Awake. */
	uint32 bWantsToBeDormant : 1;

	/** Swap the actor's role and remote role around the call to ReplicateActor. */
	uint32 bSwapRolesOnReplicate : 1;

	/** Set when the actor was found with a world location that is not finite. */
	uint32 bWasWorldLocClamped : 1;

	/** Class default mirrors: initialized from the class info, then adjustable per actor. */
	FClassReplicationInfo Settings;

	/** FastShared path data, allocated on first use. */
	TUniquePtr<FFastSharedReplicationInfo> FastSharedReplicationInfo;

	/** Last frame FlushNetDormancy was called, so repeated calls in one frame are cheap. */
	uint32 LastFlushNetDormancyFrame = 0;

	// -----------------------------------------------------------
	//	Events: kept last. The largest chunk, accessed the least.
	// -----------------------------------------------------------
	FGlobalActorReplicationEvents Events;

	/**
	 * Whether this actor has anything riding along with it.
	 *
	 * Worth asking before GatherDependentActorLists, because almost no actor has dependents and
	 * the answer costs two integer comparisons against a gather plus a list container.
	 */
	bool HasDependentActors() const { return DependentActorList.IsEmpty() == false; }

	/** Gathers the dependent actors this connection can see. */
	void GatherDependentActorLists(const UNetReplicationGraphConnection& ConnectionManager, FGatheredReplicationActorLists& OutGatheredList)
	{
		DependentActorList.Gather(ConnectionManager, OutGatheredList);
	}

	void NotifyActorRenamed(AActor* Actor, FName PreviousLevelName) { DependentActorList.UpdateActorLevel(Actor, PreviousLevelName); }

	typedef TArray<FActorRepListType> FDependantListType;

	friend struct FGlobalActorReplicationInfoMap;

private:

	/** Replicated immediately after this actor, if the client has their level and the update frequency allows. */
	FLevelBasedActorList DependentActorList;

	/** The actors this one is a dependent of. */
	TArray<AActor*> ParentActorList;
};

/**
 * Maps UClass to some data type.
 *
 * The point of this over a plain TMap is the class hierarchy walk: a class that
 * was never explicitly registered picks up the nearest registered ancestor's
 * value, and the answer is memoized so the walk happens once.
 */
template<typename ValueType>
struct TClassMap
{
	ValueType& GetChecked(UClass* Class)
	{
		ValueType* Ptr = Get(Class);
		return *Ptr;
	}

	ValueType* Get(UClass* Class)
	{
		FObjectKey ObjKey(Class);
		if (ValueType* Ptr = Map.Find(ObjKey))
		{
			return Ptr;
		}

		// Allow the owner to initialize data for a class it has not seen yet.
		if (InitNewElement)
		{
			ValueType NewValue;

			if (InitNewElement(Class, NewValue))
			{
				ValueType& NewData = Map.Emplace(ObjKey, std::move(NewValue));
				return &NewData;
			}
		}

		// Not seen before and not initialized by the owner: climb the hierarchy (slower).
		return GetClassInfoForNewClass_r(ObjKey, Class);
	}

	/** Finds the explicit entry only. Does not climb the class hierarchy. */
	const ValueType* FindWithoutClassRecursion(UClass* Class) const { return Map.Find(FObjectKey(Class)); }

	bool Contains(const UClass* Class, bool bIncludeSuperClasses) const
	{
		if (bIncludeSuperClasses)
		{
			while (Class)
			{
				if (Map.Find(FObjectKey(Class)) != nullptr)
				{
					return true;
				}

				Class = static_cast<UClass*>(Class->SuperStruct);
			}
			return false;
		}

		return Map.Contains(FObjectKey(Class));
	}

	/** Sets the value for a class. Derived classes pick it up lazily through the hierarchy walk. */
	void Set(UClass* InClass, const ValueType& Value) { Map.Emplace(FObjectKey(InClass), Value); }

	void Emplace(UClass* InClass, ValueType&& Value) { Map.Emplace(FObjectKey(InClass), std::move(Value)); }

	typename TMap<FObjectKey, ValueType>::TIterator CreateIterator() { return Map.CreateIterator(); }
	void Reset() { Map.Reset(); }

	int32 Num() const { return Map.Num(); }

	TFunction<bool(UClass*, ValueType&)> InitNewElement;

private:

	ValueType* GetClassInfoForNewClass_r(FObjectKey OriginalObjKey, const UClass* OriginalClass)
	{
		const UClass* Class = static_cast<const UClass*>(OriginalClass->SuperStruct);
		if (!Class)
		{
			return nullptr;
		}

		FObjectKey ObjKey(Class);

		if (ValueType* Ptr = Map.Find(ObjKey))
		{
			// Point the original class at what the super resolved to so the walk happens once.
			// The local copy matters: the Emplace below can reallocate the map's storage.
			ValueType LocalData = *Ptr;
			ValueType& NewData = Map.Emplace(OriginalObjKey, std::move(LocalData));
			return &NewData;
		}

		return GetClassInfoForNewClass_r(ObjKey, Class);
	}

	TMap<FObjectKey /** UClass */, ValueType> Map;
};

struct FGlobalActorReplicationInfoMap
{
	FGlobalActorReplicationInfoMap()
	{
		// Always have AActor defined with default values. Calling SetClassInfo again overrides it.
		ClassMap.Set(AActor::StaticClass(), FClassReplicationInfo());
	}

	/** Returns the data for an actor, creating it from its class defaults if necessary. */
	FGlobalActorReplicationInfo& Get(const FActorRepListType& Actor)
	{
		if (TUniquePtr<FGlobalActorReplicationInfo>* Ptr = ActorMap.Find(Actor))
		{
			return *Ptr->Get();
		}

		FClassReplicationInfo& ClassInfo = GetClassInfo(GetActorRepListTypeClass(Actor));

		FGlobalActorReplicationInfo* NewGlobalActorRepInfo = new FGlobalActorReplicationInfo(ClassInfo);
		ActorMap.Emplace(Actor, TUniquePtr<FGlobalActorReplicationInfo>(NewGlobalActorRepInfo));
		return *NewGlobalActorRepInfo;
	}

	/** As above, reporting whether the entry had to be created. Kept separate so the common path stays cheap. */
	FGlobalActorReplicationInfo& Get(const FActorRepListType& Actor, bool& bWasCreated)
	{
		if (TUniquePtr<FGlobalActorReplicationInfo>* Ptr = ActorMap.Find(Actor))
		{
			return *Ptr->Get();
		}

		bWasCreated = true;

		FClassReplicationInfo& ClassInfo = GetClassInfo(GetActorRepListTypeClass(Actor));

		FGlobalActorReplicationInfo* NewGlobalActorRepInfo = new FGlobalActorReplicationInfo(ClassInfo);
		ActorMap.Emplace(Actor, TUniquePtr<FGlobalActorReplicationInfo>(NewGlobalActorRepInfo));
		return *NewGlobalActorRepInfo;
	}

	void SetInitClassInfoFunc(TFunction<bool(UClass*, FClassReplicationInfo&)> Func) { ClassMap.InitNewElement = Func; }

	/** Finds the data for an actor without creating it. */
	FGlobalActorReplicationInfo* Find(const FActorRepListType& Actor) const
	{
		if (const TUniquePtr<FGlobalActorReplicationInfo>* Ptr = ActorMap.Find(Actor))
		{
			return Ptr->Get();
		}

		return nullptr;
	}

	int32 Remove(const FActorRepListType& RemovedActor);

	void NotifyActorRenamed(AActor* Actor, FName PreviousStreamingLevelName);

	FClassReplicationInfo& GetClassInfo(UClass* Class) { return ClassMap.GetChecked(Class); }

	void SetClassInfo(UClass* InClass, const FClassReplicationInfo& Info) { ClassMap.Set(InClass, Info); }

	TMap<FActorRepListType, TUniquePtr<FGlobalActorReplicationInfo>>::TIterator CreateActorMapIterator() { return ActorMap.CreateIterator(); }
	TMap<FObjectKey, FClassReplicationInfo>::TIterator CreateClassMapIterator() { return ClassMap.CreateIterator(); }

	int32 Num() const { return ActorMap.Num(); }

	void ResetActorMap() { ActorMap.Reset(); }

	enum class EWarnFlag : uint8
	{
		None = 0,
		WarnAlreadyDependant = 1 << 0,
		WarnParentNotRegistered = 1 << 1,
		AllWarnings = WarnAlreadyDependant | WarnParentNotRegistered
	};

	/**
	 * Makes Child considered for replication whenever Parent replicates to a
	 * client, even if Child is outside its own cull distance. The link is one
	 * way, and it is the caller's job to break it when either actor dies.
	 */
	void AddDependentActor(AActor* Parent, AActor* Child, EWarnFlag WarnFlag = EWarnFlag::None);

	void RemoveDependentActor(AActor* Parent, AActor* Child)
	{
		if (Parent && Child)
		{
			if (FGlobalActorReplicationInfo* ParentInfo = Find(Parent))
			{
				ParentInfo->DependentActorList.RemoveNetworkActor(Child);
			}

			if (FGlobalActorReplicationInfo* ChildInfo = Find(Child))
			{
				ChildInfo->ParentActorList.RemoveSingleSwap(Parent);
			}
		}
	}

	void RemoveAllActorDependencies(AActor* MainActor, FGlobalActorReplicationInfo* MainActorInfo);

	void RemoveAllActorDependencies(AActor* MainActor)
	{
		FGlobalActorReplicationInfo* MainActorInfo = Find(MainActor);
		if (MainActor && MainActorInfo)
		{
			RemoveAllActorDependencies(MainActor, MainActorInfo);
		}
	}

private:

	TMap<FActorRepListType, TUniquePtr<FGlobalActorReplicationInfo>> ActorMap;
	TClassMap<FClassReplicationInfo> ClassMap;
};

/** Per-actor data stored per connection. */
struct FConnectionReplicationActorInfo
{
	FConnectionReplicationActorInfo()
		: bDormantOnConnection(0)
		, bTearOff(0)
		, bGridSpatilization_AlreadyDormant(0)
		, bForceCullDistanceToZero(0)
	{
	}

	FConnectionReplicationActorInfo(const FGlobalActorReplicationInfo& GlobalInfo)
		: bDormantOnConnection(0)
		, bTearOff(0)
		, bGridSpatilization_AlreadyDormant(0)
		, bForceCullDistanceToZero(0)
	{
		// Duplicated from the global info so the hot path does not have to look it up,
		// and so it can be overridden per connection.
		ReplicationPeriodFrame = GlobalInfo.Settings.ReplicationPeriodFrame;
		FastPath_ReplicationPeriodFrame = GlobalInfo.Settings.FastPath_ReplicationPeriodFrame;
		SetCullDistanceSquared(GlobalInfo.Settings.GetCullDistanceSquared());
	}

	/** Resets everything except the settings pulled from the global info. */
	void ResetFrameCounters()
	{
		Channel = nullptr;
		NextReplicationFrameNum = 0;
		LastRepFrameNum = 0;
		ActorChannelCloseFrameNum = 0;

		FastPath_NextReplicationFrameNum = 0;
		FastPath_LastRepFrameNum = 0;

		// Deliberately not clearing bDormantOnConnection or bTearOff.
	}

	void SetCullDistanceSquared(float InCullDistanceSquared)
	{
		CullDistanceSquared = InCullDistanceSquared;
		CullDistance = FMath::Sqrt(CullDistanceSquared);
	}

	float GetCullDistance() const { return bForceCullDistanceToZero ? 0.0f : CullDistance; }
	float GetCullDistanceSquared() const { return bForceCullDistanceToZero ? 0.0f : CullDistanceSquared; }

	UActorChannel* Channel = nullptr;

private:

	float CullDistance = 0.f;
	float CullDistanceSquared = 0.f;

public:

	/** Default replication */
	uint32 NextReplicationFrameNum = 0;	/** The next frame we are allowed to replicate on */
	uint32 LastRepFrameNum = 0;			/** The last frame this actor replicated to this connection */

	/** FastPath versions of the above */
	uint32 FastPath_NextReplicationFrameNum = 0;
	uint32 FastPath_LastRepFrameNum = 0;

	/** Minimum frames between successive calls to ReplicateActor */
	uint16 ReplicationPeriodFrame = 1;
	uint16 FastPath_ReplicationPeriodFrame = 1;

	/** The frame the actor channel closes on. Pushed forward every time the actor replicates. */
	uint32 ActorChannelCloseFrameNum = 0;

	uint8 bDormantOnConnection : 1;
	uint8 bTearOff : 1;

	/** Stops the same actor's dormancy being replicated twice when a splitscreen connection pulls from two cells. */
	uint8 bGridSpatilization_AlreadyDormant : 1;

	/** Makes GetCullDistance() report 0, which is how an actor is made temporarily always relevant on one connection. */
	uint8 bForceCullDistanceToZero : 1;
};

/** Actor -> FConnectionReplicationActorInfo. Wraps the map so FindOrAdd can seed from the global info. */
struct FPerConnectionActorInfoMap
{
	FConnectionReplicationActorInfo& FindOrAdd(const FActorRepListType& Actor)
	{
		if (TSharedPtr<FConnectionReplicationActorInfo>* ValuePtr = ActorMap.Find(Actor))
		{
			return *ValuePtr->Get();
		}

		FConnectionReplicationActorInfo* NewInfo = new FConnectionReplicationActorInfo(GlobalMap->Get(Actor));
		ActorMap.Emplace(Actor, TSharedPtr<FConnectionReplicationActorInfo>(NewInfo));
		return *NewInfo;
	}

	FConnectionReplicationActorInfo* Find(const FActorRepListType& Actor)
	{
		if (TSharedPtr<FConnectionReplicationActorInfo>* ValuePtr = ActorMap.Find(Actor))
		{
			return ValuePtr->Get();
		}

		return nullptr;
	}

	FConnectionReplicationActorInfo* FindByChannel(UActorChannel* Channel)
	{
		if (TSharedPtr<FConnectionReplicationActorInfo>* ValuePtr = ChannelMap.Find(Channel))
		{
			return ValuePtr->Get();
		}

		return nullptr;
	}

	void AddChannel(const FActorRepListType& Actor, UActorChannel* Channel)
	{
		if (TSharedPtr<FConnectionReplicationActorInfo>* ValuePtr = ActorMap.Find(Actor))
		{
			ChannelMap.Add(Channel, *ValuePtr);
		}
	}

	void RemoveChannel(UActorChannel* Channel) { ChannelMap.Remove(Channel); }
	void RemoveActor(const FActorRepListType& Actor) { ActorMap.Remove(Actor); }

	TMap<FActorRepListType, TSharedPtr<FConnectionReplicationActorInfo>>::TIterator CreateIterator() { return ActorMap.CreateIterator(); }
	TMap<UActorChannel*, TSharedPtr<FConnectionReplicationActorInfo>>::TIterator CreateChannelIterator() { return ChannelMap.CreateIterator(); }

	void SetGlobalMap(FGlobalActorReplicationInfoMap* InGlobalMap) { GlobalMap = InGlobalMap; }

	void ResetActorMap()
	{
		ActorMap.Reset();
		ChannelMap.Reset();
	}

	int32 Num() const { return ActorMap.Num(); }

private:

	TMap<FActorRepListType, TSharedPtr<FConnectionReplicationActorInfo>> ActorMap;
	TMap<UActorChannel*, TSharedPtr<FConnectionReplicationActorInfo>> ChannelMap;
	FGlobalActorReplicationInfoMap* GlobalMap = nullptr;
};

/** The data every node in the graph is initialized with. */
struct FReplicationGraphGlobalData
{
	FReplicationGraphGlobalData() = default;
	FReplicationGraphGlobalData(FGlobalActorReplicationInfoMap* InRepMap, UWorld* InWorld, UReplicationGraph* InReplicationGraph)
		: GlobalActorReplicationInfoMap(InRepMap)
		, World(InWorld)
		, ReplicationGraph(InReplicationGraph)
	{
	}

	FGlobalActorReplicationInfoMap* GlobalActorReplicationInfoMap = nullptr;

	UWorld* World = nullptr;

	UReplicationGraph* ReplicationGraph = nullptr;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Prioritized Actor Lists
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Prioritized list of actors to replicate. This is what actually drives replication. */
struct FPrioritizedRepList
{
	FPrioritizedRepList() = default;

	struct FItem
	{
		FItem(float InPriority, FActorRepListType InActor, FGlobalActorReplicationInfo* InGlobal, FConnectionReplicationActorInfo* InConn)
			: Priority(InPriority)
			, Actor(InActor)
			, GlobalData(InGlobal)
			, ConnectionData(InConn)
		{
		}

		bool operator<(const FItem& Other) const { return Priority < Other.Priority; }

		float Priority;
		FActorRepListType Actor;

		FGlobalActorReplicationInfo* GlobalData;
		FConnectionReplicationActorInfo* ConnectionData;
	};

	TArray<FItem> Items;

	void Reset() { Items.Reset(); }
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Connection Gather Actor List Parameters
// --------------------------------------------------------------------------------------------------------------------------------------------

typedef TInlineAllocator<REPGRAPH_VIEWERS_PER_CONNECTION> FReplicationGraphConnectionsAllocator;
typedef TArray<FNetViewer, FReplicationGraphConnectionsAllocator> FNetViewerArray;

/** What is handed down to every node during the gather phase. */
struct FConnectionGatherActorListParameters
{
	FConnectionGatherActorListParameters(
		FNetViewerArray& InViewers,
		UNetReplicationGraphConnection& InConnectionManager,
		const TSet<FName>& InClientVisibleLevelNamesRef,
		uint32 InReplicationFrameNum,
		FGatheredReplicationActorLists& InOutGatheredReplicationLists,
		bool bInSelectedForHeavyComputation)
		: Viewers(InViewers)
		, ConnectionManager(InConnectionManager)
		, ReplicationFrameNum(InReplicationFrameNum)
		, OutGatheredReplicationLists(InOutGatheredReplicationLists)
		, ClientVisibleLevelNamesRef(InClientVisibleLevelNamesRef)
		, bIsSelectedForHeavyComputation(bInSelectedForHeavyComputation)
	{
	}

	FNetViewerArray Viewers;
	UNetReplicationGraphConnection& ConnectionManager;
	uint32 ReplicationFrameNum;

	/** Out: what the nodes add to. */
	FGatheredReplicationActorLists& OutGatheredReplicationLists;

	bool CheckClientVisibilityForLevel(const FName& StreamingLevelName) const
	{
		if (StreamingLevelName == LastCheckedVisibleLevelName)
		{
			return true;
		}

		const bool bVisible = ClientVisibleLevelNamesRef.Contains(StreamingLevelName);
		if (bVisible)
		{
			LastCheckedVisibleLevelName = StreamingLevelName;
		}
		return bVisible;
	}

	/** Cached reference for fast level visibility lookup. */
	const TSet<FName>& ClientVisibleLevelNamesRef;
	const bool bIsSelectedForHeavyComputation;

private:

	mutable FName LastCheckedVisibleLevelName;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// RPCs
// --------------------------------------------------------------------------------------------------------------------------------------------

struct FRPCSendPolicyInfo
{
	FRPCSendPolicyInfo(const bool bInSendImmediately) : bSendImmediately(bInSendImmediately) { }

	/**
	 * When set, an RPC called from inside NetDriver::TickDispatch is flushed to
	 * the client at the end of TickDispatch rather than waiting for TickFlush.
	 * Costs bandwidth and CPU; buys latency.
	 */
	uint8 bSendImmediately : 1;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Debug Info
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Passed down the graph when it is asked to describe itself. */
struct FReplicationGraphDebugInfo
{
	enum EFlags
	{
		ShowActors,
		ShowClasses,
		ShowNativeClasses,
		ShowTotalCount
	};

	void Log(const std::string& Str) const;

	void PushIndent() { CurrentIndent += 2; }
	void PopIndent() { CurrentIndent -= 2; }

	std::string GetIndentString() const { return std::string(static_cast<size_t>(CurrentIndent), ' '); }

	int32 CurrentIndent = 0;

	bool bShowEmptyNodes = false;
	EFlags Flags = ShowActors;
};

void LogActorRepList(FReplicationGraphDebugInfo& DebugInfo, const std::string& Prefix, const FActorRepListRefView& List);

PRISM_NAMESPACE_END
