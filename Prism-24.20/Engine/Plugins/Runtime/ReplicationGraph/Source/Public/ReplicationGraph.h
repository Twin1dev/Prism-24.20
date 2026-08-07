#pragma once

/*=============================================================================
	Replication Graph

	Implementation of the replication driver, customized by subclassing
	UReplicationGraph. UReplicationGraph on its own does not fully function and
	is meant to be overridden - see FortReplicationGraph.h for Fortnite's.

	High level overview:

	  * The graph is a collection of nodes which produce replication lists for
	    each network connection. It maintains persistent lists of actors and
	    feeds them to connections, which is what lets the work be shared and
	    makes the cost scale with actors + connections rather than actors *
	    connections.

	  * The spatialization node is the obvious example: everything that uses
	    distance based relevancy lives there. There are also always-relevant
	    nodes. Nodes can be global, per connection, or shared between a group of
	    connections ("always relevant for team").

	  * AActor::IsNetRelevantFor and AActor::GetNetPriority are NOT used. Game
	    code affects replication through three things instead: where an actor is
	    routed in the graph, FGlobalActorReplicationInfo, and
	    FConnectionReplicationActorInfo.

	  * After the graph returns them, lists are culled for distance and
	    frequency, merged, and prioritized. The result is a sorted list of actors
	    which is then turned into actor channel opens/updates.

	Subclasses implement:

	UReplicationGraph::InitGlobalActorClassSettings
	    Initialize UReplicationGraph::GlobalActorReplicationInfoMap.

	UReplicationGraph::InitGlobalGraphNodes
	    Instantiate nodes with ::CreateNewNode and register the global ones with
	    ::AddGlobalGraphNode.

	UReplicationGraph::RouteAddNetworkActorToNodes / ::RouteRemoveNetworkActorToNodes
	    Route actor spawn/despawn to the right node.

	UReplicationGraph::InitConnectionGraphNodes
	    Initialize per-connection nodes, or associate shared ones through
	    ::AddConnectionGraphNode.
=============================================================================*/

#include "ReplicationGraphTypes.h"

#include "Engine/ActorChannel.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Templates/Casts.h"

PRISM_NAMESPACE_START

class UNetReplicationGraphConnection;
class UReplicationGraph;
class UReplicationGraphNode_DormancyNode;
class UReplicationGraphNode_GridCell;

typedef TObjectKey<UNetReplicationGraphConnection> FRepGraphConnectionKey;

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraphNode
// --------------------------------------------------------------------------------------------------------------------------------------------

class UReplicationGraphNode
{
public:
	UReplicationGraphNode() = default;
	virtual ~UReplicationGraphNode();

	/** Called when a network actor is spawned or an actor changes replication status. */
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) = 0;

	/** Called when a networked actor is being destroyed or no longer wants to replicate. */
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) = 0;

	/** Called when a dynamic actor changes its outer. Nodes that key on level have to update their level info. */
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) { return false; }

	/** Called on world change: dump any persistent data about replicated actors. */
	virtual void NotifyResetAllNetworkActors();

	/** Marks the node and all its children for destruction. */
	virtual void TearDown();

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) = 0;

	/** Called once per frame before replication, on root nodes with bRequiresPrepareForReplicationCall set. */
	virtual void PrepareForReplication() { }

	/** Debug only: flattens everything this node holds into a plain array. */
	virtual void GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const;

	// -----------------------------------------------------

	bool GetRequiresPrepareForReplication() const { return bRequiresPrepareForReplicationCall; }

	void Initialize(const TSharedPtr<FReplicationGraphGlobalData>& InGraphGlobals) { GraphGlobals = InGraphGlobals; }

	UWorld* GetWorld() const { return GraphGlobals.IsValid() ? GraphGlobals->World : nullptr; }

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const;

	virtual std::string GetDebugString() const { return NodeName; }

	void SetDebugName(const std::string& InName) { NodeName = InName; }

	/** Allocates and initializes a child node. This is what a node's own factory functions call. */
	template<class T>
	T* CreateChildNode()
	{
		T* NewNode = new T();
		NewNode->Initialize(GraphGlobals);
		NewNode->SetDebugName(typeid(T).name());
		AllChildNodes.Add(NewNode);
		return NewNode;
	}

	/** Node removal behavior. */
	enum class NodeOrdering
	{
		IgnoreOrdering,	// Faster removal, but may break node processing order
		KeepOrder,		// Slower removal, keeps the node order intact
	};

	/** Removes a child node from our list and destroys it. Returns whether it was found. */
	bool RemoveChildNode(UReplicationGraphNode* OutChildNode, NodeOrdering NodeOrder = NodeOrdering::IgnoreOrdering);

	/** Drops any null or torn down nodes from our list. */
	void CleanChildNodes(NodeOrdering NodeOrder);

	const TArray<UReplicationGraphNode*>& GetAllChildNodes() const { return AllChildNodes; }

protected:

	TArray<UReplicationGraphNode*> AllChildNodes;

	TSharedPtr<FReplicationGraphGlobalData> GraphGlobals;

	/** Whether PrepareForReplication() is called. Must be set in the constructor, not later. */
	bool bRequiresPrepareForReplicationCall = false;

	std::string NodeName;
};

// -----------------------------------

/** A node holding actor lists: one "base" list plus a list per streaming level. */
class UReplicationGraphNode_ActorList : public UReplicationGraphNode
{
public:
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;
	virtual void GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const override;
	virtual void TearDown() override;

	/** Removes the actor quickly but breaks the list order. */
	bool RemoveNetworkActorFast(const FNewReplicatedActorInfo& ActorInfo);

	/** Copies Source's lists into this node. Child nodes are not copied. */
	void DeepCopyActorListsFrom(const UReplicationGraphNode_ActorList* Source);

protected:

