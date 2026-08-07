#include "pch.h"

#include "Engine/World.h"
#include "Engine/Engine.h"
#include "Engine/NetDriver.h"

PRISM_NAMESPACE_START

#define NAME_GameNetDriver FName(L"GameNetDriver")

bool UWorld::Listen(FURL& InURL)
{
	static UEngine* GEngine = (Prism::UEngine*)UFortEngine::GetEngine(); // this is weird ik

	if (!GEngine)
	{
		return false;
	}

	if (NetDriver)
	{
		GEngine->BroadcastNetworkFailure(this, (Prism::UNetDriver*)NetDriver, ENetworkFailure::NetDriverAlreadyExists);
		return false;
	}

	/*
	* Unregistered on purpose - see UEngine::CreateNetDriver. A driver made with CreateNamedNetDriver
	* lands in the world context's ActiveNetDrivers and is destroyed when the world goes away, while
	* the FLevelCollections written below still point at it. That is the use-after-free that
	* UIpNetDriver::TickDispatch trips over when it restores the level collection on scope exit.
	*/
	Prism::UNetDriver* NewNetDriver = GEngine->CreateNetDriver(this, NAME_GameNetDriver);

	if (NewNetDriver == nullptr)
	{
		GEngine->BroadcastNetworkFailure(this, nullptr, ENetworkFailure::NetDriverCreateFailure);
		return false;
	}

	/* CreateNetDriver does neither of these, because it is the named wrapper that normally would. */
	NewNetDriver->World = this;
	NewNetDriver->NetDriverName = NAME_GameNetDriver;

	/*
	* bReuseAddressAndPort is true so that binding 7777 succeeds while the socket from the previous
	* world is still in TIME_WAIT. The engine's own Listen during LoadMap has already failed by this
	* point ("LoadMap: failed to Listen"), so the port is not necessarily clear yet.
	*/
	FString Error;
	if (!NewNetDriver->InitListen(this, InURL, true, Error))
	{
		GEngine->BroadcastNetworkFailure(this, NewNetDriver, ENetworkFailure::NetDriverListenFailure, Error);
		UE_LOG(LogWorld, Log, "Failed to listen: %s", Error.ToString().c_str());
		return false;
	}

	NewNetDriver->SetWorld(this);

	/*
	* Published only now that the driver is listening. Nothing above this point has stored it
	* anywhere, so a failed InitListen leaves no pointer behind to dangle.
	*/
	for (FLevelCollection& LevelCollection : LevelCollections)
	{
		LevelCollection.NetDriver = NewNetDriver;
	}

	NetDriver = NewNetDriver;

	SetConsoleTitleA("Prism | Listening on Port 7777");
	return true;
}

void UWorld::Initialize()
{
	GetNetMode_Override.InstallAtRelativeAddress(0x1176EF4);
}

IMPLEMENT_MODULE(UWorld);

PRISM_NAMESPACE_END