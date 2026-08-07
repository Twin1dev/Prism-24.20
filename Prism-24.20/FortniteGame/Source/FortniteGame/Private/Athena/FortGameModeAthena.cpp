#include "pch.h"

#include "Athena/FortGameModeAthena.h"
#include "Engine/World.h"
#include "Net/EngineAddresses.h"
#include <Templates/Casts.h>
#include "Items/FortInventory.h"

PRISM_NAMESPACE_START

/*-----------------------------------------------------------------------------
	Safe zone start-up.

	AFortGameModeAthena::SpawnInitialSafeZone lives at 0x0992BB18 in
	  C:\NewDumps\24.20\24.20\FortniteClient-Win64-Shipping_dump_SCY.exe.i64
	(++Fortnite+Release-24.20-CL-25156858), identified by the log string
	"FortGameModeAthena::SpawnInitialSafeZone bShouldSpawnSafeZoneIndicator == false...".

	The 24.20 body matches the 20.00 client PDB's AFortGameModeAthena::SpawnInitialSafeZone
	instruction for instruction, so the PDB supplies the names and types below. What 24.20
	does *not* contain is the three calls the 20.00 build still emits at the end -
	AFortGameStateAthena::SetSafeZoneIndicator, AFortSafeZoneIndicator::InitializeWithMapDefaultPhases
	and AFortSafeZoneIndicator::StartPhase. All three are empty in the client (their bodies
	are WITH_SERVER_CODE; in the 24.20 image even their exec thunks are ICF-folded onto other
	empty thunks), and 24.20's link-time codegen inlined the empty calls away entirely. They
	are gameplay the server build performs, so they are rebuilt here rather than called.

	The rebuilt phase set-up follows 12.41's AFortGameModeAthena::SpawnInitialSafeZone
	(0x018F6E90), which still had all of it inline before it was moved onto the indicator:
	every value is the map's FFortSafeZoneDefinition sampled at the phase index, phase 0's
	radius is clamped up to the map's authored starting radius, and a negative wait time
	means the phase is event driven rather than timed.
-----------------------------------------------------------------------------*/

namespace
{
	namespace FortSafeZoneAddresses
	{
		/** void AFortGameModeAthena::SpawnInitialSafeZone() - the function this module replaces. */
		inline constexpr uint64 AFortGameModeAthena_SpawnInitialSafeZone = 0x0992BB18;

		/**
		 * void AFortGameModeAthena::HandlePostSafeZonePhaseChanged()
		 * Named by the "AFortGameModeAthena::HandlePostSafeZonePhaseChanged" log scope inside it,
		 * and by the delegate SpawnInitialSafeZone binds to AFortSafeZoneIndicator::OnSafeZonePhaseChanged.
		 * Whole in the client, and returns immediately while CurrentPhase is still 0.
		 */
		inline constexpr uint64 AFortGameModeAthena_HandlePostSafeZonePhaseChanged = 0x0991BA94;

		/**
		 * void AFortGameModeAthena::SafeZoneInsideChecks()
		 * Named by its STAT_SafeZoneInsideCheck scope; whole in the client.
		 */
		inline constexpr uint64 AFortGameModeAthena_SafeZoneInsideChecks = 0x09927208;
	}

	/** FFortSafeZoneDefinition holds every per-phase value as a curve sampled at the phase index. */
	float GetSafeZoneValueAtPhase(const FScalableFloat& Value, int32 PhaseIndex)
	{
		if (Value.Curve.CurveTable)
		{
			EEvaluateCurveTableResult Result;
			FString ContextString;
			float OutValue = 0.0f;

			UDataTableFunctionLibrary::EvaluateCurveTableRow(
				Value.Curve.CurveTable,
				Value.Curve.RowName,
				static_cast<float>(PhaseIndex),
				&Result,
				&OutValue,
				ContextString);

			if (Result == EEvaluateCurveTableResult::RowFound)
			{
				return OutValue;
			}
		}

		return Value.Value;
	}

	/** AFortAthenaMapInfo::GetSafeZoneCount - the count is authored as a curve with no level input. */
	int32 GetSafeZoneCount(AFortAthenaMapInfo* MapInfo)
	{
		float Count = 0;

		if (!MapInfo->SafeZoneDefinition.Count.Curve.CurveTable)
			Count = static_cast<int32>(MapInfo->SafeZoneDefinition.Count.Value);
		else
		{
			EEvaluateCurveTableResult Result;
			UDataTableFunctionLibrary::EvaluateCurveTableRow(MapInfo->SafeZoneDefinition.Count.Curve.CurveTable, MapInfo->SafeZoneDefinition.Count.Curve.RowName, 0.0f, &Result, &Count, FString());
		}
		
		return static_cast<int32>(Count);
	}

