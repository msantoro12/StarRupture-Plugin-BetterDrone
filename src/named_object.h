#pragma once
#include "object_ref.h"
#include "plugin_helpers.h"
#include <plugin_interface.h>
#include <chrono>
#include <cstdint>

// An object found by name and kept as an ObjectRef: a class default object or
// a loaded asset. A miss is looked for again every kRetryMs at the earliest,
// since the lookup is a GObjects scan and the object may not be loaded yet.
// Game thread only.
struct NamedObject
{
    static constexpr uint64_t kRetryMs = 5000;

    const char*             name;
    ObjectRef<SDK::UObject> ref;
    uint64_t                nextTryMs  = 0;
    bool                    missLogged = false;

    explicit NamedObject(const char* objectName) : name(objectName) {}

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

        IPluginHooks* hooks = GetSelf() ? GetSelf()->hooks : nullptr;
        IPluginObjectWalker* walker = hooks ? hooks->ObjectWalker : nullptr;
        if (!walker || !walker->IsReady() || !cls)
            return nullptr;

        auto* object = static_cast<SDK::UObject*>(walker->FindFirstObjectByName(name));
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
