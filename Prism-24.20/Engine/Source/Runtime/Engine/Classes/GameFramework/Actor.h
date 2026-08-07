#pragma once

/*=============================================================================
	Actor.h: The slice of AActor the Replication Graph talks to.

	Everything reflected comes straight from the generated class. What is added
	here is what AActor exposes as inline accessors in the engine (and therefore
	has no address of its own), plus the one call - CallPreReplication - that is
	a real function in the image.
=============================================================================*/

#include "CoreGlobals.h"
#include "Containers/Array.h"
#include "Math/UnrealMathUtility.h"
#include "Net/EngineAddresses.h"

PRISM_NAMESPACE_START

class UNetDriver;

class AActor : public UE_EXTENDS_SDK_CLASS(AActor)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(AActor)

	// -----------------------------------------------------------
	//	Replication state
	// -----------------------------------------------------------

	/**
	 * Detoured so a dormancy flush can reach this module's net driver.
	 *
	 * The engine's implementation walks FWorldContext::ActiveNetDrivers, and this module's
	 * driver is deliberately not on that list, so UNetDriver::FlushActorDormancy - and with
	 * it UReplicationGraph::FlushNetDormancy - was never called for any actor. Implemented in
	 * NetDriver.cpp alongside the rest of the dormancy wiring. See EngineAddresses.
	 */
	void FlushNetDormancy();
	UE_DECLARE_NATIVE_OVERRIDE(FlushNetDormancy);

	bool GetIsReplicated() const { return bReplicates != 0; }
	bool GetTearOff() const { return bTearOff != 0; }
	bool IsActorBeingDestroyed() const { return bActorIsBeingDestroyed != 0; }

	float GetNetCullDistanceSquared() const { return NetCullDistanceSquared; }
	float GetNetUpdateFrequency() const { return NetUpdateFrequency; }
	float GetMinNetUpdateFrequency() const { return MinNetUpdateFrequency; }
	float GetNetPriority() const { return NetPriority; }

	ENetRole GetLocalRole() const { return Role; }
	ENetRole GetRemoteRole() const { return RemoteRole; }

	AActor* GetOwner() const { return static_cast<AActor*>(Owner); }

	/**
	 * The engine answers this from AActor::bNetStartup, which is not a UPROPERTY
	 * and so has no offset in the generated class.
	 *
	 * ASSUMPTION: an actor placed in a level is loaded with the package rather
	 * than spawned, so RF_WasLoaded stands in for it. This is only consulted to
	 * decide whether a DORM_Initial actor should be gathered at all, where the
	 * two definitions agree for every actor that comes out of a level.
	 */
	bool IsNetStartupActor() const { return (static_cast<uint32>(Flags) & static_cast<uint32>(EObjectFlags::WasLoaded)) != 0; }

	/** Mirrors AActor::GetActorLocation, which reads the root component's world transform. */
	FVector GetActorLocation() const
	{
		if (const uint8* Transform = GetComponentToWorld())
		{
			const double* Translation = reinterpret_cast<const double*>(Transform + EngineAddresses::FTransform_Translation);
			return FVector(Translation[0], Translation[1], Translation[2]);
		}

		return FVector(0.0, 0.0, 0.0);
	}

	/**
	 * Mirrors AActor::GetActorRotation.
	 *
	 * The transform stores the rotation as a quaternion - under LWC it is the first member,
	 * an FQuat4d - and the engine's accessor is that quaternion converted to a rotator.
	 */
	FRotator GetActorRotation() const
	{
		if (const uint8* Transform = GetComponentToWorld())
		{
			return reinterpret_cast<const FQuat*>(Transform + EngineAddresses::FTransform_Rotation)->Rotator();
		}

		return FRotator(0.0, 0.0, 0.0);
	}

private:

	/** The root component's world transform, or null when the actor has no root component. */
	const uint8* GetComponentToWorld() const
	{
		const USceneComponent* Root = RootComponent;
		return Root ? reinterpret_cast<const uint8*>(Root) + EngineAddresses::USceneComponent_ComponentToWorld : nullptr;
	}

public:

	/** The level this actor lives in. Actors are always outered to their level. */
	ULevel* GetLevel() const
	{
		for (UObject* Test = Outer; Test != nullptr; Test = Test->Outer)
		{
			if (Test->IsA(ULevel::StaticClass()))
			{
				return static_cast<ULevel*>(Test);
			}
		}

		return nullptr;
	}

	// -----------------------------------------------------------
	//	Engine entry points
	// -----------------------------------------------------------

	/** Gives the actor its chance to update replicated properties before it is considered for replication. */
	void CallPreReplication(UNetDriver* NetDriver)
	{
		static auto Function = GetEngineFunction<void (*)(AActor*, UNetDriver*)>(EngineAddresses::AActor_CallPreReplication);
		Function(this, NetDriver);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(AActor);
UE_DECLARE_PRISM_TYPE(AActor);

/**
 * Any generated actor seen as the Prism AActor extension.
 *
 * Every generated actor class is single inheritance from UObject and the extension adds no data
 * members, so this is the same address described by a type that carries the accessors AActor
 * adds. Spelled out because the alternative - a reinterpret_cast at each of the several dozen
 * places the graph moves an SDK actor pointer into a rep list - says nothing about intent.
 */
inline AActor* ToActor(SDK::AActor* Actor) { return reinterpret_cast<AActor*>(Actor); }
inline const AActor* ToActor(const SDK::AActor* Actor) { return reinterpret_cast<const AActor*>(Actor); }

PRISM_NAMESPACE_END