	/** Logs our own lists but not our child nodes'. Useful when a subclass overrides LogNode. */
	void LogActorList(FReplicationGraphDebugInfo& DebugInfo) const;

	/** The base list that most actors go in. */
	FActorRepListRefView ReplicationActorList;

	/** Lists for actors that came from streaming levels. */
	FStreamingLevelActorListCollection StreamingLevelCollection;
};

// -----------------------------------

/**
 * A node holding several buckets of non-streaming actors, pulling from one
 * bucket per frame. A cheap way to spread replication load broadly, before each
 * actor's own frequency check.
 */
class UReplicationGraphNode_ActorListFrequencyBuckets : public UReplicationGraphNode
{
public:

	struct FSettings
	{
		int32 NumBuckets = 3;
		int32 ListSize = 12;
		bool EnableFastPath = false;	// Return lists as FastPath on "off frames". Off by default.
		int32 FastPathFrameModulo = 1;	// Only take the fast path when frame % this == 0

		/** Thresholds for rebalancing bucket count as the node's population changes. */
		struct FBucketThresholds
		{
			FBucketThresholds(int32 InMaxActors, int32 InNumBuckets) : MaxActors(InMaxActors), NumBuckets(InNumBuckets) { }
			int32 MaxActors;	// When num actors <= MaxActors
			int32 NumBuckets;	// use this many buckets
		};

		TArray<FBucketThresholds, TInlineAllocator<4>> BucketThresholds;
	};

	/** Defaults shared by every node of this type, so a game can retune all of them without subclassing. */
	static FSettings DefaultSettings;

	/** Per-node override. Falls back to DefaultSettings when unset. */
	TSharedPtr<FSettings> Settings;

	const FSettings& GetSettings() const { return Settings.IsValid() ? *Settings.Get() : DefaultSettings; }

	UReplicationGraphNode_ActorListFrequencyBuckets() { SetNonStreamingCollectionSize(GetSettings().NumBuckets); }

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;
	virtual void TearDown() override;
	virtual void GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const override;

	void SetNonStreamingCollectionSize(const int32 NewSize);

protected:

	void CheckRebalance();

	int32 TotalNumNonStreamingActors = 0;

	/** Non-streaming actors go in one of these. */
	TArray<FActorRepListRefView, TInlineAllocator<2>> NonStreamingCollection;

	FStreamingLevelActorListCollection StreamingLevelCollection;
};

// -----------------------------------

/**
 * For dynamic (moving) actors, where replication frequency falls off with
 * distance from the connection's view location rather than being a fixed period.
 */
class UReplicationGraphNode_DynamicSpatialFrequency : public UReplicationGraphNode_ActorList
{
public:

	UReplicationGraphNode_DynamicSpatialFrequency();

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	// --------------------------------------------------------

	struct FSpatializationZone
	{
		/** Direct */
		FSpatializationZone(float InMinDotProduct, float InMinDistPct, float InMaxDistPct, uint32 InMinRepPeriod, uint32 InMaxRepPeriod, uint32 InMinRepPeriod_FastPath, uint32 InMaxRepPeriod_FastPath)
			: MinDotProduct(InMinDotProduct), MinDistPct(InMinDistPct), MaxDistPct(InMaxDistPct)
			, MinRepPeriod(InMinRepPeriod), MaxRepPeriod(InMaxRepPeriod)
			, FastPath_MinRepPeriod(InMinRepPeriod_FastPath), FastPath_MaxRepPeriod(InMaxRepPeriod_FastPath)
		{
		}

		/** From target frame rate */
		FSpatializationZone(float InMinDotProduct, float InMinDistPct, float InMaxDistPct, float InMinRepHz, float InMaxRepHz, float InMinRepHz_FastPath, float InMaxRepHz_FastPath, float TickRate)
			: MinDotProduct(InMinDotProduct), MinDistPct(InMinDistPct), MaxDistPct(InMaxDistPct)
			, MinRepPeriod(HzToFrm(InMinRepHz, TickRate)), MaxRepPeriod(HzToFrm(InMaxRepHz, TickRate))
			, FastPath_MinRepPeriod(HzToFrm(InMinRepHz_FastPath, TickRate)), FastPath_MaxRepPeriod(HzToFrm(InMaxRepHz_FastPath, TickRate))
		{
		}

		float MinDotProduct = 1.f;	// Dot product must be >= this to fall in this zone
		float MinDistPct = 0.f;		// Distance, as a fraction of the connection cull distance, that maps to MinRepPeriod
		float MaxDistPct = 1.f;		// Distance, as a fraction of the connection cull distance, that maps to MaxRepPeriod

		uint32 MinRepPeriod = 5;
		uint32 MaxRepPeriod = 10;

		uint32 FastPath_MinRepPeriod = 1;
		uint32 FastPath_MaxRepPeriod = 5;

		static uint32 HzToFrm(float Hz, float TargetFrameRate)
		{
			return Hz > 0.f ? static_cast<uint32>(FMath::CeilToInt(TargetFrameRate / Hz)) : 0;
		}
	};

	struct FSettings
	{
		FSettings() : MaxBitsPerFrame(0), MaxNearestActors(-1) { }

		FSettings(const TArray<FSpatializationZone>& InZoneSettings, const TArray<FSpatializationZone>& InZoneSettings_NonFastSharedActors, int64 InMaxBitsPerFrame, int32 InMaxNearestActors = -1)
			: ZoneSettings(InZoneSettings)
			, ZoneSettings_NonFastSharedActors(InZoneSettings_NonFastSharedActors)
			, MaxBitsPerFrame(InMaxBitsPerFrame)
			, MaxNearestActors(InMaxNearestActors)
		{
		}

