#include "pch.h"

#include "Engine/NetDriver.h"

#include "Engine/NetConnection.h"
#include "GameFramework/PlayerController.h"
#include "Online/FortReplicationGraph.h"
#include "ReplicationGraph.h"

PRISM_NAMESPACE_START

/**
 * The graph for the game net driver.
 *
 * The engine keeps one per net driver in UNetDriver::ReplicationDriver; there is
 * one here because a server process only ever has one game net driver, and the
 * graph cannot live in the driver's own memory (see ReplicationGraphTypes.h).
 */
static UReplicationGraph* GReplicationGraph = nullptr;

UReplicationGraph* UNetDriver::GetReplicationGraph()
{
	return GReplicationGraph;
}

// --------------------------------------------------------------------------------------------
//	TEMPORARY: bHasServerFinishedLoading resend probe
//
//	The client will not leave the loading screen until it sees this bool, and it never logs
//	OnRep_bHasServerFinishedLoading. The server is already exonerated: it sets the flag, the
//	graph replicates the controller every frame, and UActorChannel::ReplicateActor returns 0
//	bits, which for that function means "clean" - so the value was sent and acknowledged as
//	written at least once.
//
//	This forces a second, unambiguous send. The flag is driven back to false, left there long
//	enough for the graph to replicate that, then set true again. That is a real property change
//	either side of a real replication, so it produces a fresh bunch rather than relying on the
//	first one having arrived.
//
//	  - client logs OnRep after the probe  -> the first bunch was lost and never retried, and
//	                                          the problem is delivery
//	  - client still logs nothing          -> it is receiving and not acting, and the gate is
//	                                          upstream in AFortPlayerControllerZone::CheckIfLoaded
//
//	Set GResendProbeEnabled to 0 to remove it.
// --------------------------------------------------------------------------------------------

namespace
{
	/*
	 * Off. It did its job: forcing a change the graph should have picked up and seeing zero
	 * bits written is what proved the property comparison was not running at all, which led to
	 * the ReplicationFrame fix in UReplicationGraph::ServerReplicateActors.
	 *
	 * Kept rather than deleted because it is the only tool here that can distinguish "the
	 * server never sent it" from "the client ignored it". Note that it mutates replicated
	 * state, so do not leave it enabled in a build anyone plays on.
	 */
	int32 GResendProbeEnabled = 0;

	/** AFortPlayerController::bHasServerFinishedLoading. Confirmed at runtime by the OwnerRep log. */
	constexpr int32 GHasServerFinishedLoadingOffset = 0x0BF9;

	/** Frames to wait after the flag first reads true before disturbing it. */
	constexpr uint64 GFramesBeforeProbe = 60;

	/** Frames to leave it false, so the graph gets to replicate the false. */
	constexpr uint64 GFramesHeldFalse = 5;

	enum class EResendProbeStage : uint8
	{
		Waiting,
		HoldingFalse,
		Done,
	};

	struct FResendProbeState
	{
		EResendProbeStage Stage = EResendProbeStage::Waiting;
		uint64 DueFrame = 0;
	};

	uint64 GTickFlushFrame = 0;
	TMap<FObjectKey /** APlayerController */, FResendProbeState> GResendProbes;

	uint8* GetHasServerFinishedLoading(APlayerController* Controller)
	{
		return reinterpret_cast<uint8*>(Controller) + GHasServerFinishedLoadingOffset;
	}

	void TickResendProbe(UNetConnection* Connection)
	{
		// APlayerController names Prism's extension in this file now that PlayerController.h is
		// included; the connection hands back the generated SDK type.
		APlayerController* Controller = Connection
			? reinterpret_cast<APlayerController*>(Connection->GetPlayerController())
			: nullptr;
		if (!Controller)
		{
			return;
		}

		uint8* Flag = GetHasServerFinishedLoading(Controller);

		FResendProbeState& State = GResendProbes.FindOrAdd(FObjectKey(reinterpret_cast<UObject*>(Controller)));

		switch (State.Stage)
		{
			case EResendProbeStage::Waiting:
			{
				// Nothing to resend until the server has actually set it.
				if (*Flag == 0)
				{
					return;
				}

				if (State.DueFrame == 0)
				{
					State.DueFrame = GTickFlushFrame + GFramesBeforeProbe;
					return;
				}

				if (GTickFlushFrame < State.DueFrame)
				{
					return;
				}

				*Flag = 0;
				State.Stage = EResendProbeStage::HoldingFalse;
				State.DueFrame = GTickFlushFrame + GFramesHeldFalse;

				UE_LOG(LogPrism, Display, "ResendProbe: cleared bHasServerFinishedLoading on %s (frame %llu)",
					reinterpret_cast<AActor*>(Controller)->GetName().c_str(), GTickFlushFrame);
				break;
			}

			case EResendProbeStage::HoldingFalse:
			{
				if (GTickFlushFrame < State.DueFrame)
				{
					return;
				}

				*Flag = 1;
				State.Stage = EResendProbeStage::Done;

				UE_LOG(LogPrism, Display, "ResendProbe: restored bHasServerFinishedLoading on %s (frame %llu)",
					reinterpret_cast<AActor*>(Controller)->GetName().c_str(), GTickFlushFrame);
				break;
			}

			case EResendProbeStage::Done:
			default:
			{
				break;
			}
		}
	}
}

