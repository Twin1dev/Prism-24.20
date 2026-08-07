#pragma once

/*=============================================================================
	EngineAddresses.h: Image-relative addresses and member offsets the
	Replication Graph needs and reflection cannot reach.

	Every value below was recovered from
	  C:\NewDumps\24.20\24.20\FortniteClient-Win64-Shipping_dump_SCY.exe.i64
	(++Fortnite+Release-24.20-CL-25156858). Each entry records how it was found
	so it can be re-derived against another build rather than re-guessed.

	Note on why this file has to exist at all: Fortnite's client executable ships
	the Replication Graph *classes* (their UClasses are registered and the CVars
	in FortReplicationGraph.cpp are still constructed at static-init time) but
	not the server-side bodies - UFortReplicationGraph::InitGlobalGraphNodes and
	friends were compiled out, which is why the CVar globals they would read have
	exactly one cross-reference each: the FAutoConsoleVariableRef that registers
	them. The graph therefore has to be rebuilt in this module and driven from
	our own UNetDriver::TickFlush, calling back into the engine through the
	entry points named here.
=============================================================================*/

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

namespace EngineAddresses
{
	/*-------------------------------------------------------------------------
		Functions (image-relative)
	-------------------------------------------------------------------------*/

	/**
	 * void AActor::CallPreReplication(UNetDriver* NetDriver)
	 * Named in the database; confirmed by its use from the replication path at
	 * 0x80DF899, immediately after UNetDriver::IsDormInitialStartupActor.
	 */
	inline constexpr uint64 AActor_CallPreReplication = 0x07CF4374;

	/**
	 * int64 UActorChannel::ReplicateActor()
	 * Owner of "ReplicateActor called while already replicating! %s" (0xDDCDE30)
	 * and "ReplicateActor called with PendingKill Actor! %s" (0xDDCDDC0).
	 */
	inline constexpr uint64 UActorChannel_ReplicateActor = 0x07E412AC;

	/**
	 * UChannel* UNetConnection::CreateChannelByName(const FName& ChName, EChannelCreateFlags CreateFlags, int32 ChIndex)
	 * Call site at 0x80DFA0B passes NAME_Actor, CreateFlags 2 (OpenedLocally) and ChIndex -1.
	 */
	inline constexpr uint64 UNetConnection_CreateChannelByName = 0x01250FC0;

	/**
	 * bool UActorChannel::SetChannelActor(AActor* InActor, ESetChannelActorFlags Flags)
	 * Called on the freshly created channel at 0x80DFA30 with Flags = None.
	 */
	inline constexpr uint64 UActorChannel_SetChannelActor = 0x0138F900;

	/**
	 * bool UNetDriver::IsDormInitialStartupActor(AActor* Actor)
	 * Named in the database; the guard at the top of UNetDriver::AddNetworkActor.
	 */
	inline constexpr uint64 UNetDriver_IsDormInitialStartupActor = 0x0117DEDC;

	/**
	 * void UNetDriver::AddNetworkActor(AActor* Actor)
	 * Recovered at 0x117DE84: guards on IsDormInitialStartupActor, adds to the
	 * network object list, then forwards to ReplicationDriver through vtable
	 * slot 94. This is the function the graph detours so that actors entering
	 * the world are routed into it exactly when the engine would have routed
	 * them into UReplicationDriver::AddNetworkActor.
	 */
	inline constexpr uint64 UNetDriver_AddNetworkActor = 0x0117DE84;

	/*-------------------------------------------------------------------------
		The rest of the UReplicationDriver notification surface.

		The engine tells its replication driver about far more than actors
		appearing. Every one of the functions below is the engine-side wrapper
		that would have forwarded to UReplicationDriver, identified by the
		vtable slot it dispatches through (see the slot table further down) and
		by the non-driver work it does alongside. None of them are inside
		WITH_SERVER_CODE, so all of them are present and all of them run - they
		just find UNetDriver::ReplicationDriver null and do nothing. Detouring
		them is what puts the graph back on the receiving end.
	-------------------------------------------------------------------------*/