		TArray<FSpatializationZone> ZoneSettings;
		TArray<FSpatializationZone> ZoneSettings_NonFastSharedActors;	// For actors with no FastShared support
		int64 MaxBitsPerFrame;
		int32 MaxNearestActors;	// Replicate only the X nearest actors in this node. -1 = no limit.
	};

	static FSettings DefaultSettings;	// Used by every instance
	FSettings* Settings = nullptr;		// Optional per-instance override

	FSettings& GetSettings() { return Settings ? *Settings : DefaultSettings; }

protected:

	struct FDynamicSpatialFrequency_SortedItem
	{
		FDynamicSpatialFrequency_SortedItem() = default;
		FDynamicSpatialFrequency_SortedItem(AActor* InActor, int32 InFramesTillReplicate, bool InEnableFastPath, FGlobalActorReplicationInfo* InGlobal, FConnectionReplicationActorInfo* InConnection)
			: Actor(InActor), FramesTillReplicate(InFramesTillReplicate), EnableFastPath(InEnableFastPath), GlobalInfo(InGlobal), ConnectionInfo(InConnection)
		{
		}

		FDynamicSpatialFrequency_SortedItem(AActor* InActor, int32 InDistance, FGlobalActorReplicationInfo* InGlobal)
			: Actor(InActor), FramesTillReplicate(InDistance), GlobalInfo(InGlobal)
		{
		}

		bool operator<(const FDynamicSpatialFrequency_SortedItem& Other) const { return FramesTillReplicate < Other.FramesTillReplicate; }

		AActor* Actor = nullptr;

		int32 FramesTillReplicate = 0;	// Doubles as "distance squared" during the MaxNearestActors pass
		bool EnableFastPath = false;

		FGlobalActorReplicationInfo* GlobalInfo = nullptr;
		FConnectionReplicationActorInfo* ConnectionInfo = nullptr;
	};

	/** Working area for the sorted list. Rebuilt for each connection each frame. */
	TArray<FDynamicSpatialFrequency_SortedItem> SortedReplicationList;

	/** The sorted list flattened back into a rep list, so it can be handed to the gather. */
	FActorRepListRefView SortedGatherList;

	/** Working counters for adaptive load balancing. Actors that replicate every frame do not count. */
	int32 NumExpectedReplicationsThisFrame = 0;
	int32 NumExpectedReplicationsNextFrame = 0;

	bool IgnoreCullDistance = false;

	virtual void GatherActors(const FActorRepListRefView& RepList, FGlobalActorReplicationInfoMap& GlobalMap, FPerConnectionActorInfoMap& ConnectionMap, const FConnectionGatherActorListParameters& Params, UNetConnection* NetConnection);
	virtual void GatherActors_DistanceOnly(const FActorRepListRefView& RepList, FGlobalActorReplicationInfoMap& GlobalMap, FPerConnectionActorInfoMap& ConnectionMap, const FConnectionGatherActorListParameters& Params);

	void CalcFrequencyForActor(AActor* Actor, UReplicationGraph* RepGraph, UNetConnection* NetConnection, FGlobalActorReplicationInfo& GlobalInfo, FPerConnectionActorInfoMap& ConnectionMap, FSettings& MySettings, const FNetViewerArray& Viewers, const uint32 FrameNum, int32 ExistingItemIndex);
};

// -----------------------------------

/** Per-connection copy of a dormancy list, which drops actors once they are fully dormant on that connection. */
class UReplicationGraphNode_ConnectionDormancyNode : public UReplicationGraphNode_ActorList
{
public:

	/**
	 * How many frames since the last Gather before a node is considered obsolete.
	 * 0 disables the check. Trades memory and dormancy-event cost against the
	 * cost of rebuilding a cell a connection comes back to.
	 */
	static void SetNumFramesUntilObsolete(uint32 NumFrames);

	void InitConnectionNode(const FRepGraphConnectionKey& InConnectionOwner, uint32 CurrentFrame)
	{
		ConnectionOwner = InConnectionOwner;
		LastGatheredFrame = CurrentFrame;
	}

	virtual void TearDown() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;

	void NotifyActorDormancyFlush(FActorRepListType Actor);

	void OnClientLevelVisibilityAdd(FName LevelName, UWorld* World);

	bool IsNodeObsolete(uint32 CurrentFrame) const;

private:

	static uint32 NumFramesUntilObsolete;

	FRepGraphConnectionKey ConnectionOwner;

	uint32 LastGatheredFrame = 0;

	void ConditionalGatherDormantActorsForConnection(FActorRepListRefView& ConnectionRepList, const FConnectionGatherActorListParameters& Params, FActorRepListRefView* RemovedList);

	int32 TrickleStartCounter = 10;

	/** Actors removed from this per-connection node, kept so they can be restored if the level is unloaded and reloaded. */
	FStreamingLevelActorListCollection RemovedStreamingLevelActorListCollection;
};

/** Holds dormant actors and hands out per-connection copies of the list. */
class UReplicationGraphNode_DormancyNode : public UReplicationGraphNode_ActorList
{
public:

	/** A connection's Z has to be below this for connection nodes to be created. */
	static double MaxZForConnection;

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual void NotifyResetAllNetworkActors() override;

	void AddDormantActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo);
	void RemoveDormantActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo);
	void RenameDormantActor(const FRenamedReplicatedActorInfo& ActorInfo);

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	void OnActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo);

	void ConditionalGatherDormantDynamicActors(FActorRepListRefView& RepList, const FConnectionGatherActorListParameters& Params, FActorRepListRefView* RemovedList, bool bEnforceReplistUniqueness = false, FActorRepListRefView* RemoveFromList = nullptr);

	UReplicationGraphNode_ConnectionDormancyNode* GetExistingConnectionNode(const FConnectionGatherActorListParameters& Params);

	UReplicationGraphNode_ConnectionDormancyNode* GetConnectionNode(const FConnectionGatherActorListParameters& Params);

