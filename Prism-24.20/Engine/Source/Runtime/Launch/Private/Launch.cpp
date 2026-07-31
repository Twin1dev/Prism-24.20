#include "pch.h"

DWORD WINAPI LaunchWindowsStartup(LPVOID)
{
    Sleep(5000);

    AllocConsole();
    FILE* fptr;
    freopen_s(&fptr, "CONOUT$", "w+", stdout);
    SetConsoleTitleA("Prism | Starting");

    FModuleManager::Get().StartupModules();

    UFortEngine::GetEngine()->GameInstance->LocalPlayers.Remove(0);
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