namespace
{
	/**
	 * The graph, if it is the one that belongs to this driver.
	 *
	 * Every detour below is on a function the engine calls on whichever net driver owns the
	 * actor - beacon drivers included - so each has to check that it is talking about the game
	 * net driver's graph before forwarding.
	 */
	UReplicationGraph* GetGraphFor(const UNetDriver* Driver)
	{
		return (GReplicationGraph != nullptr && GReplicationGraph->NetDriver == Driver) ? GReplicationGraph : nullptr;
	}
}

void UNetDriver::AddNetworkActor(AActor* Actor)
{
	AddNetworkActor_Original(this, Actor);

	// This is exactly where the engine would have called UReplicationDriver::AddNetworkActor
	// through vtable slot 94. The graph is not the registered driver, so it is called here.
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->AddNetworkActor(Actor);
	}
}

void UNetDriver::RemoveNetworkActor(AActor* Actor)
{
	/*
	 * Forwarded BEFORE the original, unlike AddNetworkActor.
	 *
	 * The engine's own body only unhooks the actor from the network object list and the
	 * renamed-startup set, so the order does not matter to it - but it matters here, because
	 * the graph has to walk its node lists and its per-connection actor info while the actor
	 * is still a live object. UReplicationGraph::RemoveNetworkActor reads the actor's class and
	 * its streaming level to work out which node it went into.
	 *
	 * This is the notification that was missing entirely. Actors went into the graph and never
	 * came out, so once GC collected one the graph was left holding a freed pointer and
	 * dereferenced it on a later frame - and, before it ever got that far, clients were never
	 * told about actors that had stopped replicating.
	 */
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->RemoveNetworkActor(Actor);
	}

	RemoveNetworkActor_Original(this, Actor);
}

void UNetDriver::ForceNetUpdate(AActor* Actor)
{
	/*
	 * The engine's body is an either/or: with a driver installed it dispatches and returns,
	 * and only without one does it fall through to pulling FNetworkObjectInfo::NextUpdateTime
	 * forward. That fallback is the legacy replicator's mechanism and is dead here, so running
	 * the original as well is harmless - but it is also pointless, and it walks a hash map, so
	 * the graph's answer is the only one taken when the graph is the driver.
	 */
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->ForceNetUpdate(Actor);
		return;
	}

	ForceNetUpdate_Original(this, Actor);
}

void UNetDriver::FlushActorDormancy(AActor* Actor, bool bWasDormInitial)
{
	/*
	 * Before the original, for the same reason as RemoveNetworkActor: the tail of this function
	 * is FlushActorDormancyInternal, which clears the actor out of every connection's dormancy
	 * bookkeeping. The graph wants to see the actor while that bookkeeping still describes it.
	 *
	 * This is the harvesting notification. A BuildingSMActor sits in the grid's dormancy node
	 * doing nothing until it is damaged; the damage calls AActor::FlushNetDormancy, which
	 * arrives here. Without the forward the graph never moves the building out of the dormant
	 * list, so neither the damage nor the destruction is ever gathered for anyone.
	 */
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->FlushNetDormancy(Actor, bWasDormInitial);
	}

	FlushActorDormancy_Original(this, Actor, bWasDormInitial);
}