private:

	UReplicationGraphNode_ConnectionDormancyNode* CreateConnectionNode(const FConnectionGatherActorListParameters& Params);

	typedef TFunction<void(UReplicationGraphNode_ConnectionDormancyNode*)> FConnectionDormancyNodeFunction;

	/** Calls Function on every connection node still valid, destroying or skipping the obsolete ones. */
	void CallFunctionOnValidConnectionNodes(FConnectionDormancyNodeFunction Function);

	typedef TSortedMap<FRepGraphConnectionKey, UReplicationGraphNode_ConnectionDormancyNode*> FConnectionDormancyNodeMap;
	FConnectionDormancyNodeMap ConnectionNodes;
};

/** One cell of the 2D spatialization grid. */
class UReplicationGraphNode_GridCell : public UReplicationGraphNode_ActorList
{
public:

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;
	virtual void GetAllActorsInNode_Debugging(TArray<FActorRepListType>& OutArray) const override;

	void AddStaticActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalRepInfo, bool bParentNodeHandlesDormancyChange);
	void AddDynamicActor(const FNewReplicatedActorInfo& ActorInfo);

	void RemoveStaticActor(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool bWasAddedAsDormantActor);
	void RemoveDynamicActor(const FNewReplicatedActorInfo& ActorInfo);

	/** Lets the graph decide what kind of node handles the dynamic actors in this cell. */
	TFunction<UReplicationGraphNode*(UReplicationGraphNode_GridCell* Parent)> CreateDynamicNodeOverride;

	UReplicationGraphNode_DormancyNode* GetDormancyNode(bool bInCreateIfMissing = true);

private:

	UReplicationGraphNode* DynamicNode = nullptr;
	UReplicationGraphNode_DormancyNode* DormancyNode = nullptr;

	UReplicationGraphNode* GetDynamicNode();

	void OnActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, UReplicationGraphNode_DormancyNode* DormancyNode);

	void ConditionalCopyDormantActors(FActorRepListRefView& FromList, UReplicationGraphNode_DormancyNode* ToNode);
	void OnStaticActorNetDormancyChange(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, ENetDormancy NewValue, ENetDormancy OldValue);
};

// -----------------------------------

class UReplicationGraphNode_GridSpatialization2D : public UReplicationGraphNode
{
public:

	UReplicationGraphNode_GridSpatialization2D();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;
	virtual void PrepareForReplication() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;

	void AddActor_Static(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo) { AddActorInternal_Static(ActorInfo, ActorRepInfo, false); }
	void AddActor_Dynamic(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo) { AddActorInternal_Dynamic(ActorInfo); }
	void AddActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo);

	void RemoveActor_Static(const FNewReplicatedActorInfo& ActorInfo);
	void RemoveActor_Dynamic(const FNewReplicatedActorInfo& ActorInfo) { RemoveActorInternal_Dynamic(ActorInfo); }
	void RemoveActor_Dormancy(const FNewReplicatedActorInfo& ActorInfo);

	/** Called when a cull distance changes. Only updates this node's cached state; the caller updates the rep infos. */
	virtual void NotifyActorCullDistChange(AActor* Actor, FGlobalActorReplicationInfo& GlobalInfo, float OldDist);

	float CellSize = 0.f;
	FVector2D SpatialBias = FVector2D(0.0, 0.0);
	double ConnectionMaxZ = MAX_dbl;	// Connections above this pull nothing from the grid

	/** Lets the graph decide what kind of node a cell is. */
	TFunction<UReplicationGraphNode_GridCell*(UReplicationGraphNode_GridSpatialization2D* Parent)> CreateCellNodeOverride;

	void ForceRebuild() { bNeedsRebuild = true; }

	/** Marks a class as unable to force a spatial rebuild when one of its instances leaves the grid. */
	void AddToClassRebuildDenyList(UClass* Class) { ClassRebuildDenyList.Set(Class, true); }

	/** When set, the graph tells clients to destroy dormant dynamic actors as they leave relevancy. */
	bool bDestroyDormantDynamicActors = true;

	/** How many frames a dormant actor has to be out of range before it is destroyed. */
	int32 DestroyDormantDynamicActorsCellTTL = 1;

	/** Spreads the destruction cost over multiple frames. */
	int32 ReplicatedDormantDestructionInfosPerFrame = MAX_int32;

protected:

	virtual void AddActorInternal_Dynamic(const FNewReplicatedActorInfo& ActorInfo);
	virtual void AddActorInternal_Static(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool IsDormancyDriven);
	virtual void AddActorInternal_Static_Implementation(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool IsDormancyDriven);

	virtual void RemoveActorInternal_Dynamic(const FNewReplicatedActorInfo& Actor);
	virtual void RemoveActorInternal_Static(const FNewReplicatedActorInfo& Actor, FGlobalActorReplicationInfo& ActorRepInfo, bool WasAddedAsDormantActor);

	struct FActorCellInfo
	{
		bool IsValid() const { return StartX != -1; }
		void Reset() { StartX = -1; }
		int32 StartX = -1;
		int32 StartY = 0;
		int32 EndX = 0;
		int32 EndY = 0;
	};

	FActorCellInfo GetCellInfoForActor(FActorRepListType Actor, const FVector& Location3D, float CullDistance);

	void GetGridNodesForActor(FActorRepListType Actor, const FGlobalActorReplicationInfo& ActorRepInfo, TArray<UReplicationGraphNode_GridCell*>& OutNodes);
	void GetGridNodesForActor(FActorRepListType Actor, const FActorCellInfo& CellInfo, TArray<UReplicationGraphNode_GridCell*>& OutNodes);

