#pragma once

#include "CoreGlobals.h"

PRISM_NAMESPACE_START

class UFortWorldItem : public UE_EXTENDS_SDK_CLASS(UFortWorldItem)
{
public:
	static UFortWorldItem* New(AFortInventory* Inventory, FFortItemEntry* ItemEntry, AActor* SpatialContextObj = nullptr)
	{
		static UFortWorldItem* (*Function)(AFortInventory*, FFortItemEntry*, AActor*) = decltype(Function)(InSDKUtils::GetImageBase() + 0x242E4D0);
		return Function(Inventory, ItemEntry, SpatialContextObj);
	}
};
UE_VERIFY_SDK_CLASS_EXTENSION(UFortWorldItem);
UE_DECLARE_PRISM_TYPE(UFortWorldItem);

PRISM_NAMESPACE_END