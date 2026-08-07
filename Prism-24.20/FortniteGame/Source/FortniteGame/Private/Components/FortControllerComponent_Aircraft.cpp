#include "pch.h"

#include "Components/FortControllerComponent_Aircraft.h"

PRISM_NAMESPACE_START

void UFortControllerComponent_Aircraft::ServerAttemptAircraftJump(const FRotator& ClientRotation)
{
	AFortPlayerControllerAthena* PlayerController = GetOwningPlayerController();

	if (!PlayerController)
		return;

	/*
	 * The client sends this from the bus UI, and it will send it again if it does not see itself
	 * leave - which is exactly what happens while the pawn is still on its way to the client.
	 * Without this guard the second send restarts the player a second time: the pawn that was
	 * just spawned and possessed is destroyed and replaced, so the ClientRestart the client was
	 * waiting on now names an actor that no longer exists, and the client is left looking through
	 * whatever it was looking through before. Fortnite's own guard, and the one the working
	 * reference server uses.
	 */
	if (!PlayerController->IsInAircraft())
		return;

	/*
	 * RestartPlayer, not RestartPlayerAtPlayerStart.
	 *
	 * AFortGameModeAthena overrides RestartPlayer, and that override is what knows about leaving
	 * the bus: it derives the spawn transform from the aircraft's flight path, puts the pawn into
	 * its skydiving state, and clears the controller's in-aircraft state. Going straight to
	 * RestartPlayerAtPlayerStart with the aircraft as the start spot skips all of it and only
	 * happens to look right, because AGameModeBase spawns the pawn at the start spot's transform.
	 *
	 * Reached through the SDK's UFunction wrapper, which dispatches virtually, so this calls
	 * Fortnite's own implementation rather than reproducing it.
	 */
	UWorld::GetWorld()->AuthorityGameMode->RestartPlayer(PlayerController);

	// bResetCamera: the controller's camera has to be pulled off the aircraft and put on the new
	// pawn. Passing false leaves it where it was.
	PlayerController->ClientSetRotation(ClientRotation, true);
}

void UFortControllerComponent_Aircraft::Initialize()
{
	ServerAttemptAircraftJump_Override.InstallAtVirtualIndex("FortControllerComponent_Aircraft", 0xA5);
}

IMPLEMENT_MODULE(UFortControllerComponent_Aircraft);

PRISM_NAMESPACE_END

