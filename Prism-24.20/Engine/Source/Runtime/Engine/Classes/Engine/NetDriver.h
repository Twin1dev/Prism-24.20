#pragma once

#include "CoreGlobals.h"
#include "Net/EngineAddresses.h"

PRISM_NAMESPACE_START

class AActor;
class UReplicationGraph;

class UNetDriver : public UE_EXTENDS_SDK_CLASS(UNetDriver)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UNetDriver)

	bool InitListen(void* Notify, FURL& ListenURL, bool bReuseAddressAndPort, FString& Error)
	{
		static bool (*Function)(UNetDriver*, void*, FURL&, bool, FString&) = decltype(Function)(InSDKUtils::GetImageBase() + 0x87e0a68);
		return Function(this, Notify, ListenURL, bReuseAddressAndPort, Error);
	}

	void SetWorld(UWorld* InWorld)
	{
		static void (*Function)(UNetDriver*, UWorld*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x16f650c);
		return Function(this, InWorld);
	}

	/**
	 * The graph that owns replication for this net driver.
	 *
	 * The engine keeps this in UNetDriver::ReplicationDriver (offset 0x710) and
	 * calls it through UReplicationDriver's virtuals. The graph rebuilt in this
	 * module is a plain C++ object rather than a UObject - see the note at the
	 * top of ReplicationGraphTypes.h for why - so it is held alongside the net
	 * driver and driven from TickFlush instead. Actors reach it through the
	 * detour on UNetDriver::AddNetworkActor.
	 */
	static UReplicationGraph* GetReplicationGraph();

	void TickFlush(float DeltaSeconds);
	UE_DECLARE_NATIVE_OVERRIDE(TickFlush);

	/*
	 * The UReplicationDriver notification surface.
	 *
	 * The engine does not only tell its replication driver about actors appearing. It reports
	 * every event that can change what an actor's replication should look like: the actor
	 * leaving, game code demanding an immediate send, dormancy being flushed or changed, tear
	 * off, role swapping. Each of those is a virtual on UReplicationDriver, and each has an
	 * engine-side wrapper that dispatches to it - all of them present in this binary, all of
	 * them running every match, all of them finding UNetDriver::ReplicationDriver null.
	 *
	 * The graph is not that pointer (see GetReplicationGraph below), so each wrapper is
	 * detoured here and forwarded by hand, at exactly the point the engine would have
	 * dispatched. Addresses and the slot each one was identified by are in EngineAddresses.h.
	 *
	 * Only AddNetworkActor was wired up before, which left the graph able to learn that an
	 * actor exists and nothing else. That is why dormant actors never woke and destroyed
	 * actors were never removed.
	 */

	/** Detoured so that actors entering the world are routed into the graph. */
	void AddNetworkActor(AActor* Actor);
	UE_DECLARE_NATIVE_OVERRIDE(AddNetworkActor);

	/** ...and so that actors leaving it are routed back out, before the actor is freed. */
	void RemoveNetworkActor(AActor* Actor);
	UE_DECLARE_NATIVE_OVERRIDE(RemoveNetworkActor);

	/** AActor::ForceNetUpdate lands here. The graph's answer is to pull the actor's next send forward. */
	void ForceNetUpdate(AActor* Actor);
	UE_DECLARE_NATIVE_OVERRIDE(ForceNetUpdate);

	/** AActor::FlushNetDormancy lands here: a dormant actor has something to say again. */
	void FlushActorDormancy(AActor* Actor, bool bWasDormInitial);
	UE_DECLARE_NATIVE_OVERRIDE(FlushActorDormancy);

	/** An actor's NetDormancy was assigned a different value. */
	void NotifyActorDormancyChange(AActor* Actor, ENetDormancy OldDormancyState);
	UE_DECLARE_NATIVE_OVERRIDE(NotifyActorDormancyChange);

	/** The actor wants one last replication and then to be disowned by the server. */
	void NotifyActorTearOff(AActor* Actor);
	UE_DECLARE_NATIVE_OVERRIDE(NotifyActorTearOff);

	/** The actor should be written with its Role and RemoteRole exchanged. */
	void SetRoleSwapOnReplicate(AActor* Actor, bool bSwapRoles);
	UE_DECLARE_NATIVE_OVERRIDE(SetRoleSwapOnReplicate);

	/*
	 * UNetDriver::ServerReplicateActors is virtual slot 110 (0x6E) - TickFlush's server
	 * branch is `call qword ptr [rax+880]` - and in this binary it is a stub that returns 0
	 * (WITH_SERVER_CODE, compiled out of the client). Nothing to suppress; see NetDriver.cpp.
	 */

	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(UNetDriver);
UE_DECLARE_PRISM_TYPE(UNetDriver);

PRISM_NAMESPACE_END