	/**
	 * void UNetDriver::RemoveNetworkActor(AActor* Actor)
	 * 0x117D2F4: FNetworkObjectList::Remove, then drops the actor from
	 * RenamedStartupActors, then dispatches slot 95.
	 *
	 * Without this the graph is never told an actor left. It keeps raw pointers
	 * to destroyed actors in its node lists and dereferences them on a later
	 * frame.
	 */
	inline constexpr uint64 UNetDriver_RemoveNetworkActor = 0x0117D2F4;

	/**
	 * void UNetDriver::ForceNetUpdate(AActor* Actor)
	 * 0x117D0FC: dispatches slot 96 when a driver is installed, and otherwise
	 * falls back to pulling the actor's FNetworkObjectInfo forward. Structurally
	 * identical to the same function in the typed PDB build.
	 *
	 * This is how all of game code says "send this now". Nothing arrives out of
	 * band without it.
	 */
	inline constexpr uint64 UNetDriver_ForceNetUpdate = 0x0117D0FC;

	/**
	 * void UNetDriver::FlushActorDormancy(AActor* Actor, bool bWasDormInitial)
	 * 0x7FC1120: dispatches slot 97 (UReplicationDriver::FlushNetDormancy) and
	 * then calls UNetDriver::FlushActorDormancyInternal (0x7FC1188), which is
	 * the shared tail it has with NotifyActorDormancyChange below.
	 *
	 * Buildings are dormant until something hits them. This is the notification
	 * that wakes one back up, so without it a harvested building never leaves
	 * the dormancy node and its damage and destruction never replicate.
	 */
	inline constexpr uint64 UNetDriver_FlushActorDormancy = 0x07FC1120;

	/**
	 * void AActor::FlushNetDormancy()
	 * 0x183F964, reached from AActor::execFlushNetDormancy (0x183F950).
	 *
	 * The actor-side half of the notification above, and the reason the driver-side
	 * one never fires here. Its dispatch is:
	 *
	 *     UWorld::AddNetworkActor(World, this)                          // 0x117DD9C
	 *     Context = UEngine::GetWorldContextFromWorld(GEngine, World)   // 0xDAE464
	 *     for (FNamedNetDriver& D : Context->ActiveNetDrivers)          // +0x208, count +0x210, stride 0x10
	 *         if (D.NetDriver->ShouldReplicateActor(this))              // vtable slot 138
	 *             UNetDriver::FlushActorDormancy(D.NetDriver, this, bWasDormInitial);
	 *
	 * It only ever talks to drivers on the world context's ActiveNetDrivers list, and
	 * this module's driver is deliberately absent from it - UWorld::Listen builds it
	 * with CreateNetDriver rather than CreateNamedNetDriver precisely so the engine's
	 * teardown cannot destroy it out from under the level collections. The consequence
	 * is that no dormancy flush has ever reached the Replication Graph.
	 *
	 * Identified by five independent landmarks, all of which agree: the
	 * "NET_AActor_FlushNetDormancy" stat string, InternalGetNetMode() != NM_Client,
	 * NetDormancy at +0x14D (which the SDK confirms) tested for > DORM_Awake and
	 * DORM_Initial, and the call to UNetDriver_FlushActorDormancy above.
	 */
	inline constexpr uint64 AActor_FlushNetDormancy = 0x0183F964;

