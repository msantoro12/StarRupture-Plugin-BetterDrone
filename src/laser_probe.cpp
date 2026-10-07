#include "laser_probe.h"
#include "drone_interact.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <Chimera_classes.hpp>
#include <Engine_classes.hpp>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <windows.h>

namespace
{
    constexpr const char* kCommandName = "bd_laserprobe";

    // ---- limits ---------------------------------------------------------------
    // A forgotten "on" must not grow the log for a whole session. Each category
    // has its own line cap so a chatty one cannot starve the others, and the
    // probe switches itself off after kMaxOnMs whatever else happens.
    constexpr uint64_t kMaxOnMs       = 30ull * 60ull * 1000ull;
    constexpr int      kCapHookLines  = 400;
    constexpr int      kCapStateLines = 300;
    constexpr int      kCapTraceLines = 400;

    // Identical calls closer together than this are folded into one summary
    // line, and the summary is written once the calls stop for kFlushIdleMs.
    constexpr uint64_t kRepeatWindowMs = 3000;
    constexpr uint64_t kFlushIdleMs    = 1500;

    // While the character mines, the mining state is written at most this often
    // beyond the lines for each change.
    constexpr uint64_t kHeartbeatMs = 2000;

    // The trace sweep: how often it runs, how far it reaches and how many
    // distinct hits one snapshot lists. 10000 uu is 100 m, past any mining range
    // and well past the 35 m stock tool range, so a far hit shows too.
    constexpr uint64_t kSweepIntervalMs   = 1000;
    constexpr uint64_t kSweepRelogMs      = 10000;
    constexpr double   kTraceLength       = 10000.0;
    constexpr int      kTraceChannelCount = 32;
    constexpr int      kMaxGroups         = 16;
    constexpr int      kMaxGroupLines     = 8;

    // ACrOreMassBaseActor::MineResourceRequest and SetMiningGrantee are
    // ICrMassMineableInterface members, so their `this` is the interface
    // subobject. The game's own prologue subtracts this from rcx to reach the
    // actor (lea rbx, [rcx - 0x2A8] at 0x147752151 and 0x1477628D9).
    constexpr size_t kMassInterfaceOffset = 0x2A8;

    constexpr const char* kMiningToolCdoName = "Default__BP_MiningTool_C";

    // ---- AOB patterns (Game SDK 659656c, shipping exe md5 be7be1ac...) ---------
    // Each matches exactly once in the image. They are only resolved here and
    // installed when the probe is switched on, so a miss disables that one hook.
    constexpr const char* kPatMineActor =   // UCrMiningComponent::MineResourceRequest(AActor*, FName, float, float, bool)
        "4C 89 44 24 18 55 56 41 55 41 57 48 81 EC A8 00 00 00 4C 8B A9 ?? ?? ?? ?? 4D 8B F8 0F 29 B4 24 ?? ?? ?? ?? "
        "0F 28 F3 48 8B EA 48 8B F1";
    constexpr const char* kPatMineIsm =     // UCrMiningComponent::MineResourceRequest(UPhysicalMaterial*, float, float)
        "40 53 56 41 56 48 83 EC 60 4C 8B B1 ?? ?? ?? ?? 48 8B F2 0F 29 74 24 50 0F 28 F2 44 0F 29 44 24 30 "
        "44 0F 28 C3 48 8B D9 4D 85 F6";
    constexpr const char* kPatMassMine =    // ACrOreMassBaseActor::MineResourceRequest(float, float, AActor*)
        "48 89 5C 24 18 55 56 57 48 83 EC 60 0F 29 74 24 50 48 8D 99 ?? ?? ?? ?? 0F 29 7C 24 40 48 8B CB "
        "49 8B F1 0F 28 FA 0F 28 F1 E8 ?? ?? ?? ??";
    constexpr const char* kPatMassGrantee = // ACrOreMassBaseActor::SetMiningGrantee(AActor*)
        "40 53 55 56 57 48 83 EC 38 48 8D 99 ?? ?? ?? ?? 48 8B F2 48 8B CB E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? "
        "48 8B CB 48 8B E8 E8 ?? ?? ?? ??";
    constexpr const char* kPatStopped =     // UCrMiningComponent::BP_OnMiningStopped(AActor*)
        "48 89 5C 24 10 57 48 83 EC 20 48 8B DA 48 8B F9 E8 ?? ?? ?? ?? 83 F8 03 0F 84 ?? ?? ?? ?? 48 85 DB 74 61 "
        "48 89 74 24 30 E8 ?? ?? ?? ??";
    constexpr const char* kPatClear =       // UCrMiningComponent::BP_ClearMiningRequests()
        "40 53 48 83 EC 20 48 8B D9 33 D2 33 C9 8B C1 48 89 4C 24 30 48 89 8B ?? ?? ?? ?? 48 89 8B ?? ?? ?? ?? "
        "48 89 8B ?? ?? ?? ?? 89 8B ?? ?? ?? ??";

    enum HookId
    {
        kHkMineActor,
        kHkMineIsm,
        kHkMassMine,
        kHkMassGrantee,
        kHkStopped,
        kHkClear,
        kHkCount
    };

    using MineActorFn   = void(__fastcall*)(void* comp, void* actor, uint64_t socket, float damage, float rpm, bool weakSpot);
    using MineIsmFn     = void(__fastcall*)(void* comp, void* physMat, float damage, float rpm);
    using MassMineFn    = void(__fastcall*)(void* iface, float damage, float rpm, void* miner);
    using MassGranteeFn = void(__fastcall*)(void* iface, void* grantee);
    using StoppedFn     = void(__fastcall*)(void* comp, void* actor);
    using ClearFn       = void(__fastcall*)(void* comp);

