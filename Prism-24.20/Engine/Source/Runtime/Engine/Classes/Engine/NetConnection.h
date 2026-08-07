#pragma once

/*=============================================================================
	NetConnection.h: The slice of UNetConnection the Replication Graph talks to.
=============================================================================*/

#include "CoreGlobals.h"
#include "Containers/Array.h"
#include "Math/UnrealMathUtility.h"
#include "Net/EngineAddresses.h"

PRISM_NAMESPACE_START

// Declared before they are named below: an elaborated type specifier ("class UNetConnection*")
// would find the generated SDK class through the global using-directive rather than declaring
// ours, and the two are different types.
class AActor;
class UActorChannel;
class UChannel;
class UNetConnection;
class UNetDriver;

/** Mirrors EChannelCreateFlags. */
enum class EChannelCreateFlags : uint32
{
	None = 0x00,
	OpenedLocally = 0x02,
};

/** Mirrors ESetChannelActorFlags. */
enum class ESetChannelActorFlags : uint32
{
	None = 0x00,
	SkipReplicatorCreation = 0x01,
	SkipMarkActive = 0x02,
};

/** Mirrors EChannelCloseReason. */
enum class EChannelCloseReason : uint8
{
	Destroyed = 0,
	Dormancy = 1,
	LevelUnloaded = 2,
	Relevancy = 3,
	TearOff = 4,
};

/**
 * A viewer of the world for one connection.
 *
 * The engine builds these in UNetDriver::ServerReplicateActors from the
 * connection's PlayerController and its view target. The graph takes an array
 * of them per connection - one for the connection itself and one for each
 * split-screen child.
 */
struct FNetViewer
{
	FNetViewer() = default;
	FNetViewer(UNetConnection* InConnection, float DeltaSeconds);

	/** The connection this viewer belongs to. */
	UNetConnection* Connection = nullptr;

	/** The actor that "sees" - the PlayerController. */
	AActor* InViewer = nullptr;

	/** The actor being looked through - usually the pawn. */
	AActor* ViewTarget = nullptr;

	/** Where the view target is. */
	FVector ViewLocation = FVector(0.0, 0.0, 0.0);

	/** Which way it is facing, as a unit vector. */
	FVector ViewDir = FVector(0.0, 0.0, 0.0);
};

class UNetConnection : public UE_EXTENDS_SDK_CLASS(UNetConnection)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UNetConnection)

	UNetDriver* GetDriver() const { return reinterpret_cast<UNetDriver*>(Driver); }

	SDK::APlayerController* GetPlayerController() const { return PlayerController; }

	/**
	 * The actor this connection is looking through.
	 *
	 * Nothing in this binary maintains the field - see FNetViewer, which resolves the view
	 * target itself and writes it back here - so treat it as a cache of what the graph last
	 * decided rather than as engine state.
	 */
	AActor* GetViewTarget() const { return reinterpret_cast<AActor*>(ViewTarget); }
	void SetViewTarget(AActor* InViewTarget) { ViewTarget = reinterpret_cast<SDK::AActor*>(InViewTarget); }

	AActor* GetOwningActor() const { return reinterpret_cast<AActor*>(OwningActor); }

	int32 GetCurrentNetSpeed() const { return CurrentNetSpeed; }

	/*
	 * There is deliberately no IsNetReady() here.
	 *
	 * The engine answers it from QueuedBits and the pending send buffer, neither of which
	 * is reflected. Offsets for them were taken from the typed PDB build (0x200 and 0x610)
	 * and that was wrong: only *reflected* members are pinned across builds, and this build
	 * inserts non-reflected members the PDB one does not. UNetDriver proves it - the SDK,
	 * dumped from the shipping binary, puts World at 0x150 and NetDriverName at 0x1A8 where
	 * the PDB build has 0x140 and 0x190. Reading 0x200 as QueuedBits produced a value that
	 * was always positive, i.e. "never ready", and starved replication to one actor a frame.
	 *
	 * Until those two offsets are recovered from *this* binary, the graph budgets with a
	 * per-frame bit allowance instead. See UReplicationGraph::GetMaxBitsPerFrameForConnection.
	 */

	// -----------------------------------------------------------
	//	Engine entry points
	// -----------------------------------------------------------

	UChannel* CreateChannelByName(const FName& ChName, EChannelCreateFlags CreateFlags, int32 ChIndex)
	{
		static auto Function = GetEngineFunction<UChannel* (*)(UNetConnection*, const FName&, EChannelCreateFlags, int32)>(
			EngineAddresses::UNetConnection_CreateChannelByName);

		return Function(this, ChName, CreateFlags, ChIndex);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(UNetConnection);
UE_DECLARE_PRISM_TYPE(UNetConnection);

PRISM_NAMESPACE_END
