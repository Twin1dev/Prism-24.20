#pragma once

#include "CoreGlobals.h"
#include "Net/EngineAddresses.h"

PRISM_NAMESPACE_START

class AFortPlayerPawn : public UE_EXTENDS_SDK_CLASS(AFortPlayerPawn)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(AFortPlayerPawn);

	void SetIsInsideSafeZone(bool bNewValue);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(SetIsInsideSafeZone);

	void UpdateOutsideSafeZone();
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(UpdateOutsideSafeZone);

	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(AFortPlayerPawn);
UE_DECLARE_PRISM_TYPE(AFortPlayerPawn);

PRISM_NAMESPACE_END