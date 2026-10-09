#pragma once
#include <Basic.hpp>
#include <CoreUObject_structs.hpp>

// The mining laser's look and sound, made of the mining tool's own assets:
//
//   * the tool's beam effect, NS_HarvesterResized, spawned on the drone camera
//     for each firing and driven through its user parameters (hit location,
//     active, aiming at ore, heat, cooling);
//   * the tool's sound cues: the start cue and the laser loop at the drone,
//     the ore-grinding loop at the hit point while ore is being mined, the
//     finished cue on release, and the overheat refusal click.
//
// Every component is spawned by this file, held only as an ObjectRef, and
// stopped by Stop(), which the laser calls on every tick it does not fire.
// The loops are attached to the drone camera, so they also end with the
// drone. The assets are loaded by path when firing starts; one that cannot
// be loaded is skipped, and the laser mines as before.

namespace SDK { class ACrCharacterPlayerBase; }

namespace DroneLaserFx
{
    struct Shot
    {
        SDK::FVector hit;       // where the beam ends, world space
        bool         mining;    // a mining request took: the ore look and the grinding loop
        float        heat;      // 0..1
    };

    // Game thread only. Starts the beam and the loops on the first call of a
    // firing, then keeps them on the shot.
    void Fire(SDK::ACrCharacterPlayerBase* character, const Shot& shot);

    // Game thread only. Ends the beam and every loop, with the tool's release
    // cue when the laser was firing; the beam winds down as cooling. Cheap
    // when nothing plays.
    void Stop();

    // Game thread only. The tool's refusal click, for a press while overheated.
    void PlayReject(SDK::ACrCharacterPlayerBase* character);

    // Plugin shutdown. The components are destroyed on the game thread only.
    // Anywhere else (a stock loader older than 1.22 reloading from the render
    // thread) they cannot be touched, and a beam and loops playing at that
    // moment carry on until the drone camera goes.
    void Shutdown(bool onGameThread);
}