private:

	bool WillActorLocationGrowSpatialBounds(const FVector& Location) const;

	/** Called when an actor is found outside the spatial bounds. */
	void HandleActorOutOfSpatialBounds(AActor* Actor, const FVector& Location3D, const bool bStaticActor);

	/** Classes which may not force a rebuild of the spatialization tree; their instances are clamped instead (e.g. projectiles). */
	TClassMap<bool> ClassRebuildDenyList;

	struct FCachedDynamicActorInfo
	{
		FCachedDynamicActorInfo(const FNewReplicatedActorInfo& InInfo) : ActorInfo(InInfo) { }
		FNewReplicatedActorInfo ActorInfo;
		FActorCellInfo CellInfo;
	};

	TMap<FActorRepListType, FCachedDynamicActorInfo> DynamicSpatializedActors;

	struct FCachedStaticActorInfo
	{
		FCachedStaticActorInfo(const FNewReplicatedActorInfo& InInfo, bool bInDormDriven) : ActorInfo(InInfo), bDormancyDriven(bInDormDriven) { }
		FNewReplicatedActorInfo ActorInfo;
		bool bDormancyDriven = false;	// Removed from the static list if it stops being dormant
	};

	TMap<FActorRepListType, FCachedStaticActorInfo> StaticSpatializedActors;

	/** Static actors that were not fully initialized when they registered. Their placement is deferred a frame. */
	struct FPendingStaticActors
	{
		FPendingStaticActors(const FActorRepListType& InActor, const bool InDormancyDriven) : Actor(InActor), DormancyDriven(InDormancyDriven) { }
		bool operator==(const FActorRepListType& InActor) const { return InActor == Actor; }
		FActorRepListType Actor;
		bool DormancyDriven;
	};
	TArray<FPendingStaticActors> PendingStaticSpatializedActors;

	void OnNetDormancyChange(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo, ENetDormancy NewValue, ENetDormancy OldValue);

	void PutStaticActorIntoCell(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& ActorRepInfo, bool bDormancyDriven);

	UReplicationGraphNode_GridCell* GetCellNode(UReplicationGraphNode_GridCell*& NodePtr)
	{
		if (NodePtr == nullptr)
		{
			if (CreateCellNodeOverride)
			{
				NodePtr = CreateCellNodeOverride(this);
			}
			else
			{
				NodePtr = CreateChildNode<UReplicationGraphNode_GridCell>();
			}
		}

		return NodePtr;
	}

	TArray<TArray<UReplicationGraphNode_GridCell*>> Grid;

	TArray<UReplicationGraphNode_GridCell*>& GetGridX(int32 X)
	{
		if (Grid.Num() <= X)
		{
			Grid.SetNum(X + 1);
		}
		return Grid[X];
	}

	UReplicationGraphNode_GridCell*& GetCell(TArray<UReplicationGraphNode_GridCell*>& GridX, int32 Y)
	{
		if (GridX.Num() <= Y)
		{
			GridX.SetNum(Y + 1);
		}
		return GridX[Y];
	}

	UReplicationGraphNode_GridCell*& GetCell(int32 X, int32 Y)
	{
		TArray<UReplicationGraphNode_GridCell*>& GridX = GetGridX(X);
		return GetCell(GridX, Y);
	}

	bool bNeedsRebuild = false;

	/** Reused scratch array so gathering cells does not allocate. */
	TArray<UReplicationGraphNode_GridCell*> GatheredNodes;
};

// -----------------------------------

class UReplicationGraphNode_AlwaysRelevant : public UReplicationGraphNode
{
public:

	UReplicationGraphNode_AlwaysRelevant();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override { return false; }
	virtual void NotifyResetAllNetworkActors() override { }
	virtual void PrepareForReplication() override;
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	void AddAlwaysRelevantClass(UClass* Class);

protected:

	UReplicationGraphNode* ChildNode = nullptr;

	/** Explicit list of classes that are always relevant. */
	TArray<UClass*> AlwaysRelevantClasses;
};

// -----------------------------------

struct FCachedAlwaysRelevantActorInfo
{
	AActor* LastViewer = nullptr;
	AActor* LastViewTarget = nullptr;
};

/** Adds the actors that are always relevant to one connection: its PlayerController and view target. */
class UReplicationGraphNode_AlwaysRelevant_ForConnection : public UReplicationGraphNode_ActorList
{
public:

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void TearDown() override;

	/** Rebuilt every frame from the connection's state. */
	FActorRepListRefView ReplicationActorList;

	template<typename KeyType, typename ValueType>
	static void CleanupCachedRelevantActors(TMap<TObjectKey<KeyType>, ValueType>& ActorMap)
	{
		for (auto MapIt = ActorMap.CreateIterator(); MapIt; ++MapIt)
		{
			if (MapIt.Key().ResolveObjectPtr() == nullptr)
			{
				MapIt.RemoveCurrent();
			}
		}
	}

protected:

	void AddCachedRelevantActor(const FConnectionGatherActorListParameters& Params, AActor* NewActor, AActor*& LastActor);
	void UpdateCachedRelevantActor(const FConnectionGatherActorListParameters& Params, AActor* NewActor, AActor*& LastActor);

	TMap<TObjectKey<UNetConnection>, FCachedAlwaysRelevantActorInfo> PastRelevantActorMap;
};

// -----------------------------------

struct FTearOffActorInfo
{
	FTearOffActorInfo() : TearOffFrameNum(0), Actor(nullptr), bHasReppedOnce(false) { }
	FTearOffActorInfo(AActor* InActor, uint32 InTearOffFrameNum) : TearOffFrameNum(InTearOffFrameNum), Actor(InActor), bHasReppedOnce(false) { }

