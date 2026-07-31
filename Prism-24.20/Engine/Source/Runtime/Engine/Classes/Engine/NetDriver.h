#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

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

	void TickFlush(float DeltaSeconds);
	UE_DECLARE_NATIVE_OVERRIDE(TickFlush);

	static void Initialize();
};

PRISM_NAMESPACE_END