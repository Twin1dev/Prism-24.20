#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UEngine : public UE_EXTENDS_SDK_CLASS(UEngine)
{
public:
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

	UNetDriver* FindNamedNetDriver(UWorld* World, FName NetDriverName)
	{
		static UNetDriver* (*Function)(UEngine*, UWorld*, FName) = decltype(Function)(InSDKUtils::GetImageBase() + 0xDAE410);
		return Function(this, World, NetDriverName);
	}
};

PRISM_NAMESPACE_END