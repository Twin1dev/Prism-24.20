#pragma once

/*=============================================================================
	FortReplicationGraph.h

	Fortnite's Replication Graph, Release-24.20 (CL-25156858).

	What this is reconstructed from, and how much of each:

	  * The class set, the class hierarchy, the member layout of every node and
	    of UFortReplicationGraph itself, and EClassRepNodeMapping's values, all
	    come from the reflection data in the shipped client. Those are exact.

	  * The console variables and their shipped values come out of the binary's
	    static initializers (FortReplicationGraph.cpp still constructs all of
	    them; see the values in FortRepGraph:: below).

	  * The instance-dynamic routing table comes from the one routing helper that
	    survived compilation, which maps IFortReplicationGraphInterface's routing
	    type onto EClassRepNodeMapping as 1 -> RelevantAllInsideFortVolume,
	    2 -> Spatialize_Static, 3 -> Spatialize_Dynamic, 4 -> Spatialize_Dormancy.

	  * The bodies of InitGlobalActorClassSettings, InitGlobalGraphNodes,
	    InitConnectionGraphNodes and RouteAdd/RemoveNetworkActorToNodes are NOT
	    in the client - they are inside WITH_SERVER_CODE and were compiled out,
	    which is why every one of the CVar globals they read has exactly one
	    cross-reference (its own registration). Those bodies are reconstructed
	    from ULyraReplicationGraph, which is Epic's public derivative of this
	    exact graph, and then fitted to Fortnite's node set, routing enum and
	    shipped CVar values. Where a decision could not be recovered it is
	    marked with a comment saying so.
=============================================================================*/

#include "ReplicationGraph.h"

PRISM_NAMESPACE_START

/**
 * The enum that routes an actor to the right node. One class maps to one value.
 *
 * Reflected as FortniteGame.EClassRepNodeMapping; the values below are the
 * shipped ones and must not be renumbered.
 */
enum class EClassRepNodeMapping : uint8
{
	/** Not routed anywhere. Used for actors handled by a special case node. */
	NotRouted = 0,

	/** Routed to the always relevant node, or to the always relevant streaming level list. */
	RelevantAllConnections = 1,

	/** Routed to the global relevancy node of whichever FortVolume grid the actor is inside. */
	RelevantAllInsideFortVolume = 2,

	/** Routed to the node registered for the actor's class in ClassToNodeToRouteTo. */
	Custom = 3,

	// ONLY SPATIALIZED values below here. See UFortReplicationGraph::IsSpatialized.

	/** Routed to the grid: does not move, does not need updating every frame. */
	Spatialize_Static = 4,

	/** Routed to the grid: moves, updated once per frame. */
	Spatialize_Dynamic = 5,

	/** Routed to the grid: static while dormant, dynamic once flushed. */
	Spatialize_Dormancy = 6,

	/** The actor decides for itself, through IFortReplicationGraphInterface. */
	Instance_Dynamic = 7,
};

class UFortReplicationGraphNode_AlwaysRelevant_ForConnection;
class UFortReplicationGraphNode_AlwaysRelevantForSquad;
class UFortReplicationGraphNode_Custom;
class UFortReplicationGraphNode_LiveSpectator;
class UFortReplicationGraphNode_PlayerStateFrequencyLimiter;

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraph
// --------------------------------------------------------------------------------------------------------------------------------------------

class UFortReplicationGraph : public UReplicationGraph
{
public:

	UFortReplicationGraph();

	virtual void ResetGameWorldState() override;

