#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class ABuildingSMActor : public UE_EXTENDS_SDK_CLASS(ABuildingSMActor)
{
public:
	UE_FUNCTION_OVERRIDE_BODY(ABuildingSMActor);

	void AttemptSpawnResources(const FBuildingSMActorAttemptSpawnResourcesParams& Params);
	UE_DECLARE_SCRIPT_FUNCTION_OVERRIDE(AttemptSpawnResources);

	static void Initialize();
};

PRISM_NAMESPACE_END