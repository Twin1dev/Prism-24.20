#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UNetDriver;

enum ENetMode
{
	NM_Standalone,
	NM_DedicatedServer,
	NM_ListenServer,
	NM_Client,
	NM_MAX,
};

class UWorld : public UE_EXTENDS_SDK_CLASS(UWorld)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UWorld);

	/** Every world answers as a dedicated server, as the working reference server does. */
	ENetMode GetNetMode()
	{
		return NM_DedicatedServer;
	}
	UE_DECLARE_NATIVE_OVERRIDE(GetNetMode);

	bool Listen(FURL& InURL);

	AActor* SpawnActor(UClass* Class, const FTransform& Transform, AActor* Owner = nullptr, ESpawnActorCollisionHandlingMethod Collision = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn)
	{
		AActor* Actor = UGameplayStatics::BeginDeferredActorSpawnFromClass(this, Class, Transform, Collision, Owner);

		return UGameplayStatics::FinishSpawningActor(Actor, Transform);
	}

	template <typename T = AActor>
	T* SpawnActor(const FTransform& Transform, AActor* Owner = nullptr, ESpawnActorCollisionHandlingMethod Collision = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn)
	{
		return SpawnActor(T::StaticClass(), Transform, Owner, Collision);
	}

	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(UWorld);
UE_DECLARE_PRISM_TYPE(UWorld);

PRISM_NAMESPACE_END