	/** AFortSafeZoneIndicator::ConstructMapDefaultPhase, rebuilt from the map's safe zone definition. */
	FFortSafeZonePhaseInfo ConstructMapDefaultPhase(AFortGameModeAthena* GameMode, AFortAthenaMapInfo* MapInfo, int32 PhaseIndex)
	{
		const FFortSafeZoneDefinition& Definition = MapInfo->SafeZoneDefinition;

		FFortSafeZonePhaseInfo Phase{};
		Phase.Center = GameMode->SafeZoneLocations[PhaseIndex];
		Phase.Radius = GetSafeZoneValueAtPhase(Definition.Radius, PhaseIndex);
		Phase.WaitTime = GetSafeZoneValueAtPhase(Definition.WaitTime, PhaseIndex);
		Phase.ShrinkTime = GetSafeZoneValueAtPhase(Definition.ShrinkTime, PhaseIndex);
		Phase.MegaStormGridCellThickness = static_cast<int32>(GetSafeZoneValueAtPhase(Definition.MegaStormGridCellThickness, PhaseIndex) + 0.5f);
		Phase.TimeBetweenStormCapDamage = GetSafeZoneValueAtPhase(GameMode->TimeBetweenStormCapDamage, PhaseIndex);
		Phase.StormCapDamagePerTick = GetSafeZoneValueAtPhase(GameMode->StormCapDamagePerTick, PhaseIndex);
		Phase.StormCampingInitialDelayTime = GetSafeZoneValueAtPhase(GameMode->StormCampingInitialDelayTime, PhaseIndex);
		Phase.StormCampingIncrementTimeAfterDelay = GetSafeZoneValueAtPhase(GameMode->StormCampingIncrementTimeAfterDelay, PhaseIndex);

		// The playlist's squad size picks which of the three player cap curves applies.
		const UFortPlaylistAthena* Playlist = GameMode->GameState ? GameMode->GameState->CurrentPlaylistInfo.BasePlaylist : nullptr;
		const int32 SquadSize = Playlist ? Playlist->MaxSquadSize : 1;
		const FScalableFloat& PlayerCap = SquadSize <= 1 ? Definition.PlayerCapSolo
			: SquadSize == 2 ? Definition.PlayerCapDuo
			: Definition.PlayerCapSquad;
		Phase.PlayerCap = static_cast<int32>(GetSafeZoneValueAtPhase(PlayerCap, PhaseIndex));

		// FFortSafeZonePhaseInfo::DamageInfo is deliberately left at its default. Nothing in
		// FFortSafeZoneDefinition carries it, and 12.41 did not fill it here either - the damage
		// applied outside the circle comes from the storm damage pass, not from the phase.

		// 12.41 opened the first circle at whichever is larger, the phase radius or the map's
		// authored starting radius, so a playlist can start the match wider than its curve.
		if (PhaseIndex == 0)
		{
			const float StartingRadius = UFortScalableFloatUtils::GetValue(MapInfo->SafeZoneStartingRadius);
			if (StartingRadius > Phase.Radius)
				Phase.Radius = StartingRadius;
		}

		return Phase;
	}

	/** AFortSafeZoneIndicator::InitializeWithMapDefaultPhases. */
	void InitializeWithMapDefaultPhases(AFortGameModeAthena* GameMode, AFortSafeZoneIndicator* SafeZoneIndicator, AFortAthenaMapInfo* MapInfo)
	{
		const int32 PhaseCount = GameMode->SafeZoneLocations.Num();

		SafeZoneIndicator->SafeZonePhases.Empty();
		SafeZoneIndicator->SafeZonePhases.Reserve(PhaseCount);

		for (int32 PhaseIndex = 0; PhaseIndex < PhaseCount; ++PhaseIndex)
			SafeZoneIndicator->SafeZonePhases.Add(ConstructMapDefaultPhase(GameMode, MapInfo, PhaseIndex));

		SafeZoneIndicator->PhaseCount = PhaseCount;
	}

