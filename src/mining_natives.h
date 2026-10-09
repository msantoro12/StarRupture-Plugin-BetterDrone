#pragma once
#include <cstdint>

// What the mining component's own mining requests look like in the shipping
// executable (Game SDK 659656c). Each pattern is a function start and matches
// exactly once in the image.
namespace MiningNatives
{
    // UCrMiningComponent::MineResourceRequest(AActor*, FName, float, float, bool)
    constexpr const char* kPatMineActor =
        "4C 89 44 24 18 55 56 41 55 41 57 48 81 EC A8 00 00 00 4C 8B A9 ?? ?? ?? ?? 4D 8B F8 0F 29 B4 24 ?? ?? ?? ?? "
        "0F 28 F3 48 8B EA 48 8B F1";

    // UCrMiningComponent::MineResourceRequest(UPhysicalMaterial*, float, float)
    constexpr const char* kPatMineIsm =
        "40 53 56 41 56 48 83 EC 60 4C 8B B1 ?? ?? ?? ?? 48 8B F2 0F 29 74 24 50 0F 28 F2 44 0F 29 44 24 30 "
        "44 0F 28 C3 48 8B D9 4D 85 F6";

    // The FName socket travels by value in r8, as one 8-byte integer
    // (ComparisonIndex, Number); NAME_None is 0. rpm is the time between hits
    // in seconds, despite its name.
    using MineActorFn = void(__fastcall*)(void* comp, void* actor, uint64_t socket, float damage, float rpm, bool weakSpot);
    using MineIsmFn   = void(__fastcall*)(void* comp, void* physMat, float damage, float rpm);

    // The mining tool's item data, which carries its damage, range and hit
    // interval.
    constexpr const char* kMiningToolCdoName = "Default__BP_MiningTool_C";
}