	virtual void InitGlobalActorClassSettings() override;
	virtual void InitGlobalGraphNodes() override;
	virtual void InitConnectionGraphNodes(UNetReplicationGraphConnection* RepGraphConnection) override;
	virtual void RouteAddNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo, FGlobalActorReplicationInfo& GlobalInfo) override;
	virtual void RouteRemoveNetworkActorToNodes(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual int32 ServerReplicateActors(float DeltaSeconds) override;

	/** All distance-based actors go here. */
	UReplicationGraphNode_GridSpatialization2D* RootGridNode = nullptr;

	/** Relevant to every connection, always. */
	UReplicationGraphNode_ActorList* AlwaysRelevantNode = nullptr;

	/** Everything a live spectator needs that a player does not. */
	UFortReplicationGraphNode_LiveSpectator* LiveSpectatorRelevancyNode = nullptr;

	/** Rolling subset of player states for gameplay connections. */
	UFortReplicationGraphNode_PlayerStateFrequencyLimiter* PlayerStateLimiterNode = nullptr;

	/** The same, at a different rate, for replay connections. */
	UFortReplicationGraphNode_PlayerStateFrequencyLimiter* ReplayPlayerStateLimiterNode = nullptr;

	/** Always relevant actors that came from a streaming level, keyed by that level. */
	TMap<FName, FActorRepListRefView> AlwaysRelevantStreamingLevelActors;

	/** Classes routed with EClassRepNodeMapping::Custom, and the node each one goes to. */
	TMap<FObjectKey /** UClass */, UFortReplicationGraphNode_Custom*> ClassToNodeToRouteTo;

	/** One node per squad, created on demand by GetNodeForSquad. */
	TMap<uint8 /** SquadId */, UFortReplicationGraphNode_AlwaysRelevantForSquad*> SquadMap;

	/**
	 * Classes that would spatialize on the strength of their parent's settings but
	 * do not want to. Recorded so the routing decisions can be audited.
	 */
	TArray<UClass*> NonSpatializedChildClasses;

	void PrintRepNodePolicies();

	// -----------------------------------------------------------------------------------
	//	Notifications from game code.
	//
	//	The graph deliberately exposes no generic "add this actor" surface: game code
	//	calls one of these, and the graph decides what that means. In the shipped build
	//	these are wired up from InitGlobalActorClassSettings through the multicast
	//	delegates on AFortPawn/AFortPlayerPawn/AFortGameModeAthena; here they are public
	//	so the corresponding detours can call them directly.
	// -----------------------------------------------------------------------------------

	/**
	 * Weapons never enter the graph on their own. They ride along with the pawn as a
	 * dependent actor, which keeps the graph from having to gather and prioritize them.
	 */
	void OnPawnWeaponChange(AActor* Pawn, AActor* NewWeapon, AActor* OldWeapon);

	/** Same idea as weapons: the vehicle replicates when its driver's pawn does. */
	void OnVehicleChanged(AActor* Pawn, AActor* NewVehicle, AActor* PrevVehicle);

	/** And the parachute, for as long as it is deployed. */
	void OnParachuteStateChange(AActor* Pawn, bool bNewState);

	/** Puts the player state's squad node on that player's connection. */
	void OnPlayerStateSquadAssign(AActor* PlayerState);

	/** Adds or removes a pawn from the always-relevant node, for pawns that need full rate. */
	void OnPlayerPawnHighFrequencyChange(AActor* Pawn, bool bHighFreq);

	/** Re-reads the playlist dependent settings. Called once at init and again per playlist. */
	void OnPlaylistDataLoaded(AActor* GameState);

	/** An actor changed its cull distance: update the global info, every connection, and the grid. */
	void OnCullDistSqChange(AActor* Actor, float DistSq);
	void OnPlayerPawnDistSqChange(AActor* Pawn, float DistSq);
	void OnBuildingDistSqChange(AActor* Building, float Dist);
	void OnPickupCullDistanceChanged(AActor* Pickup, float DistSq);

	/** Bound per streaming level so the level's list is re-sent once one of its actors wakes. */
	void OnAlwaysRelevantStreamingActorDormancyFlush(FActorRepListType Actor, FGlobalActorReplicationInfo& GlobalInfo);

	UFortReplicationGraphNode_AlwaysRelevantForSquad* GetNodeForSquad(uint8 SquadId);

private:

	/**
	 * Brings every tracked pawn's dependent actor list back in step with the pawn.
	 *
	 * Weapons and parachutes are deliberately NotRouted: Fortnite replicates them as dependent
	 * actors of the pawn carrying them, which keeps them out of the gather and out of the
	 * prioritize entirely. In the shipped game AFortPawn broadcasts NotifyWeaponChange and
	 * AFortPlayerPawn broadcasts NotifyParachuteStateChange, and the OnPawnWeaponChange /
	 * OnParachuteStateChange handlers below maintain the lists from those.
	 *
	 * Nothing in this build broadcasts them, and with no dependent actors nothing replicates
	 * another player's weapon at all - so the lists are reconciled once per frame instead. The
	 * cost is a few pointer comparisons per player pawn. If the notifications are ever wired
	 * up, this can go and the handlers take over unchanged.
	 */
	void UpdatePawnDependentActors();

	/**
	 * Asks every tracked player pawn to multicast its movement, at Fortnite's FastShared cadence.
	 *
	 * This is the data-producing half of FastShared replication. See the implementation for why
	 * the engine's shared-bunch optimisation is not reproduced and why that does not change what
	 * the client receives.
	 */
	void DriveFastSharedReplication();

	/** Player pawns whose dependent actors UpdatePawnDependentActors keeps in step. */
	TArray<AActor*> TrackedPawns;

	/** What each tracked pawn's dependent list was last frame, so the difference can be applied. */
	TMap<AActor*, TArray<AActor*>> TrackedPawnDependents;

	/** Scratch for one pawn's current dependents. A member so the reconcile never allocates. */
	TArray<AActor*> DesiredPawnDependents;

	void AddClassRepInfo(UClass* Class, EClassRepNodeMapping Mapping);
	void RegisterClassRepNodeMapping(UClass* Class);
	EClassRepNodeMapping GetClassNodeMapping(UClass* Class) const;

	void RegisterClassReplicationInfo(UClass* Class);
	bool ConditionalInitClassReplicationInfo(UClass* Class, FClassReplicationInfo& ClassInfo);
	void InitClassReplicationInfo(FClassReplicationInfo& Info, UClass* Class, bool Spatialize) const;

	EClassRepNodeMapping GetMappingPolicy(UClass* Class);

	/** Resolves EClassRepNodeMapping::Instance_Dynamic against the actor itself. */
	EClassRepNodeMapping GetInstanceMappingPolicy(const AActor* Actor) const;

	bool IsSpatialized(EClassRepNodeMapping Mapping) const { return Mapping >= EClassRepNodeMapping::Spatialize_Static && Mapping <= EClassRepNodeMapping::Spatialize_Dormancy; }

	TClassMap<EClassRepNodeMapping> ClassRepNodePolicies;

	/** Classes whose replication settings were set explicitly by InitGlobalActorClassSettings. */
	TArray<UClass*> ExplicitlySetClasses;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphNode_AlwaysRelevant_ForConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * Connection specific always-relevant actors.
 *
 * This node does not keep a persistent list; it rebuilds one every frame,
 * because everything in it is reachable from the PlayerController. A persistent
 * list would need change notifications that are not worth the plumbing.
 */
class UFortReplicationGraphNode_AlwaysRelevant_ForConnection : public UReplicationGraphNode_AlwaysRelevant_ForConnection
{
public:
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual void NotifyResetAllNetworkActors() override { }

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;

	void OnClientLevelVisibilityAdd(FName LevelName, UWorld* StreamingWorld);
	void OnClientLevelVisibilityRemove(FName LevelName);

	/** Puts a streaming level's always-relevant list back in play after one of its actors woke. */
	void FlushStreamingAlwaysRelevantList(const FNewReplicatedActorInfo& ActorInfo);

	void ResetGameWorldState();

	/** The graph this node belongs to. Set when the node is created. */
	UFortReplicationGraph* FortGraph = nullptr;

private:

	TArray<FName, TInlineAllocator<64>> AlwaysRelevantStreamingLevelsNeedingReplication;

	bool bInitializedPlayerState = false;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphNode_PlayerStateFrequencyLimiter
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * Player state replication, rate limited.
 *
 * Tracks every player state but hands the driver only a slice of them per
 * frame. With a hundred players in a match this is what stops player state
 * replication dominating the frame, and it lets serialization be shared between
 * the connections that receive the same slice. The owning connection's own
 * player state is not throttled here - that goes out through
 * UFortReplicationGraphNode_AlwaysRelevant_ForConnection.
 */
class UFortReplicationGraphNode_PlayerStateFrequencyLimiter : public UReplicationGraphNode
{
public:
	UFortReplicationGraphNode_PlayerStateFrequencyLimiter();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& Actor) override { }
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override { return false; }
	virtual bool NotifyActorRenamed(const FRenamedReplicatedActorInfo& Actor, bool bWarnIfNotFound = true) override { return false; }

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void PrepareForReplication() override;

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;

	/** How many player states to hand the driver per frame. Does not suppress ForceNetUpdate. */
	int32 TargetActorsPerFrame = 2;

private:

	TArray<FActorRepListRefView> ReplicationActorLists;
	FActorRepListRefView ForceNetUpdateReplicationActorList;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphNode_LiveSpectator
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * Actors a live spectator has to see wherever it is looking from.
 *
 * A spectating connection is not bound to a pawn: it can be anywhere, and it is showing the
 * match to an audience, so the things the broadcast is about - players and supply drops - must
 * not be distance culled away from it. This node holds exactly those, is gathered only for
 * spectating connections, and zeroes their cull distance on that connection so they stay
 * relevant at any range.
 *
 * Which classes qualify is a small explicit table rather than a routing policy, because these
 * actors are already routed somewhere else for everyone who is playing.
 */
class UFortReplicationGraphNode_LiveSpectator : public UReplicationGraphNode_ActorList
{
public:
	UFortReplicationGraphNode_LiveSpectator();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

private:

	/** True for a class a spectator must always see. Inherited down the hierarchy by TClassMap. */
	TClassMap<bool> ClassesForSpectatorRelevancy;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphNode_Custom
// --------------------------------------------------------------------------------------------------------------------------------------------

/**
 * A node game code owns.
 *
 * Classes routed with EClassRepNodeMapping::Custom land in whichever of these
 * was registered for them, which lets a game feature own the relevancy of its
 * own actors without the graph knowing anything about them.
 */
class UFortReplicationGraphNode_Custom : public UReplicationGraphNode
{
public:
	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	/** Set by game code to decide, per connection, whether this node contributes at all. */
	TFunction<bool(const FConnectionGatherActorListParameters&)> ShouldGatherForConnection;

private:

	FActorRepListRefView ReplicationActorList;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphNode_AlwaysRelevantForTeam / ForSquad
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Actors that are always relevant to everyone on one team. Shared by all of that team's connections. */
class UFortReplicationGraphNode_AlwaysRelevantForTeam : public UReplicationGraphNode_ActorList
{
public:
	/** The team's private info actor, which is what most of this node exists to deliver. */
	AActor* FortTeamPrivateInfo = nullptr;

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;
};

/** Actors that are always relevant to everyone in one squad. */
class UFortReplicationGraphNode_AlwaysRelevantForSquad : public UReplicationGraphNode
{
public:
	UFortReplicationGraphNode_AlwaysRelevantForSquad();

	virtual void NotifyAddNetworkActor(const FNewReplicatedActorInfo& ActorInfo) override;
	virtual bool NotifyRemoveNetworkActor(const FNewReplicatedActorInfo& ActorInfo, bool bWarnIfNotFound = true) override;
	virtual void NotifyResetAllNetworkActors() override;

	virtual void PrepareForReplication() override;

	virtual void GatherActorListsForConnection(const FConnectionGatherActorListParameters& Params) override;

	virtual void LogNode(FReplicationGraphDebugInfo& DebugInfo, const std::string& NodeName) const override;

	TArray<AActor*> PlayerStates;
	TArray<AActor*> AlwaysRelevantActors;

	uint8 SquadID = 0;

private:

	/**
	 * Set when the gathered set changed, so the next gather re-zeroes the per-connection
	 * cull distances. Squadmates are relevant to each other at any range.
	 */
	bool bUpdatePerConnectionCullDistances = false;

	FActorRepListRefView GatheredList;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// UFortReplicationGraphConnection
// --------------------------------------------------------------------------------------------------------------------------------------------

/** Fortnite's per-connection manager. */
class UFortReplicationGraphConnection : public UNetReplicationGraphConnection
{
public:
	/** True while this connection is a live spectator rather than a player. */
	bool bIsLiveSpectator = false;
};

// --------------------------------------------------------------------------------------------------------------------------------------------
// Creation
// --------------------------------------------------------------------------------------------------------------------------------------------

namespace FortRepGraph
{
	/**
	 * Creates the graph for a net driver, if that net driver should have one.
	 *
	 * Mirrors the CreateReplicationDriverDelegate the engine would have called:
	 * only the game net driver gets a graph.
	 */
	UReplicationGraph* ConditionalCreateReplicationDriver(UNetDriver* ForNetDriver, UWorld* World);
}

PRISM_NAMESPACE_END