    MineActorFn   g_origMineActor   = nullptr;
    MineIsmFn     g_origMineIsm     = nullptr;
    MassMineFn    g_origMassMine    = nullptr;
    MassGranteeFn g_origMassGrantee = nullptr;
    StoppedFn     g_origStopped     = nullptr;
    ClearFn       g_origClear       = nullptr;

    struct HookSlot
    {
        const char* label;
        const char* pattern;
        uintptr_t   addr;
        HookHandle  handle;
    };

    HookSlot g_hooks[kHkCount] = {
        { "UCrMiningComponent::MineResourceRequest(actor)",       kPatMineActor,   0, nullptr },
        { "UCrMiningComponent::MineResourceRequest(ISM)",         kPatMineIsm,     0, nullptr },
        { "ACrOreMassBaseActor::MineResourceRequest",             kPatMassMine,    0, nullptr },
        { "ACrOreMassBaseActor::SetMiningGrantee",                kPatMassGrantee, 0, nullptr },
        { "UCrMiningComponent::BP_OnMiningStopped",               kPatStopped,     0, nullptr },
        { "UCrMiningComponent::BP_ClearMiningRequests",           kPatClear,       0, nullptr },
    };

    const char* const kHookShort[kHkCount] = {
        "MineActor", "MineIsm", "MassMine", "MassGrantee", "OnMiningStopped", "ClearMiningRequests"
    };

    bool g_commandRegistered = false;

    // Set by the console command, which runs on the game thread, and read by
    // the tick and by the detours.
    std::atomic<bool>     g_enabled{ false };
    std::atomic<DWORD>    g_gameThreadId{ 0 };
    std::atomic<uint64_t> g_onMs{ 0 };

    std::atomic<int> g_hookLines{ 0 };
    int              g_stateLines = 0;
    int              g_traceLines = 0;

    bool OnGameThread()
    {
        return g_gameThreadId.load(std::memory_order_relaxed) == GetCurrentThreadId();
    }

    // Log text with the time since the probe was switched on.
    void EmitText(const char* text)
    {
        const uint64_t t = GetTickCount64() - g_onMs.load(std::memory_order_relaxed);
        LOG_INFO("[LaserProbe] t=%llu.%03llu %s", t / 1000, t % 1000, text);
    }

    void CapNotice(int cap)
    {
        LOG_WARN("[LaserProbe] line cap (%d) reached for this category; further lines are dropped until "
                 "'bd_laserprobe off' and 'on'.", cap);
    }

    void EmitV(int& counter, int cap, const char* fmt, va_list args)
    {
        if (counter >= cap)
            return;
        if (++counter == cap)
            CapNotice(cap);

        char buf[1000];
        vsnprintf(buf, sizeof(buf), fmt, args);
        buf[sizeof(buf) - 1] = '\0';
        EmitText(buf);
    }

    void EmitHook(const char* fmt, ...)
    {
        // The hook counter is atomic: a detour reached off the game thread
        // logs one raw line and must not race the game thread's lines.
        const int n = g_hookLines.fetch_add(1, std::memory_order_relaxed);
        if (n >= kCapHookLines)
            return;
        if (n + 1 == kCapHookLines)
            CapNotice(kCapHookLines);

        char buf[1000];
        va_list args;
        va_start(args, fmt);
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        buf[sizeof(buf) - 1] = '\0';
        EmitText(buf);
    }

    void EmitState(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        EmitV(g_stateLines, kCapStateLines, fmt, args);
        va_end(args);
    }

    void EmitTrace(const char* fmt, ...)
    {
        va_list args;
        va_start(args, fmt);
        EmitV(g_traceLines, kCapTraceLines, fmt, args);
        va_end(args);
    }

    // ---- reading objects ------------------------------------------------------
    // Everything read through a game pointer that is not the tick's own object
    // is copied into a plain struct inside an SEH-guarded function and turned
    // into text afterwards. The pointer is first checked against the GObjects
    // slot it claims, so a dangling pointer is refused rather than followed.

    enum : uint32_t
    {
        kIsOre         = 1u << 0,   // ACrOreActor
        kIsMassBase    = 1u << 1,   // ACrOreMassBaseActor
        kIsMassHighRes = 1u << 2,   // ACrOreMassHighResActor
        kIsChunk       = 1u << 3,   // ACrStandaloneMeteOreChunk
        kIsMeteor      = 1u << 4,   // ACrMeteOreActor
        kIsPlayer      = 1u << 5,   // ACrCharacterPlayerBase
        kIsDrone       = 1u << 6,   // ACrCharacterDroneBase
        kIsIsm         = 1u << 7,   // UInstancedStaticMeshComponent
        kIsMiningComp  = 1u << 8,   // UCrMiningComponent
    };

    struct ClassSet
    {
        const SDK::UClass* ore        = nullptr;
        const SDK::UClass* massBase   = nullptr;
        const SDK::UClass* massHigh   = nullptr;
        const SDK::UClass* chunk      = nullptr;
        const SDK::UClass* meteor     = nullptr;
        const SDK::UClass* player     = nullptr;
        const SDK::UClass* drone      = nullptr;
        const SDK::UClass* ism        = nullptr;
        const SDK::UClass* miningComp = nullptr;
        const SDK::UClass* weaponData = nullptr;
    };

    // Native classes live for the whole process, so the pointers stay valid;
    // they are looked up on the game thread when the probe is switched on.
    ClassSet g_cls;

    struct ObjDesc
    {
        bool       ok;
        int32_t    index;
        uint32_t   flags;
        SDK::FName name;
        SDK::FName cls;
    };

