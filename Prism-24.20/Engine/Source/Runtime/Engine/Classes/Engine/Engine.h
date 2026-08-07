#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UNetDriver;

class UEngine : public UE_EXTENDS_SDK_CLASS(UEngine)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UEngine)

	/**
	 * Caps the frame rate the way a dedicated server would.
	 *
	 * Without this the client's uncapped rate applies, and after a long map load the engine
	 * hands every tick function a delta measured in seconds - the logs show DeltaTime: 24.08
	 * going into UNetDriver::TickDispatch. The reference server pins this at 30.
	 */
	float GetMaxTickRate(float DeltaTime, bool bAllowFrameRateSmoothing)
	{
		return 30.f;
	}
	UE_DECLARE_NATIVE_OVERRIDE(GetMaxTickRate);

	static void Initialize();

	void BroadcastNetworkFailure(UWorld* InWorld, Prism::UNetDriver* NetDriver, ENetworkFailure FailureType, const FString& ErrorString = TEXT(""))
	{
		static void (*Function)(UEngine*, UWorld*, Prism::UNetDriver*, ENetworkFailure, const FString&) = decltype(Function)(InSDKUtils::GetImageBase() + 0x81A1C74);
		Function(this, InWorld, NetDriver, FailureType, ErrorString);
	}

	bool CreateNamedNetDriver(UWorld* InWorld, FName NetDriverName, FName NetDriverDefinition)
	{
		static bool (*Function)(UEngine*, UWorld*, FName, FName) = decltype(Function)(InSDKUtils::GetImageBase() + 0x2429A2C);
		return Function(this, InWorld, NetDriverDefinition, NetDriverDefinition);
	}

	/**
	 * Creates a net driver without registering it in the world context.
	 *
	 * This is the difference that matters against CreateNamedNetDriver, which appends the driver to
	 * the context's ActiveNetDrivers. Anything on that list is owned by the engine's teardown: the
	 * world going away destroys it, on the engine's schedule rather than ours. That would be fine if
	 * the engine also owned every pointer to it, but the server writes the driver into each
	 * FLevelCollection by hand, and nothing clears those - so the collections keep pointing at a
	 * destroyed driver, and UWorld::SetCurrentLevelCollection reads it back out on the next
	 * UIpNetDriver::TickDispatch.
	 *
	 * Creating it unregistered leaves the driver ours for the life of the process, which is what a
	 * server wants and what the working reference server does.
	 *
	 * The driver comes back unnamed and unattached, so World and NetDriverName are set by the caller.
	 */
	UNetDriver* CreateNetDriver(UWorld* InWorld, FName NetDriverDefinition)
	{
		static void* (*GetWorldContext)(UEngine*, UWorld*) = decltype(GetWorldContext)(InSDKUtils::GetImageBase() + 0xDAE464);
		static UNetDriver* (*Function)(UEngine*, void*, FName, int) = decltype(Function)(InSDKUtils::GetImageBase() + 0x2429AF8);

		return Function(this, GetWorldContext(this, InWorld), NetDriverDefinition, 0);
	}

	UNetDriver* FindNamedNetDriver(UWorld* World, FName NetDriverName)
	{
		static UNetDriver* (*Function)(UEngine*, UWorld*, FName) = decltype(Function)(InSDKUtils::GetImageBase() + 0xDAE410);
		return Function(this, World, NetDriverName);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(UEngine);
UE_DECLARE_PRISM_TYPE(UEngine);

PRISM_NAMESPACE_END