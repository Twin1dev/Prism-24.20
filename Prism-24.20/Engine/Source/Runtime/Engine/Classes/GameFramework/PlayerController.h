#pragma once

/*=============================================================================
	PlayerController.h: The view-point chain, rebuilt from reflected state.

	The Replication Graph asks one question of every connection each frame -
	"where is this player, and which way are they facing" - and the engine
	answers it through APlayerController::GetPlayerViewPoint. That answer feeds
	distance culling, spatialization cell selection and the dynamic spatial
	frequency zones, so it has to be both correct and cheap.

	The chain is APlayerController::GetPlayerViewPoint ->
	AController::GetActorEyesViewPoint -> APawn::GetActorEyesViewPoint, and it
	is reproduced here rather than called into for two reasons:

	  * Every step of it is reachable through reflected members - ControlRotation,
	    Pawn, BaseEyeHeight, PlayerCameraManager and the spectator sync fields are
	    all UPROPERTYs - so there is nothing to recover and nothing to guess.

	  * The generated SDK reaches these functions through ProcessEvent, which
	    means a trip through the script VM per call. Called once per connection
	    per frame from the graph, and again by game code, that is real cost for
	    a handful of member reads.

	The bodies below are UE 5.1's, with the branches that only a client can take
	written out but never taken on a dedicated server, so the code still reads
	like the engine's.

	One spelling note: every Prism type is named with its namespace inside these
	class bodies. The generated bases carry their own injected-class-names, so an
	unqualified AActor inside a class deriving from SDK::AActor finds SDK::AActor,
	not ours.
=============================================================================*/

#include "CoreGlobals.h"
#include "GameFramework/Actor.h"
#include "Math/UnrealMathUtility.h"

PRISM_NAMESPACE_START

class AController;

/*-----------------------------------------------------------------------------
	APawn
-----------------------------------------------------------------------------*/

