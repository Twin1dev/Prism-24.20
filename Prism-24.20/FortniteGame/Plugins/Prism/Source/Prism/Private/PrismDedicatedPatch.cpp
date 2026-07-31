#include "pch.h"

#include "../Public/PrismDedicatedPatch.h"

void FPrismDedicatedPatch::Initialize()
{
	FImageMemory::Patch<bool>(0x10512933, false); // GIsClient
	FImageMemory::Patch<bool>(0x1075FF2C, true); // GIsServer

	// Random bullshit
	FImageMemory::Null(0x31BEF70);
	FImageMemory::Null(0x1F2CD64);
	FImageMemory::Null(0x99AF8CC);
	FImageMemory::Null(0xB830A74);
	FImageMemory::Null(0x21EE7FC);
}

IMPLEMENT_MODULE(FPrismDedicatedPatch);