	uint32 TearOffFrameNum;
	AActor* Actor;
	bool bHasReppedOnce;
};

/** Handles torn off actors, replicating each one final time to every connection. */
class UReplicationGraphNode_TearOff_ForConnection : public UReplicationGraphNode
{
public:
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override { return false; }
	virtual void NotifyResetAllNetworkActors() override { TearOffActors.Reset(); }
	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;

	void NotifyTearOffActor(AActor* Actor, uint32 FrameNum);

	TArray<FTearOffActorInfo> TearOffActors;

	FActorRepListRefView ReplicationActorList;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UReplicationGraph
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Manages actor replication for an entire world / net driver. */
class UReplicationGraph
{
public:

	UReplicationGraph();
	virtual ~UReplicationGraph();

	/** The per-connection manager class to instantiate. */
	TFunction<UNetReplicationGraphConnection* ()> ReplicationConnectionManagerFactory;

	UNetDriver* NetDriver = nullptr;

	/** Connection managers. Not sorted and not stable. */
	TArray<UNetReplicationGraphConnection*> Connections;

	/** Managers created before the net driver officially had the connection in its client list. */
	TArray<UNetReplicationGraphConnection*> PendingConnections;

	/** The furthest an FActorDestructionInfo may be from a connection and still be replicated. */
	float DestructInfoMaxDistanceSquared = 15000.f * 15000.f;

	// --------------------------------------------------------------
	//	Replication driver interface
	// --------------------------------------------------------------

	virtual void SetRepDriverWorld(UWorld* InWorld);
	virtual void InitForNetDriver(UNetDriver* InNetDriver);
	virtual void InitializeActorsInWorld(UWorld* InWorld);
	virtual void ResetGameWorldState() { }
	virtual void TearDown();

	virtual void AddClientConnection(UNetConnection* NetConnection);
	virtual void RemoveClientConnection(UNetConnection* NetConnection);
	virtual void AddNetworkActor(AActor* Actor);
	virtual void RemoveNetworkActor(AActor* Actor);
	virtual void ForceNetUpdate(AActor* Actor);
	virtual void FlushNetDormancy(AActor* Actor, bool bWasDormInitial);
	virtual void NotifyActorTearOff(AActor* Actor);
	virtual void NotifyActorFullyDormantForConnection(AActor* Actor, UNetConnection* Connection);
	virtual void NotifyActorDormancyChange(AActor* Actor, ENetDormancy OldDormancyState);
	virtual void NotifyActorRenamed(AActor* Actor, UObject* PreviousOuter, FName PreviousName);
	virtual void SetRoleSwapOnReplicate(AActor* Actor, bool bSwapRoles);
	virtual int32 ServerReplicateActors(float DeltaSeconds);

	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo);
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo);

	bool IsConnectionReady(UNetConnection* Connection);

	void SetActorDiscoveryBudget(int32 ActorDiscoveryBudgetInKBytesPerSec);

	/** Sets the global and per-connection cull distance of an actor. */
	void SetAllCullDistanceSettingsForActor(const FActorRepListType& Actor, float CullDistanceSquared);

	// --------------------------------------------------------------

	/** Creates a node owned by the graph. This and UReplicationGraphNode::CreateChildNode are the only node factories. */
	template<class T>
	T* CreateNewNode()
	{
		T* NewNode = new T();
		InitNode(NewNode);
		NewNode->SetDebugName(typeid(T).name());
		return NewNode;
	}

	/** Adds a node that gets a chance to emit lists for every connection. */
	void AddGlobalGraphNode(UReplicationGraphNode* GraphNode);
	void RemoveGlobalGraphNode(UReplicationGraphNode* GraphNode);

	/** Associates a node with one connection. */
	void AddConnectionGraphNode(UReplicationGraphNode* GraphNode, UNetReplicationGraphConnection* ConnectionManager);
	void AddConnectionGraphNode(UReplicationGraphNode* GraphNode, UNetConnection* NetConnection) { AddConnectionGraphNode(GraphNode, FindOrAddConnectionManager(NetConnection)); }

	void RemoveConnectionGraphNode(UReplicationGraphNode* GraphNode, UNetReplicationGraphConnection* ConnectionManager);

	// --------------------------------------------------------------

	UWorld* GetWorld() const { return GraphGlobals.IsValid() ? GraphGlobals->World : nullptr; }

	virtual void LogGraph(FReplicationGraphDebugInfo& DebugInfo) const;
	virtual void LogGlobalGraphNodes(FReplicationGraphDebugInfo& DebugInfo) const;
	virtual void LogConnectionGraphNodes(FReplicationGraphDebugInfo& DebugInfo) const;

	const TSharedPtr<FReplicationGraphGlobalData>& GetGraphGlobals() const { return GraphGlobals; }

	uint32 GetReplicationGraphFrame() const { return ReplicationGraphFrame; }

	/** Affects how an actor's final priority is calculated during the prioritize phase. */
	struct FPrioritizationConstants
	{
		float MaxDistanceScaling = 60000.f * 60000.f;	// Distance scaling saturates here
		uint32 MaxFramesSinceLastRep = 20;				// Starvation scaling saturates here
	};
	FPrioritizationConstants PrioritizationConstants;

	struct FFastSharedPathConstants
	{
		float DistanceRequirementPct = 0.1f;	// Must be this close, as a fraction of cull distance squared, to use the fast path
		int32 MaxBitsPerFrame = 2048;			// 5kBytes/sec @ 20hz
		int32 ListSkipPerFrame = 3;
	};
	FFastSharedPathConstants FastSharedPathConstants;

	// --------------------------------------------------------------

	virtual void ReplicateActorsForConnection(UNetConnection* NetConnection, FPerConnectionActorInfoMap& ConnectionActorInfoMap, UNetReplicationGraphConnection* ConnectionManager, const uint32 FrameNum);

	int64 ReplicateSingleActor(const FActorRepListType& Actor, FConnectionReplicationActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalActorInfo, FPerConnectionActorInfoMap& ConnectionActorInfoMap, UNetReplicationGraphConnection& ConnectionManager, const uint32 FrameNum);

	void UpdateActorChannelCloseFrameNum(AActor* Actor, FConnectionReplicationActorInfo& ConnectionData, const FGlobalActorReplicationInfo& GlobalData, const uint32 FrameNum, UNetConnection* NetConnection) const;

	void NotifyConnectionSaturated(UNetReplicationGraphConnection& Connection);

	void NotifyConnectionFastPathSaturated();

	void SetActorDestructionInfoToIgnoreDistanceCulling(AActor* DestroyedActor);

	uint16 GetReplicationPeriodFrameForFrequency(float NetUpdateFrequency) const
	{
		// The graph is frame based, so NetUpdateFrequency has to become a period in frames at the server tick rate.
		uint32 FramesBetweenUpdates = static_cast<uint32>(FMath::RoundToInt(static_cast<float>(GetNetServerMaxTickRate()) / NetUpdateFrequency));
		FramesBetweenUpdates = FMath::Clamp<uint32>(FramesBetweenUpdates, 1u, static_cast<uint32>(MAX_uint16));

		return static_cast<uint16>(FramesBetweenUpdates);
	}

	int32 GetNetServerMaxTickRate() const;

	/**
	 * How many bits a connection may be sent this frame.
	 *
	 * The engine asks UNetConnection::IsNetReady, which reads QueuedBits and the
	 * pending send buffer. Neither is reflected, and no surviving function reads
	 * them, so the budget is computed here from the connection's negotiated
	 * speed and the server tick rate instead. Same shape of answer, derived from
	 * data that is reachable.
	 */
	int32 GetMaxBitsPerFrameForConnection(UNetConnection* Connection) const;

	UNetReplicationGraphConnection* FindConnectionManager(UNetConnection* NetConnection) const;

	FGlobalActorReplicationInfoMap& GetGlobalActorReplicationInfoMap() { return GlobalActorReplicationInfoMap; }

