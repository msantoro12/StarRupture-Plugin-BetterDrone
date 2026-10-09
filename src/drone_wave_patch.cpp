#include "drone_wave_patch.h"
#include "drone_config.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <windows.h>

// ACrCharacterPlayerBase::CanBuildingDroneBeActive
// Returns false during EnviroWaveStage_PreWave / Moving / Fadeout, blocking drone use.
// We call the original; if it returns false we return true so waves never restrict the drone.
// Other checks in the original (interior, exclusion zones, sliding, being attacked) are
// preserved because we only override the false case, not any of the true paths.

static constexpr const char* kCanBuildingDroneBeActivePattern =
    "48 89 5C 24 ?? 55 56 57 48 83 EC ?? 48 8B D9 48 8B 89";

typedef bool(__fastcall* CanBuildingDroneBeActive_t)(void* thisPtr);
static CanBuildingDroneBeActive_t g_original = nullptr;
static HookHandle                 g_hook      = nullptr;

// Resolved during OnPluginLoadHooks; 0 means the pattern missed on this build.
static uintptr_t                  g_addr      = 0;

// ACrCharacterPlayerBase::CheckIfInInterior
// Called from the character's Tick with every component overlapping any of the
// character's own components (AActor::GetOverlappingComponents), and reports a
// change through ServerSetInInterior. The InInterior / InSafeInterior tags it
// sets are what zero the wave's heat (Tick scales BlendTemperature by them).
// DroneCameraColl is one of the character's components and flies with the
// drone, so in drone mode the drone's surroundings count as the player's: the
// drone parked in a shelter shields an exposed body, and the drone leaving a
// shelter can expose a sheltered body. While the drone is out the check is
// skipped, so the state stays what the body had when the drone launched; the
// body is parked and does not move. On foot nothing changes.
static constexpr const char* kCheckIfInInteriorPattern =
    "48 8B C4 48 89 50 10 48 89 48 08 48 83 EC ?? 48 89 58 18 48 89 68 F8 40 32 ED 48 89 70 F0 40 32 F6";

typedef void(__fastcall* CheckIfInInterior_t)(void* thisPtr, const SDK::TArray<SDK::UPrimitiveComponent*>& overlaps);
static CheckIfInInterior_t g_originalInterior = nullptr;
static HookHandle          g_hookInterior     = nullptr;
static uintptr_t           g_addrInterior     = 0;

// Game thread only. What the drone-side overlaps would have reported, so the
// log line fires on a change rather than every tick.
static int      g_lastDroneSide = -1;
static uint64_t g_lastLogMs     = 0;

static void LogHeldInterior(SDK::ACrCharacterPlayerBase* character,
                            const SDK::TArray<SDK::UPrimitiveComponent*>& overlaps)
{
    bool interior = false;
    bool safe     = false;
    for (int32_t i = 0; i < overlaps.Num(); ++i)
    {
        SDK::UPrimitiveComponent* comp = overlaps[i];
        if (!comp || !comp->IsA(SDK::UInteriorOverlapComponent::StaticClass()))
            continue;

        interior = true;
        safe     = safe || static_cast<SDK::UInteriorOverlapComponent*>(comp)->bIsSafeInterior;
    }

    const int droneSide = (interior ? 1 : 0) | (safe ? 2 : 0);
    const uint64_t now  = GetTickCount64();
    if (droneSide == g_lastDroneSide || now - g_lastLogMs < 1000)
        return;

    g_lastDroneSide = droneSide;
    g_lastLogMs     = now;
    LOG_INFO("WaveShelter: drone out, body holds interior=%d safe=%d; drone-side overlaps say interior=%d safe=%d (ignored)",
             character->IsInInterior() ? 1 : 0, character->IsInSafeInterior() ? 1 : 0,
             interior ? 1 : 0, safe ? 1 : 0);
}

static void __fastcall Detour_CheckIfInInterior(void* thisPtr, const SDK::TArray<SDK::UPrimitiveComponent*>& overlaps)
{
    auto* character = static_cast<SDK::ACrCharacterPlayerBase*>(thisPtr);
    if (character && character->Status == SDK::EPlayerCharacterStatus::BuildingDrone)
    {
        LogHeldInterior(character, overlaps);
        return;
    }

    g_lastDroneSide = -1;
    if (g_originalInterior)
        g_originalInterior(thisPtr, overlaps);
}

static bool __fastcall Detour_CanBuildingDroneBeActive(void* thisPtr)
{
    if (DroneConfig::Config::ReadAlwaysAllowDrone())
        return true;

    return g_original ? g_original(thisPtr) : false;
}

void ResolveWavePatch(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    // Optional: a miss leaves the rest of BetterDrone working, which is what the
    // old scan-at-init path did. The loader still lists it for the user.
    //
    // FUNCTION_START because a detour is written over this address: the loader
    // checks the match is a real function entry with room for the 14-byte jump,
    // rather than taking the pattern's word for it.
    PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
    req.hookName = "ACrCharacterPlayerBase::CanBuildingDroneBeActive";
    req.pattern  = kCanBuildingDroneBeActivePattern;
    req.kind     = PLUGIN_SCAN_FUNCTION_START;
    req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

    g_addr = scanner->Resolve(self, &req);

    PluginScanRequest interiorReq = PLUGIN_SCAN_REQUEST_INIT;
    interiorReq.hookName = "ACrCharacterPlayerBase::CheckIfInInterior";
    interiorReq.pattern  = kCheckIfInInteriorPattern;
    interiorReq.kind     = PLUGIN_SCAN_FUNCTION_START;
    interiorReq.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

    g_addrInterior = scanner->Resolve(self, &interiorReq);
}

static void InitShelterHook()
{
    if (!g_addrInterior)
    {
        LOG_WARN("WaveShelter: CheckIfInInterior unresolved — the drone still counts toward shelter");
        return;
    }

    g_hookInterior = GetSelf()->hooks->Hooks->Install(
        g_addrInterior,
        reinterpret_cast<void*>(&Detour_CheckIfInInterior),
        reinterpret_cast<void**>(&g_originalInterior));

    if (!g_hookInterior)
    {
        LOG_WARN("WaveShelter: hook installation failed");
        return;
    }

    LOG_INFO("WaveShelter: shelter is judged at the parked body while the drone is out");
}

bool InitWavePatch()
{
    InitShelterHook();

    uintptr_t addr = g_addr;

    if (!addr)
    {
        LOG_WARN("WavePatch: CanBuildingDroneBeActive unresolved — wave restriction not patched");
        return false;
    }

    LOG_INFO("WavePatch: CanBuildingDroneBeActive at 0x%llX", addr);

    g_hook = GetSelf()->hooks->Hooks->Install(
        addr,
        reinterpret_cast<void*>(&Detour_CanBuildingDroneBeActive),
        reinterpret_cast<void**>(&g_original));

    if (!g_hook)
    {
        LOG_WARN("WavePatch: hook installation failed");
        return false;
    }

    LOG_INFO("WavePatch: drone wave restriction removed");
    return true;
}

void ShutdownWavePatch()
{
    if (g_hook)
    {
        GetSelf()->hooks->Hooks->Remove(g_hook);
        g_hook      = nullptr;
        g_original  = nullptr;
        LOG_DEBUG("WavePatch: hook removed");
    }

    if (g_hookInterior)
    {
        GetSelf()->hooks->Hooks->Remove(g_hookInterior);
        g_hookInterior     = nullptr;
        g_originalInterior = nullptr;
        g_lastDroneSide    = -1;
        LOG_DEBUG("WaveShelter: hook removed");
    }
}