	/** AFortSafeZoneIndicator::StartPhase. */
	void StartPhase(AFortGameModeAthena* GameMode, AFortSafeZoneIndicator* SafeZoneIndicator, int32 InPhaseIndex)
	{
		if (!SafeZoneIndicator->SafeZonePhases.IsValidIndex(InPhaseIndex))
			return;

		float TimeSeconds = static_cast<float>(UGameplayStatics::GetTimeSeconds(GameMode));

		if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(InPhaseIndex - 1))
		{
			const FFortSafeZonePhaseInfo& PreviousPhase = SafeZoneIndicator->SafeZonePhases[InPhaseIndex - 1];

			static_cast<FVector&>(GameMode->SafeZoneIndicator->PreviousCenter) = PreviousPhase.Center;
			GameMode->SafeZoneIndicator->PreviousRadius = PreviousPhase.Radius;
		}

		const FFortSafeZonePhaseInfo& Phase = SafeZoneIndicator->SafeZonePhases[InPhaseIndex];

		static_cast<FVector&>(GameMode->SafeZoneIndicator->NextCenter) = Phase.Center;
		GameMode->SafeZoneIndicator->NextRadius = Phase.Radius;
		GameMode->SafeZoneIndicator->NextMegaStormGridCellThickness = Phase.MegaStormGridCellThickness;

		if (SafeZoneIndicator->SafeZonePhases.IsValidIndex(InPhaseIndex + 1))
		{
			const FFortSafeZonePhaseInfo& NextPhase = SafeZoneIndicator->SafeZonePhases[InPhaseIndex + 1];

			static_cast<FVector&>(GameMode->SafeZoneIndicator->FutureReplicator->NextNextCenter) = NextPhase.Center;
			GameMode->SafeZoneIndicator->FutureReplicator->NextNextRadius = NextPhase.Radius;

			static_cast<FVector&>(GameMode->SafeZoneIndicator->NextNextCenter) = NextPhase.Center;
			GameMode->SafeZoneIndicator->NextNextRadius = NextPhase.Radius;
			GameMode->SafeZoneIndicator->NextNextMegaStormGridCellThickness = NextPhase.MegaStormGridCellThickness;
		}

		GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime = TimeSeconds + Phase.WaitTime;
		GameMode->SafeZoneIndicator->SafeZoneFinishShrinkTime = GameMode->SafeZoneIndicator->SafeZoneStartShrinkTime + Phase.ShrinkTime;

		GameMode->SafeZoneIndicator->CurrentDamageInfo = Phase.DamageInfo;
		GameMode->SafeZoneIndicator->OnRep_CurrentDamageInfo();

		GameMode->SafeZoneIndicator->CurrentPhase = InPhaseIndex;
		GameMode->SafeZoneIndicator->OnRep_CurrentPhase();

		GameMode->SafeZoneIndicator->OnSafeZonePhaseChanged.Process();

		*(uint8_t*)(__int64(SafeZoneIndicator->FutureReplicator) - 0x4) = 2;
		SafeZoneIndicator->OnSafeZoneStateChange(EFortSafeZoneState::Holding, false);
		SafeZoneIndicator->SafezoneStateChangedDelegate.Process(SafeZoneIndicator, EFortSafeZoneState::Holding);
	}
}

bool AFortGameModeAthena::ReadyToStartMatch()
{
    if (!GameState || !GameState->MapInfo)
        return false;

    if (!bWorldIsReady)
    {
        WarmupRequiredPlayerCount = 1;

        UFortPlaylistAthena* Playlist = UObject::FindObject<UFortPlaylistAthena>("FortPlaylistAthena Playlist_DefaultSolo.Playlist_DefaultSolo");

        Playlist->GarbageCollectionFrequency = 9999999999999999.f;
        PlaylistHotfixOriginalGCFrequency = 9999999999999999.f;
        bDisableGCOnServerDuringMatch = true;
        bPlaylistHotfixChangedGCDisabling = true;

        GameState->CurrentPlaylistInfo.BasePlaylist = Playlist;
        GameState->CurrentPlaylistInfo.PlaylistReplicationKey++;
        GameState->CurrentPlaylistInfo.MarkArrayDirty();
        GameState->OnRep_CurrentPlaylistInfo();

        GameState->CurrentPlaylistId = Playlist->PlaylistId;
        GameState->OnRep_CurrentPlaylistId();

        CurrentPlaylistId = Playlist->PlaylistId;
        CurrentPlaylistName = Playlist->PlaylistName;

        GameState->AirCraftBehavior = Playlist->AirCraftBehavior;
        GameState->CachedSafeZoneStartUp = Playlist->SafeZoneStartUp;

        FURL Url{};
        Url.Port = 7777;
        if (!ToPrism(UWorld::GetWorld())->Listen(Url))
        {
            UE_LOG(LogPrism, Error, "Failed to listen on port 7777");
            return false;
        }

        GameSession->MaxPlayers = Playlist->MaxPlayers;

        bWorldIsReady = true;
    }

    if (!UWorld::GetWorld()->NetDriver)
        return false;

    return AlivePlayers.Num() > 0;
}

