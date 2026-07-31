#include "pch.h"

#include "Engine/NetDriver.h"

PRISM_NAMESPACE_START

void UNetDriver::TickFlush(float DeltaSeconds)
{
	if (ClientConnections.Num() > 0 && ClientConnections[0]->InternalAck == false)
	{
		FModuleManager::Get().TickModules(DeltaSeconds);

		// ServerReplicateActors
	}

	return TickFlush_Original(this, DeltaSeconds);
}

void UNetDriver::Initialize()
{
	TickFlush_Override.InstallAtRelativeAddress(0x13db208);
}

PRISM_NAMESPACE_END