	/**
	 * void APlayerController::ServerUpdateLevelVisibility_Implementation(const FUpdateLevelVisibilityLevelInfo&)
	 * 0x249E5EC. Not virtual, so it is detoured by address rather than by vtable slot.
	 *
	 * This is how the client tells the server which streaming levels it has finished
	 * loading, and it is the only source for UNetReplicationGraphConnection::
	 * ClientVisibleLevelNames - which had no writer at all, so the set was permanently
	 * empty and FConnectionGatherActorListParameters::CheckClientVisibilityForLevel
	 * answered false for every streaming level. On a World Partition map that is every
	 * tree, door, chest and prop: they were tracked by the grid and never gathered for
	 * anyone. See UNetDriver::Initialize.
	 *
	 * ServerUpdateMultipleLevelsVisibility (the batched form the client actually uses
	 * most of the time) needs no separate detour: its exec thunk validates each element
	 * and then loops calling this function once per FUpdateLevelVisibilityLevelInfo,
	 * stride 0x10, which matches the struct's size in the SDK.
	 */
	inline constexpr uint64 APlayerController_ServerUpdateLevelVisibility = 0x0249E5EC;

	/**
	 * void UNetDriver::NotifyActorTearOff(AActor* Actor)
	 * 0x7FC764C: dispatches slot 98, then does the Iris equivalent.
	 */
	inline constexpr uint64 UNetDriver_NotifyActorTearOff = 0x07FC764C;

	/**
	 * void UNetDriver::NotifyActorDormancyChange(AActor* Actor, ENetDormancy OldDormancyState)
	 * 0x7FC725C: dispatches slot 100, then FlushActorDormancyInternal.
	 */
	inline constexpr uint64 UNetDriver_NotifyActorDormancyChange = 0x07FC725C;

	/**
	 * void UNetDriver::SetRoleSwapOnReplicate(AActor* Actor, bool bSwapRoles)
	 * 0x7FCF9F0: sets bSwapRolesOnReplicate on the actor's FNetworkObjectInfo
	 * (bit 4 of the byte at +208) and dispatches slot 102.
	 */
	inline constexpr uint64 UNetDriver_SetRoleSwapOnReplicate = 0x07FCF9F0;

	/**
	 * void APlayerController::SendClientAdjustment()
	 *
	 * 0x80B71F4. Confirmed by shape: it reads ServerFrameInfo, calls ClientRecvServerAckFrame,
	 * tests Pawn (+0x2C8) against AcknowledgedPawn (+0x330) and SpectatorPawn, requires
	 * RemoteRole == ROLE_AutonomousProxy and AActor::InternalGetNetMode() != NM_Client, then
	 * reaches the movement component through vtable slot 223 and dispatches slot 2 of its
	 * UNetworkPredictionInterface.
	 *
	 * This is the server-to-client half of the character movement protocol - it is what flushes
	 * ClientAckGoodMove and ClientAdjustPosition. The shipped graph calls it once per connection
	 * per frame before anything else and never lets bandwidth suppress it;
	 * UNetDriver::ServerReplicateActors would have called it too and is a stub here, so without
	 * the graph doing it nothing does.
	 */
	inline constexpr uint64 APlayerController_SendClientAdjustment = 0x080B71F4;

	/**
	 * bool AFortPlayerPawnAthena::UpdateSharedReplication()
	 *
	 * 0x9A07104. The engine-side half of FastShared replication, and the exact shape Lyra's
	 * public equivalent has: bail unless Role == ROLE_Authority, bail if there is no root
	 * component or the pawn is attached, fill an FSharedRepMovement from the character, compare
	 * it against LastSharedReplication, and only if it differs store it and fire the
	 * FastSharedReplication multicast.
	 *
	 * Because it only fires the multicast when the movement actually changed, it is safe to call
	 * every frame - it throttles itself.
	 */
	inline constexpr uint64 AFortPlayerPawnAthena_UpdateSharedReplication = 0x09A07104;

	/*-------------------------------------------------------------------------
		Member offsets
	-------------------------------------------------------------------------*/

	/** UNetDriver::ReplicationDriver - loaded as a1[226] by UNetDriver::AddNetworkActor. */
	inline constexpr int32 UNetDriver_ReplicationDriver = 0x710;