APawn* AFortGameModeAthena::SpawnDefaultPawnFor(AController* NewPlayer, AActor* StartSpot)
{
	auto SpawnDefaultPawnForFort = InSDKUtils::GetVirtualFunction<APawn* (*)(AFortGameMode*, AController*, AActor*)>(AFortGameMode::GetDefaultObj(), 0xE2);

	APawn* Pawn = SpawnDefaultPawnForFort(this, NewPlayer, StartSpot);

	if (!Pawn)
	{
		UE_LOG(LogFort, Error, "AFortGameModeAthena::SpawnDefaultPawnFor failed to spawn a pawn for player %s", GetNameSafe(NewPlayer).c_str());
		return nullptr;
	}

    if (Pawn)
    {
		AFortPlayerController* FortPC = Cast<AFortPlayerController>(NewPlayer);

        if (!FortPC || FortPC->bHasInitializedWorldInventory)
            return Pawn;

        AFortInventory* Inventory = ToPrism(FortPC->WorldInventory);

        for (const FItemAndCount& StartingItem : StartingItems)
        {
            if (!StartingItem.Item || StartingItem.Count <= 0 || StartingItem.Item->IsA<UFortSmartBuildingItemDefinition>())
                continue;

            Inventory->AddItem(StartingItem.Item, StartingItem.Count);
        }

        Inventory->AddItem(FortPC->CosmeticLoadoutPC.Pickaxe->WeaponDefinition, 1);

        FortPC->bHasInitializedWorldInventory = true;
    }

    return Pawn;
}

