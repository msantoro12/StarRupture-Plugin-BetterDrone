#pragma once
#include "object_ref.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <chrono>
#include <cstdint>

// An object found by name and kept as an ObjectRef: a class default object or
// an asset. An asset given with its path is loaded from it if it is not in
// memory, since the assets the game loads with a tool are not there while
// another tool is held. Nothing keeps such an asset loaded, so its user
// calls Forget() before each use that may follow a garbage collection. A
// miss is looked for again every kRetryMs at the earliest, since the lookup
// is a GObjects scan or a load. Game thread only.
struct NamedObject
{
    static constexpr uint64_t kRetryMs = 5000;

    const char*             name;
    const char*             path = nullptr;  // "/Game/Dir/Asset.Asset", for an asset
    ObjectRef<SDK::UObject> ref;
    uint64_t                nextTryMs  = 0;
    bool                    missLogged = false;

    explicit NamedObject(const char* objectName, const char* assetPath = nullptr)
        : name(objectName), path(assetPath) {}

    // Drops the cached object, so the next Resolve looks again at once.
    void Forget()
    {
        ref.Reset();
        nextTryMs = 0;
    }

    // The object, or null while it is not found. cls is what it must be.
    SDK::UObject* Resolve(const SDK::UClass* cls)
    {
        if (SDK::UObject* object = ref.Get())
            return object;

        const uint64_t now = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
        if (now < nextTryMs)
            return nullptr;
        nextTryMs = now + kRetryMs;

        if (!cls)
            return nullptr;

        IPluginHooks* hooks = GetSelf() ? GetSelf()->hooks : nullptr;
        SDK::UObject* object = nullptr;
        if (path)
        {
            if (!hooks || !hooks->Pak)
                return nullptr;
            object = static_cast<SDK::UObject*>(hooks->Pak->LoadObject(path));
        }
        else
        {
            IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
            if (!walker || !walker->IsReady())
                return nullptr;
            object = static_cast<SDK::UObject*>(walker->FindFirstObjectByName(name));
        }

        if (!object || !object->IsA(cls))
        {
            if (!missLogged)
            {
                LOG_WARN("DroneLaser: '%s' not found (yet); looking again every %llu s", name, kRetryMs / 1000);
                missLogged = true;
            }
            return nullptr;
        }

        ref.Set(object);
        return object;
    }
};
