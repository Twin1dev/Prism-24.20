#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UWorld : public UE_EXTENDS_SDK_CLASS(UWorld)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UWorld);

	PRISM_PROPERTY_OVERRIDE(UNetDriver*, NetDriver);

	bool Listen(FURL& InURL);

	static void Initialize();
};

PRISM_NAMESPACE_END