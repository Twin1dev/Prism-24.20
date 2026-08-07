#include "pch.h"

#include "Pawns/FortPlayerPawn.h"

PRISM_NAMESPACE_START

void AFortPlayerPawn::SetIsInsideSafeZone(bool bNewValue)
{
	bIsInsideSafeZone = bNewValue;
	OnRep_IsInsideSafeZone();
}

void AFortPlayerPawn::UpdateOutsideSafeZone()
{
	bIsInsideSafeZone = !bIsInAnyStorm;
	OnRep_IsInsideSafeZone();
}

void AFortPlayerPawn::Initialize()
{
	SetIsInsideSafeZone_Override.InstallAtVirtualIndex("FortPlayerPawnAthena", 0x250);
	UpdateOutsideSafeZone_Override.InstallAtVirtualIndex("FortPlayerPawnAthena", 0x251);
}

IMPLEMENT_MODULE(AFortPlayerPawn);

PRISM_NAMESPACE_END
