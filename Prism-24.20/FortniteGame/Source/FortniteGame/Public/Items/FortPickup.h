#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

struct FortPickupCreationData
{
public:
	UWorld* World;
	FFortItemEntry PickupDataItemEntry;
	const FVector Position;
	const FRotator Rotation;
	TWeakObjectPtr<AFortPlayerController> OptionalPCOwner;
	TSubclassOf<AFortPickup> OverrideClass;
	TWeakObjectPtr<AActor> OwnerContainer;
	TWeakObjectPtr<AActor const> OwnerInstigator;
	EFortPickupSourceTypeFlag SourceTypeFlags;
	EFortPickupSpawnSource SpawnSource;
	uint8 bRandomRotation : 1;
	uint8 bPickupOnlyRelevantToOwner : 1;

	FortPickupCreationData() = default;

	FortPickupCreationData(UWorld* InWorld, const FFortItemEntry& InItemEntry, const FVector& InPosition,
		EFortPickupSourceTypeFlag InSourceTypeFlags = EFortPickupSourceTypeFlag::Other,
		EFortPickupSpawnSource InSpawnSource = EFortPickupSpawnSource::Unset,
		bool bInRandomRotation = false,
		AFortPlayerController* InOptionalPCOwner = nullptr,
		AActor* InOwnerContainer = nullptr)
		: World(InWorld)
		, Position(InPosition)
		, OptionalPCOwner(InOptionalPCOwner)
		, OwnerContainer(InOwnerContainer)
		, SourceTypeFlags(InSourceTypeFlags)
		, SpawnSource(InSpawnSource)
		, bRandomRotation(bInRandomRotation)
	{
		PickupDataItemEntry = InItemEntry;
	}
};

class AFortPickup : public UE_EXTENDS_SDK_CLASS(AFortPickup)
{
public:
	static AFortPickup* CreateFromData(FortPickupCreationData& Data)
	{
		static AFortPickup* (*Function)(FortPickupCreationData&) = decltype(Function)(InSDKUtils::GetImageBase() + 0xA27F1E4);
		return Function(Data);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortPickup);
UE_DECLARE_PRISM_TYPE(AFortPickup);

PRISM_NAMESPACE_END