#include "pch.h"

#include "Engine/Engine.h"

PRISM_NAMESPACE_START

void UEngine::Initialize()
{
	GetMaxTickRate_Override.InstallAtRelativeAddress(0x1005580);
}

IMPLEMENT_MODULE(UEngine);

PRISM_NAMESPACE_END