void AActor::FlushNetDormancy()
{
	/*
	 * The reason the notification above never arrived.
	 *
	 * AActor::FlushNetDormancy dispatches over FWorldContext::ActiveNetDrivers, and this
	 * module's driver is deliberately not registered there: UWorld::Listen builds it with
	 * CreateNetDriver rather than CreateNamedNetDriver so that the engine's teardown cannot
	 * destroy it while the level collections still point at it. The cost of staying off that
	 * list is that every push notification the engine sends through it - this one included -
	 * goes to drivers we do not own and never to ours.
	 *
	 * So no dormancy flush ever reached the graph. Buildings are the only class family routed
	 * to Spatialize_Dormancy, which is why they alone were affected: a door's state change or
	 * a pickaxe hit woke the actor on the server and the graph was never told, so the client
	 * kept the state it had loaded with the level. The door reverts and refuses to open again
	 * because the two sides never agreed it was open; the building shows no damage because the
	 * damage never left the server.
	 *
	 * NetDormancy has to be sampled before the original runs, because the original is what
	 * converts DORM_Initial into DORM_DormantAll and that transition is bWasDormInitial.
	 */
	const ENetDormancy PreviousDormancy = NetDormancy;

	FlushNetDormancy_Original(this);

	// Mirrors the original's own guard: a client never flushes, and an actor that is not
	// dormant has nothing to wake from.
	if (PreviousDormancy <= ENetDormancy::DORM_Awake)
	{
		return;
	}

	if (UWorld* World = UWorld::GetWorld())
	{
		if (UNetDriver* Driver = reinterpret_cast<UNetDriver*>(World->NetDriver))
		{
			// Prism's override, not the engine's - it is what forwards to the graph before
			// letting FlushActorDormancyInternal clear the per-connection bookkeeping.
			Driver->FlushActorDormancy(this, PreviousDormancy == ENetDormancy::DORM_Initial);
		}
	}
}

void APlayerController::ServerUpdateLevelVisibility(const SDK::FUpdateLevelVisibilityLevelInfo& LevelVisibility)
{
	// After the original: it is what validates the request and updates the engine's own
	// UNetConnection::ClientVisibleLevelNames. The graph's copy should only ever reflect a
	// change the engine actually accepted.
	ServerUpdateLevelVisibility_Original(this, LevelVisibility);

	/*
	 * The notification the graph was never given.
	 *
	 * UNetReplicationGraphConnection::ClientVisibleLevelNames had no writer anywhere in the
	 * tree - only its own definition and declaration - so it stayed empty for the lifetime of
	 * every connection. BuildVisibleLevels() then cached an empty set each frame, and
	 * FConnectionGatherActorListParameters::CheckClientVisibilityForLevel answered false for
	 * every streaming level name, which made both gather paths drop everything they held:
	 * FStreamingLevelActorListCollection::GatherActorLists and
	 * UReplicationGraphNode_ConnectionDormancyNode::GatherActorListsForConnection.
	 *
	 * Asteria is World Partition, so essentially every gameplay actor lives in a runtime
	 * streaming cell rather than the persistent level. The grid tracked ~2500 static actors
	 * and gathered 27, forever - the diagnostic signature being a large tracked count with
	 * distcull=0, i.e. actors that are not being culled because they never reach the gather.
	 * No building ever got a channel, which is why doors never changed state on the client
	 * and a harvested tree stayed standing while its server-side collision was gone.
	 *
	 * Unlike the dormancy notifications this one does not go through the net driver at all,
	 * so it is not an ActiveNetDrivers problem - the hook simply did not exist.
	 */
	UReplicationGraph* Graph = UNetDriver::GetReplicationGraph();
	if (Graph == nullptr)
	{
		return;
	}

	SDK::UNetConnection* OwningConnection = GetNetConnection();
	if (OwningConnection == nullptr)
	{
		return;
	}

	UNetReplicationGraphConnection* ConnectionManager =
		Graph->FindConnectionManager(reinterpret_cast<UNetConnection*>(OwningConnection));
	if (ConnectionManager == nullptr)
	{
		return;
	}

	// PackageName, not Filename: the streaming level name the graph files actors under comes
	// from FNewReplicatedActorInfo::GetStreamingLevelNameOfActor, which is the level's package.
	if (LevelVisibility.bIsVisible)
	{
		ConnectionManager->NotifyClientVisibleLevelNamesAdd(LevelVisibility.PackageName, nullptr);
	}
	else
	{
		ConnectionManager->NotifyClientVisibleLevelNamesRemove(LevelVisibility.PackageName);
	}
}

void UNetDriver::NotifyActorDormancyChange(AActor* Actor, ENetDormancy OldDormancyState)
{
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->NotifyActorDormancyChange(Actor, OldDormancyState);
	}

	NotifyActorDormancyChange_Original(this, Actor, OldDormancyState);
}

void UNetDriver::NotifyActorTearOff(AActor* Actor)
{
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->NotifyActorTearOff(Actor);
	}

	NotifyActorTearOff_Original(this, Actor);
}

void UNetDriver::SetRoleSwapOnReplicate(AActor* Actor, bool bSwapRoles)
{
	if (UReplicationGraph* Graph = GetGraphFor(this))
	{
		Graph->SetRoleSwapOnReplicate(Actor, bSwapRoles);
	}

	SetRoleSwapOnReplicate_Original(this, Actor, bSwapRoles);
}

/*
 * There is deliberately no hook on UNetDriver::ServerReplicateActors.
 *
 * TickFlush's server branch does call it - virtual slot 110, `call qword ptr [rax+880]` - but
 * in this binary that slot (0x7FCF490, 0x60 bytes) is a stub: a stat scope and `return 0`. The
 * body is inside WITH_SERVER_CODE and was compiled out of the client, exactly like the
 * replication graph bodies this module reconstructs. So calling TickFlush_Original below does
 * NOT run a second replication pass, and the graph is already the only replicator.
 */