    bool DescribeSeh(const void* p, ObjDesc* out)
    {
        memset(out, 0, sizeof(*out));
        out->index = -1;
        __try
        {
            const SDK::UObject* o = static_cast<const SDK::UObject*>(p);
            if (!o)
                return false;

            const int32_t idx = o->Index;
            if (idx < 0 || SDK::UObject::GObjects->GetByIndex(idx) != o || !o->Class)
                return false;

            constexpr uint32_t kDeadFlags =
                static_cast<uint32_t>(SDK::EObjectFlags::BeginDestroyed) |
                static_cast<uint32_t>(SDK::EObjectFlags::FinishDestroyed) |
                static_cast<uint32_t>(SDK::EObjectFlags::MirroredGarbage);
            if (static_cast<uint32_t>(o->Flags) & kDeadFlags)
                return false;

            uint32_t f = 0;
            if (g_cls.ore        && o->IsA(g_cls.ore))        f |= kIsOre;
            if (g_cls.massBase   && o->IsA(g_cls.massBase))   f |= kIsMassBase;
            if (g_cls.massHigh   && o->IsA(g_cls.massHigh))   f |= kIsMassHighRes;
            if (g_cls.chunk      && o->IsA(g_cls.chunk))      f |= kIsChunk;
            if (g_cls.meteor     && o->IsA(g_cls.meteor))     f |= kIsMeteor;
            if (g_cls.player     && o->IsA(g_cls.player))     f |= kIsPlayer;
            if (g_cls.drone      && o->IsA(g_cls.drone))      f |= kIsDrone;
            if (g_cls.ism        && o->IsA(g_cls.ism))        f |= kIsIsm;
            if (g_cls.miningComp && o->IsA(g_cls.miningComp)) f |= kIsMiningComp;

            out->index = idx;
            out->name  = o->Name;
            out->cls   = o->Class->Name;
            out->flags = f;
            out->ok    = true;
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            out->ok = false;
            return false;
        }
    }

    std::string NameOf(const SDK::FName& name)
    {
        try { return name.ToString(); }
        catch (...) { return "?"; }
    }

    std::string FlagsText(uint32_t f)
    {
        std::string s;
        auto add = [&](uint32_t bit, const char* text) { if (f & bit) { s += s.empty() ? "" : ","; s += text; } };
        add(kIsOre,         "OreActor");
        add(kIsMassBase,    "MassOre");
        add(kIsMassHighRes, "MassHighRes");
        add(kIsChunk,       "MeteOreChunk");
        add(kIsMeteor,      "MeteOre");
        add(kIsPlayer,      "Player");
        add(kIsDrone,       "Drone");
        add(kIsIsm,         "ISM");
        add(kIsMiningComp,  "MiningComp");
        return s;
    }

    // "Name : Class [flags]", or why the pointer could not be read.
    std::string Describe(const void* p)
    {
        if (!p)
            return "null";

        ObjDesc d;
        if (!DescribeSeh(p, &d))
            return "unreadable";

        std::string s = NameOf(d.name) + " : " + NameOf(d.cls);
        const std::string flags = FlagsText(d.flags);
        if (!flags.empty())
            s += " [" + flags + "]";
        return s;
    }

    // Detours can in principle be reached from a thread that is not the game
    // thread; only the game thread may follow a pointer into an object.
    std::string DescribeFromHook(const void* p)
    {
        if (!p)
            return "null";
        return OnGameThread() ? Describe(p) : "(not read: off the game thread)";
    }

    int32_t IdOf(const SDK::UObject* o)
    {
        return o ? o->Index : -1;
    }

    // ---- hook call statistics -------------------------------------------------

    struct CallStat
    {
        std::atomic<uint64_t> total{ 0 };
        std::atomic<uint64_t> offThread{ 0 };

        // Game thread only below.
        bool      have   = false;
        uint64_t  lastMs = 0;
        uintptr_t keyA   = 0;
        uintptr_t keyB   = 0;
        uint32_t  keyC   = 0;
        uint32_t  keyD   = 0;
        uint64_t  repeats = 0;
        uint64_t  dtSum   = 0;
        uint64_t  dtMin   = ~0ull;
        uint64_t  dtMax   = 0;
    };

    CallStat g_stat[kHkCount];

    uint32_t Bits(float f)
    {
        uint32_t u;
        memcpy(&u, &f, sizeof(u));
        return u;
    }

    void FlushRepeats(int h)
    {
        CallStat& s = g_stat[h];
        if (!s.repeats)
            return;

        EmitHook("%s: %llu identical repeats of the line above, gap min/avg/max = %llu/%llu/%llu ms",
            kHookShort[h], s.repeats, s.dtMin, s.dtSum / s.repeats, s.dtMax);
        s.repeats = 0;
        s.dtSum   = 0;
        s.dtMin   = ~0ull;
        s.dtMax   = 0;
    }

    // Records one call. Returns the gap since the previous call of this hook, or
    // -1 when the call is an identical repeat that only counts toward the summary.
    int64_t NoteCall(int h, uintptr_t a, uintptr_t b, float c, float d, bool foldRepeats)
    {
        CallStat& s = g_stat[h];
        s.total.fetch_add(1, std::memory_order_relaxed);

        const uint64_t now = GetTickCount64();
        const uint64_t dt  = s.have ? now - s.lastMs : 0;
        const bool same = foldRepeats && s.have && dt < kRepeatWindowMs && a == s.keyA && b == s.keyB &&
                          Bits(c) == s.keyC && Bits(d) == s.keyD;
        s.lastMs = now;

        if (same)
        {
            ++s.repeats;
            s.dtSum += dt;
            if (dt < s.dtMin) s.dtMin = dt;
            if (dt > s.dtMax) s.dtMax = dt;
            return -1;
        }

        FlushRepeats(h);
        const bool first = !s.have;
        s.have = true;
        s.keyA = a;
        s.keyB = b;
        s.keyC = Bits(c);
        s.keyD = Bits(d);
        return first ? 0 : static_cast<int64_t>(dt);
    }