protected:

	virtual void InitializeForWorld(UWorld* World);

	virtual void InitNode(UReplicationGraphNode* Node);

	/** Override to initialize the per-class replication data. */
	virtual void InitGlobalActorClassSettings();

	/** Override to build the global graph. */
	virtual void InitGlobalGraphNodes();

	/** Override to build the graph for one connection. Nodes may be shared between connections. */
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* ConnectionManager);

	UNetReplicationGraphConnection* FindOrAddConnectionManager(UNetConnection* NetConnection);

	/** Pushes the channel close frame out for the actors we ran out of bandwidth for, and their dependents. */
	void HandleStarvedActorList(const UNetReplicationGraphConnection& RepGraphConnection, const FPrioritizedRepList& List, int32 StartIdx, FPerConnectionActorInfoMap& ConnectionActorInfoMap, uint32 FrameNum);

	/** Routes a rename to every global node. Subclasses will usually want something more direct. */
	virtual void RouteRenameNetworkActorToNodes(const FRenamedReplicatedActorInfo& ActorInfo);

	/** Frames without replicating before an actor channel closes. Added to the actor's own ActorChannelFrameTimeout. */
	uint32 GlobalActorChannelFrameNumTimeout = 0;

	TSharedPtr<FReplicationGraphGlobalData> GraphGlobals;

	/** Scratch list used while prioritizing. */
	FPrioritizedRepList PrioritizedReplicationList;

	/** Nodes that may add actors for every connection. Added through ::AddGlobalGraphNode. */
	TArray<UReplicationGraphNode*> GlobalGraphNodes;

	/** Nodes that want PrepareForReplication() at the top of the frame. */
	TArray<UReplicationGraphNode*> PrepareForReplicationNodes;

	FGlobalActorReplicationInfoMap GlobalActorReplicationInfoMap;

	/** The authoritative set of "what actors are in the graph". */
	TSet<AActor*> ActiveNetworkActors;

	/** Special case handling for specific RPCs. Currently only immediate send/flush for multicasts. */
	TMap<FObjectKey /** UFunction* */, FRPCSendPolicyInfo> RPCSendPolicyMap;

	/** Classes set true here get a channel opened when they receive a multicast RPC and are in range. */
	TClassMap<bool> RPC_Multicast_OpenChannelForClass;

	FName FastSharedReplicationFuncName = FName();

	/** Default replication path. */
	void ReplicateActorListsForConnections_Default(UNetReplicationGraphConnection* ConnectionManager, FGatheredReplicationActorLists& GatheredReplicationListsForConnection, FNetViewerArray& Viewers);

	/** "FastShared" replication path. */
	void ReplicateActorListsForConnections_FastShared(UNetReplicationGraphConnection* ConnectionManager, FGatheredReplicationActorLists& GatheredReplicationListsForConnection, FNetViewerArray& Viewers);

	/** Basic stats about what was replicated this frame. */
	struct FFrameReplicationStats
	{
		int32 NumReplicatedActors = 0;
		int32 NumReplicatedCleanActors = 0;
		int32 NumReplicatedFastPathActors = 0;
		int32 NumConnections = 0;

		void Reset() { *this = FFrameReplicationStats(); }
	};

	virtual void PostServerReplicateStats(const FFrameReplicationStats& Stats) { }