	/**
	 * UNetDriver::ReplicationFrame.
	 *
	 * The single most load-bearing number in this file: FReplicationChangelistMgr::Update
	 * skips comparing an actor's properties whenever its LastReplicationFrame still equals
	 * this counter. That is what net.ShareShadowState buys - compare once per frame, share the
	 * result across connections. Leave it frozen and every actor is compared exactly once, on
	 * the frame its channel opens, and looks clean for the rest of the match.
	 *
	 * Derived from FObjectReplicator::ReplicateProperties (0x7E62E38), which passes it down as
	 *     *(uint32*)(*(void**)(Replicator + 80) + 0x58) + 1080)
	 * where Replicator + 80 is the UNetConnection and Connection + 0x58 is Driver (reflected,
	 * so the SDK confirms it). 1080 = 0x438.
	 *
	 * Cross-checked against the typed PDB, which puts ReplicationFrame at 0x420: that build's
	 * UNetDriver runs 0x18 smaller by this point (NetDriverName at 0x190 against the SDK's
	 * 0x1A8), and 0x420 + 0x18 = 0x438. Two independent derivations agree.
	 */
	inline constexpr int32 UNetDriver_ReplicationFrame = 0x438;

	/** UNetConnection::ActorChannels - the TMap indexed at Connection + 5160 in the replication path. */
	inline constexpr int32 UNetConnection_ActorChannels = 0x1428;

	/**
	 * USceneComponent::ComponentToWorld.
	 *
	 * Recovered from the binary, not derived from the layout. Two independent
	 * exec thunks read it and agree:
	 *
	 *   AActor::execK2_GetActorLocation   (0x242C344) reads 24 bytes out of
	 *       RootComponent + 0x260 - a 16-byte load at 0x260 and an 8-byte load
	 *       at 0x270 - and hands them back as the FVector result.
	 *   AActor::execK2_GetActorRotation   (0x26DEB04) reads the 32-byte FQuat4d
	 *       at RootComponent + 0x240 (16-byte loads at 0x240 and 0x250), compares
	 *       it against the FRotationConversionCache at 0x1C0/0x1D0/0x1E0 and
	 *       converts on a miss.
	 *   USceneComponent::execK2_GetComponentLocation (0x286FDD8) reads the same
	 *       0x260 on the component itself.
	 *
	 * FTransform under LWC is { FQuat4d Rotation; FVector4d Translation;
	 * FVector4d Scale3D; } - 0x60 bytes - so Rotation at 0x240 puts Translation
	 * at 0x260, exactly what the location thunk reads, and ends the transform at
	 * 0x2A0, which is sizeof(USceneComponent). Three readings, one answer.
	 *
	 * This was previously guessed at 0x190 from the reflected layout. That was
	 * wrong by 0xB0: it read the padding after the bitfields at 0x188, so every
	 * actor's world location came back as zero. With every actor and every viewer
	 * at the origin the spatialization grid collapses into a single cell, no
	 * distance cull ever rejects anything, and every connection is handed the
	 * whole world every frame - which saturates the connection and starves real
	 * updates. If world locations ever look wrong again, re-derive from the exec
	 * thunks above rather than from the class layout.
	 */
	inline constexpr int32 USceneComponent_ComponentToWorld = 0x240;

	/** FTransform under LWC is { FQuat4d Rotation; FVector4d Translation; FVector4d Scale3D; }. */
	inline constexpr int32 FTransform_Rotation = 0x00;
	inline constexpr int32 FTransform_Translation = 0x20;

	/*-------------------------------------------------------------------------
		Virtual function slots
	-------------------------------------------------------------------------*/

	/**
	 * UChannel::Close(EChannelCloseReason Reason)
	 * Dispatched as (*(void(**)(UChannel*, EChannelCloseReason))(*(void**)Channel + 712))(...)
	 * from the replication path; 712 / sizeof(void*) == 89.
	 */
	inline constexpr int32 UChannel_Close_VTableSlot = 89;

