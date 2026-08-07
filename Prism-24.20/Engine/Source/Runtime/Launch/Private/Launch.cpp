#include "pch.h"

DWORD WINAPI LaunchWindowsStartup(LPVOID)
{
    Sleep(5000);

    AllocConsole();
    FILE* fptr;
    freopen_s(&fptr, "CONOUT$", "w+", stdout);
    SetConsoleTitleA("Prism | Starting");

    FModuleManager::Get().StartupModules();

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), TEXT("log LogFortUIDirector off"), nullptr);
    //UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), TEXT("log LogNet off"), nullptr);

    /*
    * Without this, nothing replicates after its first send.
    *
    * FReplicationChangelistMgr::Update skips the property comparison whenever
    * LastReplicationFrame == NetDriver->ReplicationFrame - that is what net.ShareShadowState
    * buys: compare an actor once per frame, share the result across every connection. The
    * counter is incremented in two places, and this process has neither of them:
    *
    *   - UNetDriver::ServerReplicateActors, a WITH_SERVER_CODE stub in this binary that just
    *     returns 0 (see the note in NetDriver.cpp), and
    *   - UReplicationGraph::ServerReplicateActors, whose first act in the shipped graph is
    *     `++NetDriver->ReplicationFrame` (NetDriver + 0x420) - the reimplementation in this
    *     module never did it.
    *
    * So every actor is compared exactly once, on its first replication, and looks clean for
    * the rest of the match. What that looked like: a forced flip of the replicated
    * bHasServerFinishedLoading writing 0 bits, and ~1000 gathered actors writing nothing at
    * all, frame after frame, while the client sat on the loading screen waiting for that bool.
    *
    * Turning sharing off forces the comparison every time. It costs CPU that does not matter
    * at this player count. The correct fix is to increment ReplicationFrame in
    * UReplicationGraph::ServerReplicateActors, and that is blocked only on recovering the
    * offset from *this* binary - the typed PDB's UNetDriver layout does not match it (World at
    * 0x140 there against the SDK's 0x150), so 0x420 cannot simply be copied across.
    */
    // net.ShareShadowState is deliberately left alone. Turning sharing off was a workaround for
    // the frozen ReplicationFrame described above; the counter is now incremented properly in
    // UReplicationGraph::ServerReplicateActors, so the sharing can stay on and do its job.

    /*
    * Required, and tested: with the local player left in place the client UI initializes and dies
    * inside UFortGameInstance::InitializeUI instead. Removing it is what keeps the client half of
    * the game from starting up - it is not the cause of the teardown crash.
    */
    // Reached through the world, exactly as the working reference server does it. Going via
    // UFortEngine::GetEngine()->GameInstance instead relies on a different field resolving to the
    // same object - and if it does not, RemoveAt(0) decrements the count of some other array.
    //
    // RemoveAt and not Remove: on the engine's TArray, Remove takes a value, and Remove(0) on an
    // array of pointers would mean "every null entry" rather than "the entry at index zero".
    UWorld::GetWorld()->OwningGameInstance->LocalPlayers.RemoveAt(0);

    UKismetSystemLibrary::ExecuteConsoleCommand(UWorld::GetWorld(), L"open Asteria_Terrain", nullptr);

    return 0;
}

BOOL APIENTRY DllMain(HMODULE hModule,
    DWORD  ul_reason_for_call,
    LPVOID lpReserved
)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
        CreateThread(0, 0, LaunchWindowsStartup, lpReserved, 0, 0);
        break;
    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }
    return TRUE;
}