    // The detours below only read and log. Each hands the call on unchanged, and
    // each guards its logging so that a failed read can never stop the game's own
    // call from running.

    void __fastcall Detour_MineActor(void* comp, void* actor, uint64_t socket, float damage, float rpm, bool weakSpot)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkMineActor].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkMineActor].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const int64_t gap = NoteCall(kHkMineActor, reinterpret_cast<uintptr_t>(comp),
                    reinterpret_cast<uintptr_t>(actor), damage, rpm, true);
                if (gap >= 0)
                    EmitHook("MineActor comp=%p actor=%p (%s) socket=0x%llX damage=%.4f rpm=%.4f weakSpot=%d gap=%lld ms",
                        comp, actor, DescribeFromHook(actor).c_str(), socket, damage, rpm, weakSpot ? 1 : 0, gap);
            }
        }
        catch (...) {}

        g_origMineActor(comp, actor, socket, damage, rpm, weakSpot);
    }

    void __fastcall Detour_MineIsm(void* comp, void* physMat, float damage, float rpm)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkMineIsm].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkMineIsm].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const int64_t gap = NoteCall(kHkMineIsm, reinterpret_cast<uintptr_t>(comp),
                    reinterpret_cast<uintptr_t>(physMat), damage, rpm, true);
                if (gap >= 0)
                    EmitHook("MineIsm comp=%p physMat=%p (%s) damage=%.4f rpm=%.4f gap=%lld ms",
                        comp, physMat, DescribeFromHook(physMat).c_str(), damage, rpm, gap);
            }
        }
        catch (...) {}

        g_origMineIsm(comp, physMat, damage, rpm);
    }

    void __fastcall Detour_MassMine(void* iface, float damage, float rpm, void* miner)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkMassMine].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkMassMine].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const void* actor = iface ? static_cast<const char*>(iface) - kMassInterfaceOffset : nullptr;
                const int64_t gap = NoteCall(kHkMassMine, reinterpret_cast<uintptr_t>(iface),
                    reinterpret_cast<uintptr_t>(miner), damage, rpm, true);
                if (gap >= 0)
                    EmitHook("MassMine iface=%p actor=%p (%s) damage=%.4f rpm=%.4f miner=%p (%s) gap=%lld ms",
                        iface, actor, DescribeFromHook(actor).c_str(), damage, rpm, miner,
                        DescribeFromHook(miner).c_str(), gap);
            }
        }
        catch (...) {}

        g_origMassMine(iface, damage, rpm, miner);
    }

    void __fastcall Detour_MassGrantee(void* iface, void* grantee)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkMassGrantee].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkMassGrantee].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const void* actor = iface ? static_cast<const char*>(iface) - kMassInterfaceOffset : nullptr;
                const int64_t gap = NoteCall(kHkMassGrantee, reinterpret_cast<uintptr_t>(iface),
                    reinterpret_cast<uintptr_t>(grantee), 0.0f, 0.0f, false);
                EmitHook("MassGrantee iface=%p actor=%p (%s) grantee=%p (%s) gap=%lld ms",
                    iface, actor, DescribeFromHook(actor).c_str(), grantee, DescribeFromHook(grantee).c_str(), gap);
            }
        }
        catch (...) {}

        g_origMassGrantee(iface, grantee);
    }

    void __fastcall Detour_Stopped(void* comp, void* actor)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkStopped].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkStopped].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const int64_t gap = NoteCall(kHkStopped, reinterpret_cast<uintptr_t>(comp),
                    reinterpret_cast<uintptr_t>(actor), 0.0f, 0.0f, false);
                EmitHook("OnMiningStopped comp=%p actor=%p (%s) gap=%lld ms",
                    comp, actor, DescribeFromHook(actor).c_str(), gap);
            }
        }
        catch (...) {}

        g_origStopped(comp, actor);
    }

    void __fastcall Detour_Clear(void* comp)
    {
        try
        {
            if (!OnGameThread())
            {
                g_stat[kHkClear].total.fetch_add(1, std::memory_order_relaxed);
                g_stat[kHkClear].offThread.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                const int64_t gap = NoteCall(kHkClear, reinterpret_cast<uintptr_t>(comp), 0, 0.0f, 0.0f, false);
                EmitHook("ClearMiningRequests comp=%p gap=%lld ms", comp, gap);
            }
        }
        catch (...) {}

        g_origClear(comp);
    }

    void* const kDetours[kHkCount] = {
        reinterpret_cast<void*>(&Detour_MineActor),
        reinterpret_cast<void*>(&Detour_MineIsm),
        reinterpret_cast<void*>(&Detour_MassMine),
        reinterpret_cast<void*>(&Detour_MassGrantee),
        reinterpret_cast<void*>(&Detour_Stopped),
        reinterpret_cast<void*>(&Detour_Clear),
    };

    void** const kOriginals[kHkCount] = {
        reinterpret_cast<void**>(&g_origMineActor),
        reinterpret_cast<void**>(&g_origMineIsm),
        reinterpret_cast<void**>(&g_origMassMine),
        reinterpret_cast<void**>(&g_origMassGrantee),
        reinterpret_cast<void**>(&g_origStopped),
        reinterpret_cast<void**>(&g_origClear),
    };

    int InstallHooks(IPluginSelf* self)
    {
        auto* hooks = self->hooks->Hooks;
        int installed = 0;
        for (int h = 0; h < kHkCount; ++h)
        {
            if (!g_hooks[h].addr)
            {
                LOG_WARN("[LaserProbe] %s: pattern did not resolve, not logged.", g_hooks[h].label);
                continue;
            }

            g_hooks[h].handle = hooks->Install(g_hooks[h].addr, kDetours[h], kOriginals[h]);
            if (!g_hooks[h].handle)
            {
                LOG_WARN("[LaserProbe] %s: hook install failed, not logged.", g_hooks[h].label);
                continue;
            }

            LOG_INFO("[LaserProbe] %s: logging at 0x%llX", g_hooks[h].label,
                static_cast<unsigned long long>(g_hooks[h].addr));
            ++installed;
        }
        return installed;
    }

    void RemoveHooks(IPluginSelf* self)
    {
        auto* hooks = self->hooks->Hooks;
        for (int h = 0; h < kHkCount; ++h)
        {
            if (!g_hooks[h].handle)
                continue;

            hooks->Remove(g_hooks[h].handle);
            g_hooks[h].handle = nullptr;
            *kOriginals[h] = nullptr;
        }
    }

    // ---- per-session state ----------------------------------------------------

    struct SessionState
    {
        bool     inited         = false;
        uint32_t status         = 0xFFFFFFFFu;
        int32_t  weaponDataId   = -2;
        int32_t  oreId          = -2;
        int32_t  matId          = -2;
        float    rpm            = -1.0f;
        float    damage         = -1.0f;
        bool     mining         = false;
        uint64_t heartbeatMs    = 0;
        int32_t  heat           = -1;
        int32_t  heatLimit      = -1;
        uint64_t sweepMs        = 0;
        uint64_t lastLogMs      = 0;
        std::string lastSig;
        bool     cdoLogged      = false;
        int      cdoTries       = 0;
        uint64_t cdoNextMs      = 0;
    };

    SessionState g_s;

    std::string CurveText(const SDK::FScalableFloat& f)
    {
        std::string s = "value=";
        char buf[48];
        snprintf(buf, sizeof(buf), "%.4f", f.Value);
        s += buf;
        if (f.Curve.CurveTable)
            s += " curve=" + Describe(f.Curve.CurveTable) + " row=" + NameOf(f.Curve.RowName);
        else
            s += " curve=none";
        return s;
    }

    void LogWeaponData(const char* what, SDK::UCrWeaponItemDataBase* d)
    {
        if (!d)
        {
            EmitState("%s: none", what);
            return;
        }

        std::string tags;
        try
        {
            const int32_t n = d->WeaponType.Num();
            for (int32_t i = 0; i < n && i < 4; ++i)
                tags += (tags.empty() ? "" : ",") + NameOf(d->WeaponType[i].TagName);
        }
        catch (...) { tags = "?"; }

        EmitState("%s: %s miningTypeDamage=%.4f weakpointMultiplier=%.4f shotAggro=%.4f | baseRange %s | "
                  "roundsPerMinute %s | enemyDamage %s | weaponType=[%s]",
            what, Describe(d).c_str(), d->MiningTypeDamage, d->WeakpointDamageMultiplier, d->ShotAggro,
            CurveText(d->BaseRange).c_str(), CurveText(d->RoundsPerMinute).c_str(),
            CurveText(d->BaseDamage).c_str(), tags.c_str());
    }

    // The mining tool's data asset, once, to compare with what the equipped
    // weapon reports on foot and in the drone.
    void LogToolCdo(uint64_t now)
    {
        if (g_s.cdoLogged || g_s.cdoTries >= 6 || now < g_s.cdoNextMs)
            return;

        ++g_s.cdoTries;
        g_s.cdoNextMs = now + 5000;

        IPluginHooks* hooks = GetSelf()->hooks;
        IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady())
            return;

        auto* object = static_cast<SDK::UObject*>(walker->FindFirstObjectByName(kMiningToolCdoName));
        if (!object || !g_cls.weaponData || !object->IsA(g_cls.weaponData))
        {
            if (g_s.cdoTries == 6)
                EmitState("mining tool CDO '%s' not found", kMiningToolCdoName);
            return;
        }

        g_s.cdoLogged = true;
        LogWeaponData("mining tool CDO", static_cast<SDK::UCrWeaponItemDataBase*>(object));
    }

    void LogStatusAndWeapon(SDK::ACrCharacterPlayerBase* character)
    {
        const uint32_t status = static_cast<uint32_t>(character->Status);
        SDK::UCrWeaponComponent* weapons = character->WeaponSystem;
        SDK::UCrWeaponItemDataBase* data = weapons ? weapons->LastEquippedWeaponData : nullptr;
        const int32_t dataId = IdOf(data);

        if (status == g_s.status && dataId == g_s.weaponDataId)
            return;

        g_s.status       = status;
        g_s.weaponDataId = dataId;

        EmitState("character status=%u (%s)", status, status == 1 ? "BuildingDrone" : "on foot");
        LogWeaponData("last equipped weapon data", data);
    }

    void LogHeat(SDK::ACrCharacterPlayerBase* character)
    {
        const SDK::FHarvesterRepHeatStackInfo& heat = character->CurrentReplicatedHarvesterHeatStack;
        if (heat.CurrentStackCount == g_s.heat && heat.StackLimit == g_s.heatLimit)
            return;

        g_s.heat      = heat.CurrentStackCount;
        g_s.heatLimit = heat.StackLimit;
        EmitState("harvester heat stack current=%d old=%d limit=%d%s", heat.CurrentStackCount, heat.OldStackCount,
            heat.StackLimit, (heat.StackLimit > 0 && heat.CurrentStackCount >= heat.StackLimit) ? " (at the limit)" : "");
    }

    // The component's own record of what it is mining: the damage and interval
    // the stock game handed it, which are the values GetMiningDamage and
    // GetMiningRPM returned for the equipped weapon on that call. Returns
    // whether anything is being mined.
    bool LogMiningState(SDK::ACrCharacterPlayerBase* character, uint64_t now)
    {
        SDK::UCrMiningComponent* comp = character->MiningComponent;
        if (!comp)
            return false;

        const SDK::FCrOreMiningState& st = comp->CurrentlyMinedOre;
        const int32_t oreId = IdOf(st.OreActor);
        const int32_t matId = IdOf(st.InfiniteOrePhysicalMaterial);
        const bool mining = oreId >= 0 || matId >= 0;

        const bool changed = oreId != g_s.oreId || matId != g_s.matId || st.MiningRPM != g_s.rpm ||
                             st.MiningDamage != g_s.damage;
        if (changed)
        {
            g_s.oreId  = oreId;
            g_s.matId  = matId;
            g_s.rpm    = st.MiningRPM;
            g_s.damage = st.MiningDamage;

            if (mining)
            {
                EmitState("mining state: ore=%s infinitePhysMat=%s objectType=%u damage=%.4f rpm=%.4f "
                          "applied=%.4f weakSpot=%d",
                    Describe(st.OreActor).c_str(), Describe(st.InfiniteOrePhysicalMaterial).c_str(),
                    static_cast<unsigned>(st.MiningObjectType), st.MiningDamage, st.MiningRPM, st.DamageApplied,
                    st.bIsMiningWeakSpot ? 1 : 0);
                g_s.heartbeatMs = now;
            }
            else if (g_s.mining)
            {
                EmitState("mining state cleared");
            }
        }
        else if (mining && now - g_s.heartbeatMs >= kHeartbeatMs)
        {
            g_s.heartbeatMs = now;
            EmitState("mining state (heartbeat): ore=%s infinitePhysMat=%s damage=%.4f rpm=%.4f applied=%.4f",
                Describe(st.OreActor).c_str(), Describe(st.InfiniteOrePhysicalMaterial).c_str(),
                st.MiningDamage, st.MiningRPM, st.DamageApplied);
        }

        g_s.mining = mining;
        return mining;
    }

    // ---- trace sweep ----------------------------------------------------------

    struct HitGroup
    {
        int32_t  actorId;
        int32_t  compId;
        int32_t  item;
        float    distance;
        uint32_t channelMask[2];   // [0] simple collision, [1] complex collision
        ObjDesc  actor;
        ObjDesc  comp;
        ObjDesc  mat;
    };

    std::string MaskText(uint32_t mask)
    {
        if (!mask)
            return "-";

        std::string s;
        char buf[8];
        for (int c = 0; c < kTraceChannelCount; ++c)
        {
            if (!(mask & (1u << c)))
                continue;
            snprintf(buf, sizeof(buf), "%s%d", s.empty() ? "" : ",", c + 1);
            s += buf;
        }
        return s;
    }

    std::string ObjText(const ObjDesc& d)
    {
        if (!d.ok)
            return "none";
        std::string s = NameOf(d.name) + " : " + NameOf(d.cls);
        const std::string flags = FlagsText(d.flags);
        if (!flags.empty())
            s += " [" + flags + "]";
        return s;
    }

    // One line trace from the eyes the weapon trace would use, on every
    // ETraceTypeQuery channel, simple and complex. The hits are grouped by what
    // they hit, with the list of channels that reached it, because which channel
    // sees ore is the question.
    void SweepTraces(SDK::ACrCharacterPlayerBase* character, bool inDrone, uint64_t now)
    {
        SDK::FVector eye{};
        SDK::FRotator rot{};
        character->GetActorEyesViewPoint(&eye, &rot);

        const double kDegToRad = 3.14159265358979323846 / 180.0;
        const double pitch = rot.Pitch * kDegToRad;
        const double yaw   = rot.Yaw * kDegToRad;
        SDK::FVector dir{};
        dir.X = std::cos(pitch) * std::cos(yaw);
        dir.Y = std::cos(pitch) * std::sin(yaw);
        dir.Z = std::sin(pitch);

        SDK::FVector end{};
        end.X = eye.X + dir.X * kTraceLength;
        end.Y = eye.Y + dir.Y * kTraceLength;
        end.Z = eye.Z + dir.Z * kTraceLength;

        HitGroup groups[kMaxGroups];
        int groupCount = 0;
        int totalHits  = 0;
        uint32_t noHit[2] = { 0, 0 };
        bool overflow = false;

        const SDK::TArray<SDK::AActor*> ignore{};
        const SDK::FLinearColor colour{};

        for (int complex = 0; complex < 2; ++complex)
        {
            for (int ch = 0; ch < kTraceChannelCount; ++ch)
            {
                SDK::FHitResult hit{};
                const bool hitSomething = SDK::UKismetSystemLibrary::LineTraceSingle(character, eye, end,
                    static_cast<SDK::ETraceTypeQuery>(ch), complex != 0, ignore, SDK::EDrawDebugTrace::None, &hit,
                    true, colour, colour, 0.0f);
                if (!hitSomething || !hit.bBlockingHit)
                {
                    noHit[complex] |= 1u << ch;
                    continue;
                }

                ++totalHits;
                SDK::UPrimitiveComponent* comp = hit.Component.Get();
                SDK::UObject* actorObj = hit.HitObjectHandle.ReferenceObject.Get();
                if (!actorObj && comp)
                    actorObj = comp->GetOwner();

                const int32_t actorId = IdOf(actorObj);
                const int32_t compId  = IdOf(comp);

                int g = 0;
                for (; g < groupCount; ++g)
                    if (groups[g].actorId == actorId && groups[g].compId == compId && groups[g].item == hit.Item)
                        break;

                if (g == groupCount)
                {
                    if (groupCount == kMaxGroups)
                    {
                        overflow = true;
                        continue;
                    }

                    HitGroup& n = groups[groupCount++];
                    memset(&n, 0, sizeof(n));
                    n.actorId  = actorId;
                    n.compId   = compId;
                    n.item     = hit.Item;
                    n.distance = hit.Distance;
                    DescribeSeh(actorObj, &n.actor);
                    DescribeSeh(comp, &n.comp);
                    DescribeSeh(hit.PhysMaterial.Get(), &n.mat);
                }
                groups[g].channelMask[complex] |= 1u << ch;
            }
        }

        // The signature leaves out the distance: a moving drone would otherwise
        // log every snapshot.
        std::string sig;
        char tmp[96];
        for (int g = 0; g < groupCount; ++g)
        {
            snprintf(tmp, sizeof(tmp), "|%d,%d,%d,%u,%u", groups[g].actorId, groups[g].compId, groups[g].item,
                groups[g].channelMask[0], groups[g].channelMask[1]);
            sig += tmp;
        }

        if (sig == g_s.lastSig && now - g_s.lastLogMs < kSweepRelogMs)
            return;

        g_s.lastSig   = sig;
        g_s.lastLogMs = now;

        const SDK::FVector body = character->K2_GetActorLocation();
        const double dx = eye.X - body.X, dy = eye.Y - body.Y, dz = eye.Z - body.Z;
        EmitTrace("trace %s: eye=(%.0f %.0f %.0f) dir=(%.3f %.3f %.3f) eyeToBody=%.0f uu length=%.0f | %d hits over "
                  "%d queries, %d distinct",
            inDrone ? "DRONE" : "foot", eye.X, eye.Y, eye.Z, dir.X, dir.Y, dir.Z,
            std::sqrt(dx * dx + dy * dy + dz * dz), kTraceLength, totalHits, 2 * kTraceChannelCount, groupCount);

        // Nearest first, so the thing the crosshair is on leads.
        int order[kMaxGroups];
        for (int i = 0; i < groupCount; ++i)
            order[i] = i;
        for (int i = 1; i < groupCount; ++i)
            for (int j = i; j > 0 && groups[order[j]].distance < groups[order[j - 1]].distance; --j)
            {
                const int t = order[j]; order[j] = order[j - 1]; order[j - 1] = t;
            }

        for (int i = 0; i < groupCount && i < kMaxGroupLines; ++i)
        {
            const HitGroup& g = groups[order[i]];
            EmitTrace("  hit dist=%.0f uu item=%d actor=%s | comp=%s | physMat=%s | channels(simple)=%s channels(complex)=%s",
                g.distance, g.item, ObjText(g.actor).c_str(), ObjText(g.comp).c_str(), ObjText(g.mat).c_str(),
                MaskText(g.channelMask[0]).c_str(), MaskText(g.channelMask[1]).c_str());
        }
        if (groupCount > kMaxGroupLines || overflow)
            EmitTrace("  (more distinct hits than listed)");
        if (noHit[0] || noHit[1])
            EmitTrace("  no blocking hit on channels(simple)=%s channels(complex)=%s",
                MaskText(noHit[0]).c_str(), MaskText(noHit[1]).c_str());
    }

    // ---- tick -----------------------------------------------------------------

    void Disable(IPluginSelf* self, const char* why);

    void TickImpl()
    {
        IPluginSelf* self = GetSelf();
        const uint64_t now = GetTickCount64();

        if (now - g_onMs.load(std::memory_order_relaxed) > kMaxOnMs)
        {
            Disable(self, "time limit reached");
            return;
        }

        // Fold-in summaries for calls that have gone quiet.
        for (int h = 0; h < kHkCount; ++h)
            if (g_stat[h].repeats && now - g_stat[h].lastMs >= kFlushIdleMs)
                FlushRepeats(h);

        LogToolCdo(now);

        SDK::ACrCharacterPlayerBase* character = LocalPlayerCharacter();
        if (!character)
        {
            g_s.status       = 0xFFFFFFFFu;
            g_s.weaponDataId = -2;
            return;
        }

        LogStatusAndWeapon(character);
        LogHeat(character);
        const bool mining = LogMiningState(character, now);

        // From the drone always. On foot only while the character is actually
        // mining: that is the one case where the stock game proves which target
        // and which channel work, and it keeps the sweep off the rest of play.
        const bool inDrone = IsLocalPlayerInDrone();
        if ((inDrone || mining) && now - g_s.sweepMs >= kSweepIntervalMs)
        {
            g_s.sweepMs = now;
            SweepTraces(character, inDrone, now);
        }
    }

    // The SEH guard lives here because TickImpl builds C++ objects that need
    // unwinding, which a function containing __try cannot do.
    bool RunGuarded()
    {
        __try
        {
            TickImpl();
            return true;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return false;
        }
    }

    void CacheClasses()
    {
        g_cls.ore        = SDK::ACrOreActor::StaticClass();
        g_cls.massBase   = SDK::ACrOreMassBaseActor::StaticClass();
        g_cls.massHigh   = SDK::ACrOreMassHighResActor::StaticClass();
        g_cls.chunk      = SDK::ACrStandaloneMeteOreChunk::StaticClass();
        g_cls.meteor     = SDK::ACrMeteOreActor::StaticClass();
        g_cls.player     = SDK::ACrCharacterPlayerBase::StaticClass();
        g_cls.drone      = SDK::ACrCharacterDroneBase::StaticClass();
        g_cls.ism        = SDK::UInstancedStaticMeshComponent::StaticClass();
        g_cls.miningComp = SDK::UCrMiningComponent::StaticClass();
        g_cls.weaponData = SDK::UCrWeaponItemDataBase::StaticClass();
    }

    void Enable(IPluginSelf* self)
    {
        if (g_enabled.load())
            return;

        g_gameThreadId.store(GetCurrentThreadId());
        g_onMs.store(GetTickCount64());
        g_hookLines.store(0);
        g_stateLines = 0;
        g_traceLines = 0;
        g_s = SessionState{};
        for (int h = 0; h < kHkCount; ++h)
        {
            g_stat[h].total.store(0);
            g_stat[h].offThread.store(0);
            g_stat[h].have    = false;
            g_stat[h].repeats = 0;
            g_stat[h].dtSum   = 0;
            g_stat[h].dtMin   = ~0ull;
            g_stat[h].dtMax   = 0;
        }

        CacheClasses();
        const int installed = InstallHooks(self);
        LOG_INFO("[LaserProbe] on: %d of %d call hooks logging. Mine on foot, then fly the drone and aim at ore. "
                 "Auto-off after %llu min.", installed, static_cast<int>(kHkCount),
                 static_cast<unsigned long long>(kMaxOnMs / 60000));
        g_enabled.store(true);
    }

    void Disable(IPluginSelf* self, const char* why)
    {
        if (!g_enabled.exchange(false))
            return;

        for (int h = 0; h < kHkCount; ++h)
            FlushRepeats(h);

        if (self && self->hooks && self->hooks->Hooks)
            RemoveHooks(self);

        LOG_INFO("[LaserProbe] off (%s). Calls seen: MineActor=%llu MineIsm=%llu MassMine=%llu MassGrantee=%llu "
                 "OnMiningStopped=%llu ClearMiningRequests=%llu; off-thread calls: %llu.",
            why,
            static_cast<unsigned long long>(g_stat[kHkMineActor].total.load()),
            static_cast<unsigned long long>(g_stat[kHkMineIsm].total.load()),
            static_cast<unsigned long long>(g_stat[kHkMassMine].total.load()),
            static_cast<unsigned long long>(g_stat[kHkMassGrantee].total.load()),
            static_cast<unsigned long long>(g_stat[kHkStopped].total.load()),
            static_cast<unsigned long long>(g_stat[kHkClear].total.load()),
            static_cast<unsigned long long>(
                g_stat[kHkMineActor].offThread.load() + g_stat[kHkMineIsm].offThread.load() +
                g_stat[kHkMassMine].offThread.load() + g_stat[kHkMassGrantee].offThread.load() +
                g_stat[kHkStopped].offThread.load() + g_stat[kHkClear].offThread.load()));
    }

    // "bd_laserprobe [on|off]" -- no argument prints the state. gameThread = true,
    // so this runs on the tick.
    void HandleLaserProbe(const char* const* argv, int argc, PluginConsoleSink sink, void* userData)
    {
        IPluginSelf* self = static_cast<IPluginSelf*>(userData);
        if (!self || !self->hooks || !self->hooks->Console)
            return;

        IPluginConsole* console = self->hooks->Console;

        if (argc >= 2)
        {
            if (_stricmp(argv[1], "on") == 0 || strcmp(argv[1], "1") == 0)
                Enable(self);
            else if (_stricmp(argv[1], "off") == 0 || strcmp(argv[1], "0") == 0)
                Disable(self, "switched off");
            else
            {
                console->Printf(sink, PluginConsoleLineKind::Error, "Usage: bd_laserprobe [on|off]");
                return;
            }
        }

        console->Printf(sink, PluginConsoleLineKind::Output,
            "Laser probe is %s. Mine ore on foot, then fly the drone and aim at ore; grep the log for [LaserProbe].",
            g_enabled.load() ? "on" : "off");
    }

    uintptr_t ResolveHook(IPluginSelf* self, IPluginHookScanner* scanner, const char* name, const char* pattern)
    {
        PluginScanRequest req = PLUGIN_SCAN_REQUEST_INIT;
        req.hookName = name;
        req.pattern  = pattern;
        req.kind     = PLUGIN_SCAN_FUNCTION_START;
        req.flags    = PLUGIN_SCAN_FLAG_OPTIONAL;

        return scanner->Resolve(self, &req);
    }
}