void AFortGameModeAthena::SpawnInitialSafeZone()
{
	AFortGameStateAthena* GameStateAthena = Cast<AFortGameStateAthena>(GameState);

	if (!GameStateAthena || !GameStateAthena->MapInfo)
	{
		UE_LOG(LogFort, Error, "FortGameModeAthena::SpawnInitialSafeZone has no game state or map info to build the safe zone from");
		return;
	}

	AFortAthenaMapInfo* MapInfo = GameStateAthena->MapInfo;

	bSafeZoneActive = true;

	// SendPreSafeZonePhaseChangedAnalytics and SendPostSafeZonePhaseChangedAnalytics run before this
	// in the shipped function; both are telemetry and are left out. HandlePostSafeZonePhaseChanged is
	// gameplay, so it is kept - the indicator does not exist yet, so it returns immediately, which is
	// what it does in the shipped build too.
	GetEngineFunction<void (*)(AFortGameModeAthena*)>(FortSafeZoneAddresses::AFortGameModeAthena_HandlePostSafeZonePhaseChanged)(this);

	// FortGameModeAthena_ShouldSpawnSafeZoneIndicator is polled over the mutator context here. No
	// mutator is loaded on this server, so the answer is the default the iterator starts from.
	const bool bShouldSpawnSafeZoneIndicator = true;

	if (!bShouldSpawnSafeZoneIndicator)
	{
		UE_LOG(LogFort, Log, "FortGameModeAthena::SpawnInitialSafeZone bShouldSpawnSafeZoneIndicator == false, skipping spawning Safe Zone Indicator");
		return;
	}

	// The shipped function only checks these; the locations are filled in earlier, during the
	// aircraft phase. Nothing drives that call yet, so seed them here if they are still empty.
	if (!bSafeZoneLocationsInitialized)
		InitializeSafeZoneLocations();

	const int32 ExpectedSafeZoneCount = GetSafeZoneCount(MapInfo);

	if (SafeZoneLocations.Num() <= 0 || SafeZoneLocations.Num() != ExpectedSafeZoneCount)
	{
		UE_LOG(LogFort, Error, "FortGameModeAthena::SpawnInitialSafeZone expected %d safe zone locations, got %d",
			ExpectedSafeZoneCount, SafeZoneLocations.Num());
		return;
	}

	if (!SafeZoneIndicator)
	{
		FTransform SpawnTransform{};
		SpawnTransform.Rotation.W = 1.0;
		SpawnTransform.Translation = SafeZoneLocations[0];
		SpawnTransform.Scale3D = FVector{ 1.0, 1.0, 1.0 };

		SafeZoneIndicator = Cast<AFortSafeZoneIndicator>(ToPrism(UWorld::GetWorld())->SpawnActor(
			SafeZoneIndicatorClass, SpawnTransform, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn));
	}

	if (!SafeZoneIndicator)
	{
		UE_LOG(LogFort, Error, "FortGameModeAthena::SpawnInitialSafeZone failed to spawn a Safe Zone Indicator of class %s",
			GetNameSafe(SafeZoneIndicatorClass).c_str());
		return;
	}

	// The shipped function binds HandlePostSafeZonePhaseChanged to the indicator's
	// OnSafeZonePhaseChanged here. Phases are advanced by UpdateSafeZonesPhase on this server, which
	// reaches the handler directly, so nothing is lost by leaving the dynamic binding off.

	// AFortGameStateAthena::SetSafeZoneIndicator.
	GameStateAthena->SafeZoneIndicator = SafeZoneIndicator;
	GameStateAthena->OnRep_SafeZoneIndicator();

	InitializeWithMapDefaultPhases(this, SafeZoneIndicator, MapInfo);
	StartPhase(this, SafeZoneIndicator, 0);

	GetEngineFunction<void (*)(AFortGameModeAthena*)>(FortSafeZoneAddresses::AFortGameModeAthena_SafeZoneInsideChecks)(this);

	OnSafeZoneIndicatorSpawned.Process(SafeZoneIndicator);

	/*if (OnSafeZoneIndicatorSpawned.InvocationList.Num() > 0)
	{
		struct { AFortSafeZoneIndicator* SpawnedSafeZoneIndicator; } Parms{ SafeZoneIndicator };

		for (const FScriptDelegate& Binding : OnSafeZoneIndicatorSpawned.InvocationList)
		{
			UObject* BoundObject = Binding.Object.Get();

			if (!BoundObject)
				continue;

			if (UFunction* Function = BoundObject->Class->GetFunction(Binding.FunctionName))
				BoundObject->ProcessEvent(Function, &Parms);
		}
	}*/

	UE_LOG(LogFort, Log, "Safe zone started: %d phases, first circle %.0f units at (%.0f, %.0f)",
		SafeZoneIndicator->PhaseCount, SafeZoneIndicator->Radius,
		SafeZoneIndicator->NextCenter.X, SafeZoneIndicator->NextCenter.Y);
}

void AFortGameModeAthena::UpdateSafeZonesPhase()
{
	if (bSafeZoneActive && UGameplayStatics::GetTimeSeconds(this) >= SafeZoneIndicator->SafeZoneFinishShrinkTime && !bSafeZonePaused && SafeZoneIndicator->SafeZonePhases.IsValidIndex(SafeZoneIndicator->CurrentPhase + 1))
	{
		StartPhase(this, SafeZoneIndicator, SafeZoneIndicator->CurrentPhase + 1);
	}

	return UpdateSafeZonesPhase_Original(this);
}

void AFortGameModeAthena::Initialize()
{
    // Slots read from the exec thunks: AGameMode::execReadyToStartMatch calls [rax+900h] and
    // AGameModeBase::execSpawnDefaultPawnFor calls [rax+710h].
    ReadyToStartMatch_Override.InstallAtVirtualIndex("FortGameModeBR", 0x120);
    SpawnDefaultPawnFor_Override.InstallAtVirtualIndex("FortGameModeBR", 0xE2);

    // Not a UFUNCTION, so it is detoured at its address rather than through a script slot.
    SpawnInitialSafeZone_Override.InstallAtRelativeAddress(FortSafeZoneAddresses::AFortGameModeAthena_SpawnInitialSafeZone);
	UpdateSafeZonesPhase_Override.InstallAtRelativeAddress(0x9930A74);
}

IMPLEMENT_MODULE(AFortGameModeAthena);

PRISM_NAMESPACE_END
