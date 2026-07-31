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
		GEngine->BroadcastNetworkFailure(this, NetDriver, ENetworkFailure::NetDriverAlreadyExists);
		return false;
	}

	FName NetDriverDefinition = NAME_GameNetDriver;

	if (GEngine->CreateNamedNetDriver(this, NAME_GameNetDriver, NetDriverDefinition))
	{
		NetDriver = GEngine->FindNamedNetDriver(this, NAME_GameNetDriver);
		NetDriver->SetWorld(this);

		for (FLevelCollection& LevelCollection : LevelCollections)
		{
			LevelCollection.NetDriver = NetDriver;
		}
	}

	if (NetDriver == nullptr)
	{
		GEngine->BroadcastNetworkFailure(this, NULL, ENetworkFailure::NetDriverCreateFailure);
		return false;
	}

	FString Error;
	if (!NetDriver->InitListen(this, InURL, false, Error))
	{
		GEngine->BroadcastNetworkFailure(this, NetDriver, ENetworkFailure::NetDriverListenFailure, Error);
		UE_LOG(LogWorld, Log, "Failed to listen: %s", Error.ToString().c_str());
		//GEngine->DestroyNamedNetDriver(this, NetDriver->NetDriverName); TODO
		NetDriver = nullptr;
		// TODO: Prooper levelcollections
		for (FLevelCollection& LevelCollection : LevelCollections)
		{
			LevelCollection.NetDriver = nullptr;
		}
		return false;
	}

	return true;
}

PRISM_NAMESPACE_END