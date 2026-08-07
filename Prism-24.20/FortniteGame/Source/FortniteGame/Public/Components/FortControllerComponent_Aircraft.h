#pragma once

#include "CoreGlobals.h"
#include <Templates/Casts.h>

PRISM_NAMESPACE_START

class UFortControllerComponent_Aircraft : public UE_EXTENDS_SDK_CLASS(UFortControllerComponent_Aircraft)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(UFortControllerComponent_Aircraft);

	AFortPlayerControllerAthena* GetOwningPlayerController()
	{
		return Cast<AFortPlayerControllerAthena>(GetOwner());
	}

	void ServerAttemptAircraftJump(const FRotator& ClientRotation);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(ServerAttemptAircraftJump);

	static void Initialize();
};
UE_VERIFY_SDK_CLASS_EXTENSION(UFortControllerComponent_Aircraft);
UE_DECLARE_PRISM_TYPE(UFortControllerComponent_Aircraft);

PRISM_NAMESPACE_END