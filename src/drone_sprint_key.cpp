#include "drone_sprint_key.h"
#include "drone_key_vk.h"
#include <Chimera_classes.hpp>
#include <EnhancedInput_classes.hpp>
#include <cstdio>
#include <string>

namespace
{
    // UCrInputConfig is the data asset pairing native input handlers (of
    // which UCrInputSprint is one) with their UInputAction. Scanned by
    // handler class rather than by the action's own object name, which is
    // content-authored and not something this plugin should have to guess.
    // Plain data reads only (GObjects walk, IsA, TArray indexing) -- no
    // UFunction call, so nothing here can hit the null-Func crash class
    // described below.
    SDK::UInputAction* FindSprintInputAction()
    {
        auto* objects = SDK::UObject::GObjects.GetTypedPtr();
        if (!objects)
            return nullptr;

        const SDK::UClass* configClass        = SDK::UCrInputConfig::StaticClass();
        const SDK::UClass* sprintHandlerClass = SDK::UCrInputSprint::StaticClass();
        if (!configClass || !sprintHandlerClass)
            return nullptr;

        const int32_t count = objects->Num();
        for (int32_t i = 0; i < count; ++i)
        {
            SDK::UObject* obj = objects->GetByIndex(i);
            if (!obj || obj->IsDefaultObject() || !obj->IsA(configClass))
                continue;

            auto* config = static_cast<SDK::UCrInputConfig*>(obj);
            const int32_t entryCount = config->NativeInputActionsWithHandlers.Num();
            for (int32_t j = 0; j < entryCount; ++j)
            {
                const SDK::FCrNativeInputAction& entry = config->NativeInputActionsWithHandlers[j];
                if (entry.InputAction && entry.NativeInputHandler &&
                    entry.NativeInputHandler->IsA(sprintHandlerClass))
                    return entry.InputAction;
            }
        }

        return nullptr;
    }
}

// Deliberately reads applied mappings off UEnhancedPlayerInput rather than
// calling IEnhancedInputSubsystemInterface::QueryKeysMappedToAction: that
// Dumper-7 wrapper resolves its UFunction with
// AsUObject()->Class->GetFunction("EnhancedInputSubsystemInterface", ...),
// but an interface's UFunctions are never in the *implementing* class's own
// super chain, so the lookup returns null and the wrapper's unchecked
// Func->FunctionFlags dereferences a null pointer -- an access violation,
// which try/catch cannot catch, crashing the game every time boost tried to
// follow Sprint. EnhancedActionMappings is data on an object already in
// hand, so no UFunction call -- and no version of this crash -- is possible.
int ResolveSprintVk(char* outKeyName, size_t outKeyNameSize)
{
    if (outKeyName && outKeyNameSize > 0)
        outKeyName[0] = '\0';

    try
    {
        SDK::UWorld* world = SDK::UWorld::GetWorld();
        if (!world)
            return 0;

        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!pc)
            return 0;

        SDK::UPlayerInput* playerInput = pc->PlayerInput;
        if (!playerInput || !playerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
            return 0;

        auto* enhancedInput = static_cast<SDK::UEnhancedPlayerInput*>(playerInput);

        SDK::UInputAction* sprintAction = FindSprintInputAction();
        if (!sprintAction)
            return 0;

        // The live applied mappings, so an in-game Sprint rebind shows up
        // here without any extra work on this plugin's part.
        const int32_t mappingCount = enhancedInput->EnhancedActionMappings.Num();
        for (int32_t i = 0; i < mappingCount; ++i)
        {
            const SDK::FEnhancedActionKeyMapping& mapping = enhancedInput->EnhancedActionMappings[i];
            if (mapping.Action != sprintAction)
                continue;

            const std::string name = mapping.Key.KeyName.ToString();
            const int vk = KeyNameToVk(name.c_str());
            if (vk == 0)
                continue;

            if (outKeyName && outKeyNameSize > 0)
                snprintf(outKeyName, outKeyNameSize, "%s", name.c_str());
            return vk;
        }
    }
    catch (...)
    {
    }

    return 0;
}