private:

	/** Whether a connection was saturated during this update. */
	bool bWasConnectionSaturated = false;

	/** Whether the FastShared budget was fully spent during this update. */
	bool bWasConnectionFastPathSaturated = false;

	FFrameReplicationStats FrameReplicationStats;

	/** The graph's own frame counter. UNetDriver's can be moved by RPCs and means something else. */
	uint32 ReplicationGraphFrame = 0;

	/** Separate budget for the traffic spent opening actor channels. Ignored when 0. */
	int32 ActorDiscoveryMaxBitsPerFrame = 0;

	/** Which connection gets the expensive per-connection work this frame. */
	int32 HeavyComputationConnectionSelector = 0;

	UNetReplicationGraphConnection* CreateClientConnectionManagerInternal(UNetConnection* Connection);
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UNetReplicationGraphConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

struct FLastLocationGatherInfo
{
	FLastLocationGatherInfo() = default;
	FLastLocationGatherInfo(const UNetConnection* InConnection, FVector InLastLocation)
		: Connection(InConnection)
		, LastLocation(InLastLocation)
		, LastOutOfRangeLocationCheck(InLastLocation)
	{
	}

	const UNetConnection* Connection = nullptr;
	FVector LastLocation = FVector(0.0, 0.0, 0.0);
	FVector LastOutOfRangeLocationCheck = FVector(0.0, 0.0, 0.0);

	bool operator==(const UNetConnection* Other) const { return Connection == Other; }
};

/** Manages actor replication for one connection. */
class UNetReplicationGraphConnection
{
public:

	UNetReplicationGraphConnection() = default;
	virtual ~UNetReplicationGraphConnection() = default;

	UNetConnection* NetConnection = nullptr;

	/** All of our per-actor data. */
	FPerConnectionActorInfoMap ActorInfoMap;

	DECLARE_MULTICAST_DELEGATE_TwoParams(FOnClientVisibleLevelNamesAdd, FName, UWorld*);
	FOnClientVisibleLevelNamesAdd OnClientVisibleLevelNameAdd;	// Called for every level

	DECLARE_MULTICAST_DELEGATE_OneParam(FOnClientVisibleLevelNamesRemove, FName);
	FOnClientVisibleLevelNamesRemove OnClientVisibleLevelNameRemove;

	/** Index in the graph's connection list. Reassigned whenever a client disconnects, so it is only a key within one tick. */
	int32 ConnectionOrderNum = 0;

	TArray<FLastLocationGatherInfo> LastGatherLocations;

	/** Bits spent this frame opening actor channels, when that has its own budget. */
	int32 QueuedBitsForActorDiscovery = 0;

	/** Const so that nodes are added and removed through AddConnectionGraphNode/RemoveConnectionGraphNode. */
	const TArray<UReplicationGraphNode*>& GetConnectionGraphNodes() const { return ConnectionGraphNodes; }

	virtual void TearDown();

	virtual void NotifyClientVisibleLevelNamesAdd(FName LevelName, UWorld* StreamingWorld);
	virtual void NotifyClientVisibleLevelNamesRemove(FName LevelName);

	virtual void NotifyActorChannelAdded(AActor* Actor, UActorChannel* Channel);
	virtual void NotifyActorChannelRemoved(AActor* Actor);
	virtual void NotifyActorChannelCleanedUp(UActorChannel* Channel);

	virtual void NotifyResetAllNetworkActors();

	/** Every level name visible to this connection and its sub-connections. */
	virtual void GetClientVisibleLevelNames(TSet<FName>& OutLevelNames) const;

	/** The visible levels as they were cached at the start of the gather phase. */
	const TSet<FName>& GetCachedClientVisibleLevelNames() const { return CachedVisibleLevels; }

	FActorRepListRefView& GetPrevDormantActorListForNode(const UReplicationGraphNode* GridNode);

	void RemoveActorFromAllPrevDormantActorLists(AActor* InActor);

	struct FVisibleCellInfo
	{
		FIntPoint Location = FIntPoint(0, 0);
		int32 Lifetime = 0;

		friend bool operator==(const FVisibleCellInfo& Cell, const FIntPoint& Point) { return Cell.Location.X == Point.X && Cell.Location.Y == Point.Y; }
	};

	TArray<FVisibleCellInfo>& GetVisibleCellsForNode(const UReplicationGraphNode* GridNode);

	void CleanupNodeCaches(const UReplicationGraphNode* Node);

	friend class UReplicationGraph;

private:

	/** Dormant actors from a cell that was left, for the dormant dynamic actor destruction feature. */
	TMap<TObjectKey<UReplicationGraphNode>, FActorRepListRefView> PrevDormantActorListPerNode;

	/** Which cells were visible per node, so dormant dynamic actor cleanup can be deferred. */
	TMap<TObjectKey<UReplicationGraphNode>, TArray<FVisibleCellInfo>> NodesVisibleCells;

	/** Called right after creation to associate with the owning graph. */
	void InitForGraph(UReplicationGraph* Graph);

	/** Called after InitForGraph to associate with a net connection. */
	void InitForConnection(UNetConnection* Connection);

	void AddConnectionGraphNode(UReplicationGraphNode* Node);
	void RemoveConnectionGraphNode(UReplicationGraphNode* Node);

	bool PrepareForReplication();

	void SetActorNotDormantOnConnection(AActor* InActor);

	void BuildVisibleLevels();

	TArray<UReplicationGraphNode*> ConnectionGraphNodes;

	UReplicationGraphNode_TearOff_ForConnection* TearOffNode = nullptr;

	/** Level names visible to this connection. Valid during the gather phase. */
	TSet<FName> CachedVisibleLevels;

	/** Fed by APlayerController::ServerUpdateLevelVisibility; see FReplicationGraphLevelVisibility. */
	TSet<FName> ClientVisibleLevelNames;
};

PRISM_NAMESPACE_END