	/**
	 * UReplicationDriver's vtable, as this build lays it out.
	 *
	 * Recovered by taking the dispatch displacement out of each engine-side
	 * wrapper and matching the resulting run against UReplicationDriver's
	 * declaration order:
	 *
	 *     0x2F0 = 94   UNetDriver::AddNetworkActor          (0x117DE84)
	 *     0x2F8 = 95   UNetDriver::RemoveNetworkActor       (0x117D2F4)
	 *     0x300 = 96   UNetDriver::ForceNetUpdate           (0x117D0FC)
	 *     0x308 = 97   UNetDriver::FlushActorDormancy       (0x7FC1120)
	 *     0x310 = 98   UNetDriver::NotifyActorTearOff       (0x7FC764C)
	 *     0x320 = 100  UNetDriver::NotifyActorDormancyChange(0x7FC725C)
	 *     0x330 = 102  UNetDriver::SetRoleSwapOnReplicate   (0x7FCF9F0)
	 *
	 * Six recovered slots, six consecutive declarations, no gaps other than the
	 * two the declaration order predicts (99 NotifyActorFullyDormantForConnection,
	 * 101 NotifyDestructionInfoCreated). The same exercise against the typed PDB
	 * build gives the identical run one slot lower, which is the only difference
	 * between the two builds' UObject bases.
	 *
	 * These are only needed if the graph is ever installed as a real
	 * UReplicationDriver rather than driven from detours; they are recorded
	 * because they are what identifies each detour target above.
	 */
	inline constexpr int32 UReplicationDriver_SetRepDriverWorld_VTableSlot = 88;
	inline constexpr int32 UReplicationDriver_InitForNetDriver_VTableSlot = 89;
	inline constexpr int32 UReplicationDriver_InitializeActorsInWorld_VTableSlot = 90;
	inline constexpr int32 UReplicationDriver_ResetGameWorldState_VTableSlot = 91;
	inline constexpr int32 UReplicationDriver_AddClientConnection_VTableSlot = 92;
	inline constexpr int32 UReplicationDriver_RemoveClientConnection_VTableSlot = 93;
	inline constexpr int32 UReplicationDriver_AddNetworkActor_VTableSlot = 94;
	inline constexpr int32 UReplicationDriver_RemoveNetworkActor_VTableSlot = 95;
	inline constexpr int32 UReplicationDriver_ForceNetUpdate_VTableSlot = 96;
	inline constexpr int32 UReplicationDriver_FlushNetDormancy_VTableSlot = 97;
	inline constexpr int32 UReplicationDriver_NotifyActorTearOff_VTableSlot = 98;
	inline constexpr int32 UReplicationDriver_NotifyActorFullyDormantForConnection_VTableSlot = 99;
	inline constexpr int32 UReplicationDriver_NotifyActorDormancyChange_VTableSlot = 100;
	inline constexpr int32 UReplicationDriver_NotifyDestructionInfoCreated_VTableSlot = 101;
	inline constexpr int32 UReplicationDriver_SetRoleSwapOnReplicate_VTableSlot = 102;
	inline constexpr int32 UReplicationDriver_ServerReplicateActors_VTableSlot = 103;
}

/** Resolves an image-relative address recorded above to a callable function pointer. */
template<typename FunctionType>
inline FunctionType GetEngineFunction(uint64 RelativeVirtualAddress)
{
	return reinterpret_cast<FunctionType>(InSDKUtils::GetImageBase() + RelativeVirtualAddress);
}

/** Reads a non-reflected member at a recovered offset. */
template<typename MemberType, typename ObjectType>
inline MemberType& GetEngineMember(ObjectType* Object, int32 Offset)
{
	return *reinterpret_cast<MemberType*>(reinterpret_cast<uint8*>(Object) + Offset);
}

template<typename MemberType, typename ObjectType>
inline const MemberType& GetEngineMember(const ObjectType* Object, int32 Offset)
{
	return *reinterpret_cast<const MemberType*>(reinterpret_cast<const uint8*>(Object) + Offset);
}

PRISM_NAMESPACE_END