class APawn : public UE_EXTENDS_SDK_CLASS(APawn)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(APawn)

	/** This pawn seen as the Prism AActor extension, for the accessors AActor adds. */
	PRISM_NAMESPACE_NAME::AActor* AsActor() { return reinterpret_cast<PRISM_NAMESPACE_NAME::AActor*>(this); }
	const PRISM_NAMESPACE_NAME::AActor* AsActor() const { return reinterpret_cast<const PRISM_NAMESPACE_NAME::AActor*>(this); }

	PRISM_NAMESPACE_NAME::AController* GetController() const { return reinterpret_cast<PRISM_NAMESPACE_NAME::AController*>(Controller); }

	/** Eye position: the pawn's location raised by its eye height. Mirrors APawn::GetPawnViewLocation. */
	FVector GetPawnViewLocation() const
	{
		const FVector Location = AsActor()->GetActorLocation();
		return FVector(Location.X, Location.Y, Location.Z + static_cast<double>(BaseEyeHeight));
	}

	/**
	 * Where the pawn is looking.
	 *
	 * On the server this is the controller's control rotation, which is the rotation the
	 * client sends up - and the only one carrying pitch, since a character's actor rotation
	 * is yaw only.
	 */
	FRotator GetViewRotation() const;

	/** APawn::GetActorEyesViewPoint. */
	void GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const
	{
		OutLocation = GetPawnViewLocation();
		OutRotation = GetViewRotation();
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(APawn);
UE_DECLARE_PRISM_TYPE(APawn);

/*-----------------------------------------------------------------------------
	AController
-----------------------------------------------------------------------------*/

class AController : public UE_EXTENDS_SDK_CLASS(AController)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(AController)

	PRISM_NAMESPACE_NAME::AActor* AsActor() { return reinterpret_cast<PRISM_NAMESPACE_NAME::AActor*>(this); }
	const PRISM_NAMESPACE_NAME::AActor* AsActor() const { return reinterpret_cast<const PRISM_NAMESPACE_NAME::AActor*>(this); }

	PRISM_NAMESPACE_NAME::APawn* GetPawn() const { return reinterpret_cast<PRISM_NAMESPACE_NAME::APawn*>(Pawn); }

	/**
	 * AController::GetControlRotation.
	 *
	 * Read straight out of the reflected member rather than through the UFunction: the
	 * engine's accessor is a plain return of exactly this field.
	 */
	const FRotator& GetControlRotation() const { return ControlRotation; }

	bool IsInState(const FName& InStateName) const { return StateName == InStateName; }

	/**
	 * AController::GetActorEyesViewPoint.
	 *
	 * A controller has no physical location of its own, so with no pawn the engine leaves
	 * the outputs alone. That is a trap for callers who pass uninitialized values, so the
	 * controller's own transform is written instead - the same answer AActor would give.
	 */
	void GetActorEyesViewPoint(FVector& OutLocation, FRotator& OutRotation) const
	{
		if (const PRISM_NAMESPACE_NAME::APawn* ControlledPawn = GetPawn())
		{
			ControlledPawn->GetActorEyesViewPoint(OutLocation, OutRotation);
			return;
		}

		OutLocation = AsActor()->GetActorLocation();
		OutRotation = ControlRotation;
	}

	/** AController::GetPlayerViewPoint. */
	void GetPlayerViewPoint(FVector& OutLocation, FRotator& OutRotation) const
	{
		GetActorEyesViewPoint(OutLocation, OutRotation);
	}

	/** AController::GetViewTarget - the pawn, or the controller itself when unpossessed. */
	PRISM_NAMESPACE_NAME::AActor* GetViewTarget() const
	{
		if (PRISM_NAMESPACE_NAME::APawn* ControlledPawn = GetPawn())
		{
			return ControlledPawn->AsActor();
		}

		return const_cast<AController*>(this)->AsActor();
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(AController);
UE_DECLARE_PRISM_TYPE(AController);

inline FRotator APawn::GetViewRotation() const
{
	if (const PRISM_NAMESPACE_NAME::AController* PawnController = GetController())
	{
		return PawnController->GetControlRotation();
	}

	return AsActor()->GetActorRotation();
}

/*-----------------------------------------------------------------------------
	APlayerController
-----------------------------------------------------------------------------*/

class APlayerController : public UE_EXTENDS_SDK_CLASS(APlayerController)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(APlayerController)

	PRISM_NAMESPACE_NAME::AController* AsController() { return reinterpret_cast<PRISM_NAMESPACE_NAME::AController*>(this); }
	const PRISM_NAMESPACE_NAME::AController* AsController() const { return reinterpret_cast<const PRISM_NAMESPACE_NAME::AController*>(this); }

	PRISM_NAMESPACE_NAME::AActor* AsActor() { return reinterpret_cast<PRISM_NAMESPACE_NAME::AActor*>(this); }
	const PRISM_NAMESPACE_NAME::AActor* AsActor() const { return reinterpret_cast<const PRISM_NAMESPACE_NAME::AActor*>(this); }

	bool IsLocalController() const { return bIsLocalPlayerController; }

	/**
	 * The connection this controller speaks to, or null for a controller that has no client.
	 *
	 * NetConnection is set for a child (split-screen) connection; for an ordinary remote
	 * player it is Player, which UNetConnection derives from. Both are checked because the
	 * engine's own GetNetConnection does the same, and a controller reached from a child
	 * connection would otherwise resolve to the parent's manager.
	 */
	SDK::UNetConnection* GetNetConnection() const
	{
		if (NetConnection != nullptr)
		{
			return NetConnection;
		}

		if (Player != nullptr && Player->IsA(SDK::UNetConnection::StaticClass()))
		{
			return static_cast<SDK::UNetConnection*>(Player);
		}

		return nullptr;
	}

	/**
	 * The client reporting that a streaming level has finished loading, or been unloaded.
	 *
	 * Detoured purely to forward to the Replication Graph, which keeps its own copy of the
	 * set in UNetReplicationGraphConnection::ClientVisibleLevelNames and gates every
	 * streaming-level actor list on it. Implemented in NetDriver.cpp with the rest of the
	 * graph notification wiring.
	 */
	void ServerUpdateLevelVisibility(const SDK::FUpdateLevelVisibilityLevelInfo& LevelVisibility);
	UE_DECLARE_NATIVE_OVERRIDE(ServerUpdateLevelVisibility);

	/** True while the controller is in the spectating state, whose view the client syncs up to us. */
	bool IsSpectating() const
	{
		static const FName NAME_Spectating(L"Spectating");
		return AsController()->IsInState(NAME_Spectating);
	}

	/**
	 * APlayerController::GetViewTarget.
	 *
	 * The camera manager owns the view target; without one - or before it has been given a
	 * target - the controller is looking through itself.
	 */
	PRISM_NAMESPACE_NAME::AActor* GetViewTarget() const
	{
		if (const SDK::APlayerCameraManager* CameraManager = PlayerCameraManager)
		{
			if (SDK::AActor* Target = CameraManager->ViewTarget.Target)
			{
				return reinterpret_cast<PRISM_NAMESPACE_NAME::AActor*>(Target);
			}
		}

		return AsController()->GetViewTarget();
	}

	/**
	 * APlayerController::GetPlayerViewPoint.
	 *
	 * Three cases, in the engine's order:
	 *
	 *   1. A spectator on the server. We are not simulating that player's camera - the client
	 *      is, and syncs it up - so the synced transform is the only description of it there is.
	 *
	 *   2. A camera manager that has run at least once - but ONLY for a local controller.
	 *
	 *      The engine's own version has no such condition, and reproducing it faithfully was a
	 *      bug. UpdateCameraManager is called from APlayerController::PlayerTick, which only
	 *      runs for local controllers, so on a dedicated server a remote player's camera manager
	 *      is never ticked. Its cache is not empty either: possession runs SetViewTarget, which
	 *      stamps the cache once, so Timestamp becomes non-zero while POV stays at whatever it
	 *      was initialised to - near the origin.
	 *
	 *      The effect was that a connection's view point was correct right up until its player
	 *      possessed a pawn, and from then on read as roughly (0,0,0). Every FNetViewer in the
	 *      Replication Graph inherits that, so every connection believed it was standing at the
	 *      world origin: it gathered the grid cell containing the origin instead of the one it
	 *      was in, and distance-culled every actor by its true distance from the origin -
	 *      including its own pawn, hundreds of metres away. No player ever saw another player.
	 *
	 *      Gating on IsLocalController keeps the branch correct for a listen server, where the
	 *      camera does tick, and makes it dead on a dedicated server. This is what the working
	 *      24.20 reference server does too: its GetPlayerViewPoint has no camera branch at all.
	 *
	 *   3. Otherwise the view target's transform, falling back to the controller's own eyes -
	 *      which is where a controller with no pawn and no camera ends up.
	 */
	void GetPlayerViewPoint(FVector& OutLocation, FRotator& OutRotation) const
	{
		if (IsSpectating() && !IsLocalController())
		{
			OutLocation = LastSpectatorSyncLocation;
			OutRotation = LastSpectatorSyncRotation;
			return;
		}

		if (IsLocalController())
		{
			if (const SDK::APlayerCameraManager* CameraManager = PlayerCameraManager)
			{
				// Timestamp stays zero until the camera has actually been updated at least once.
				const FCameraCacheEntry& CameraCache = CameraManager->CameraCachePrivate;
				if (CameraCache.Timestamp > 0.f)
				{
					OutLocation = CameraCache.POV.Location;
					OutRotation = CameraCache.POV.Rotation;
					return;
				}
			}
		}

		PRISM_NAMESPACE_NAME::AActor* TheViewTarget = GetViewTarget();
		if (TheViewTarget != nullptr && TheViewTarget != AsActor())
		{
			OutLocation = TheViewTarget->GetActorLocation();
			OutRotation = TheViewTarget->GetActorRotation();
			return;
		}

		AsController()->GetPlayerViewPoint(OutLocation, OutRotation);
	}

	/**
	 * Flushes any pending movement correction to the client.
	 *
	 * The server half of the character movement protocol: UCharacterMovementComponent::ServerMove
	 * decides that the client is right (ClientAckGoodMove) or wrong (ClientAdjustPosition) and
	 * parks the answer on FNetworkPredictionData_Server_Character::PendingAdjustment, but nothing
	 * is sent until this runs. Both of the engine's callers are gone here - the legacy
	 * UNetDriver::ServerReplicateActors is a WITH_SERVER_CODE stub, and the graph that would
	 * otherwise call it is the one being rebuilt - so the graph calls it, once per connection per
	 * frame, ahead of everything else and never suppressed by bandwidth.
	 *
	 * Called rather than reimplemented: it is present in this binary and does real work that
	 * would be miserable to reproduce (client frame acks, spectator handling, and the hop through
	 * the pawn's UNetworkPredictionInterface).
	 */
	void SendClientAdjustment()
	{
		static auto Function = GetEngineFunction<void (*)(APlayerController*)>(EngineAddresses::APlayerController_SendClientAdjustment);
		Function(this);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(APlayerController);
UE_DECLARE_PRISM_TYPE(APlayerController);

PRISM_NAMESPACE_END