void ResolveLaserProbe(IPluginSelf* self, IPluginHookScanner* scanner)
{
    if (!self || !scanner)
        return;

    // Optional throughout: a miss drops that one hook from the probe.
    for (int h = 0; h < kHkCount; ++h)
        g_hooks[h].addr = ResolveHook(self, scanner, g_hooks[h].label, g_hooks[h].pattern);
}

void InitLaserProbe(IPluginSelf* self)
{
    if (!self || !self->hooks || !self->hooks->Console)
    {
        LOG_WARN("LaserProbe: console unavailable, '%s' not registered.", kCommandName);
        return;
    }

    PluginConsoleCommandDesc desc{};
    desc.name       = kCommandName;
    desc.usage      = "bd_laserprobe [on|off]";
    desc.help       = "Log how mining works on foot and what the drone camera hits, for the drone laser. Off by default.";
    desc.handler    = &HandleLaserProbe;
    desc.userData   = self;
    desc.gameThread = true;

    g_commandRegistered = self->hooks->Console->RegisterCommand(self, &desc);
    if (!g_commandRegistered)
        LOG_WARN("LaserProbe: console command '%s' is already taken.", kCommandName);
}

void ShutdownLaserProbe(IPluginSelf* self)
{
    Disable(self, "plugin shutdown");

    if (g_commandRegistered && self && self->hooks && self->hooks->Console)
        self->hooks->Console->UnregisterCommand(self, kCommandName);
    g_commandRegistered = false;
}

void TickLaserProbe(float)
{
    if (!g_enabled.load(std::memory_order_relaxed))
        return;

    // Fail closed: a bad read ends the probe instead of repeating every tick.
    try
    {
        if (RunGuarded())
            return;

        LOG_ERROR("[LaserProbe] access violation while reading the game; probe switched off");
    }
    catch (...)
    {
        LOG_ERROR("[LaserProbe] read threw; probe switched off");
    }

    Disable(GetSelf(), "read failed");
}