void UNetDriver::TickFlush(float DeltaSeconds)
{
	if (ClientConnections.Num() > 0 && ClientConnections[0]->InternalAck == false)
	{
		FModuleManager::Get().TickModules(DeltaSeconds);

		// The graph is created the first time this driver has a world and a client, which
		// is the point the engine would have created its replication driver.
		if (GReplicationGraph == nullptr)
		{
			if (UWorld* World = UWorld::GetWorld())
			{
				GReplicationGraph = FortRepGraph::ConditionalCreateReplicationDriver(this, World);

				if (GReplicationGraph != nullptr)
				{
					GReplicationGraph->SetRepDriverWorld(World);
					GReplicationGraph->InitForNetDriver(this);
					GReplicationGraph->InitializeActorsInWorld(World);
				}
			}
		}

		if (GReplicationGraph != nullptr)
		{
			// Pick up any connection the graph has not been told about yet.
			for (SDK::UNetConnection* ClientConnection : ClientConnections)
			{
				UNetConnection* Connection = static_cast<UNetConnection*>(ClientConnection);
				if (GReplicationGraph->FindConnectionManager(Connection) == nullptr)
				{
					GReplicationGraph->AddClientConnection(Connection);
				}
			}

			GReplicationGraph->ServerReplicateActors(DeltaSeconds);
		}

		// TEMPORARY. Runs after the graph so a flag cleared this frame is picked up by the
		// next frame's pass rather than being written and reverted inside one replication.
		if (GResendProbeEnabled != 0)
		{
			++GTickFlushFrame;

			for (SDK::UNetConnection* ClientConnection : ClientConnections)
			{
				TickResendProbe(static_cast<UNetConnection*>(ClientConnection));
			}
		}
	}

	return TickFlush_Original(this, DeltaSeconds);
}

void UNetDriver::Initialize()
{
	TickFlush_Override.InstallAtRelativeAddress(0x13db208);

	/*
	 * The whole UReplicationDriver notification surface, not just AddNetworkActor. Each
	 * UE_DECLARE_NATIVE_OVERRIDE declares its own override object, so unlike the script
	 * function overrides there is no shared Original to collide over - but a failed install
	 * is still silent, so each one is checked.
	 */
	struct FDetour
	{
		const char* Name;
		bool bInstalled;
	};

	const FDetour Detours[] =
	{
		{ "AddNetworkActor",           AddNetworkActor_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_AddNetworkActor) },
		{ "RemoveNetworkActor",        RemoveNetworkActor_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_RemoveNetworkActor) },
		{ "ForceNetUpdate",            ForceNetUpdate_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_ForceNetUpdate) },
		{ "FlushActorDormancy",        FlushActorDormancy_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_FlushActorDormancy) },
		{ "NotifyActorDormancyChange", NotifyActorDormancyChange_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_NotifyActorDormancyChange) },
		{ "NotifyActorTearOff",        NotifyActorTearOff_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_NotifyActorTearOff) },
		{ "SetRoleSwapOnReplicate",    SetRoleSwapOnReplicate_Override.InstallAtRelativeAddress(EngineAddresses::UNetDriver_SetRoleSwapOnReplicate) },

		// The actor-side flush. Without this one none of the driver-side dormancy detours above
		// can fire at all, because the engine only dispatches them over ActiveNetDrivers and our
		// driver is not on that list. See AActor::FlushNetDormancy.
		{ "AActor::FlushNetDormancy",  AActor::FlushNetDormancy_Override.InstallAtRelativeAddress(EngineAddresses::AActor_FlushNetDormancy) },

		// Not a driver notification at all - the client's level-visibility report, which is the
		// only thing that can populate the graph's ClientVisibleLevelNames. Without it every
		// streaming-level actor is invisible to the gather; on a World Partition map that is
		// almost the entire world. See APlayerController::ServerUpdateLevelVisibility.
		{ "APlayerController::ServerUpdateLevelVisibility",
		  APlayerController::ServerUpdateLevelVisibility_Override.InstallAtRelativeAddress(EngineAddresses::APlayerController_ServerUpdateLevelVisibility) },
	};

	for (const FDetour& Detour : Detours)
	{
		if (!Detour.bInstalled)
		{
			UE_LOG(LogPrism, Warning, "UNetDriver::Initialize - failed to install the detour on %s. "
				"The replication graph will not be told about this event.", Detour.Name);
		}
	}
}

IMPLEMENT_MODULE(UNetDriver);

PRISM_NAMESPACE_END
