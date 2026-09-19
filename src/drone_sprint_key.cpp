#include "drone_sprint_key.h"
#include <Chimera_classes.hpp>
#include <EnhancedInput_classes.hpp>
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <string>

namespace
{
    // UE FKey names this plugin turns into a VK to poll: letters, digits,
    // function keys, the six modifiers, a few named keyboard keys, and the
    // five mouse buttons -- the realistic set for a keyboard-and-mouse
    // Sprint binding. Anything else (a gamepad key, most likely) is left
    // for the next candidate.
    int VkFromKeyName(const std::string& name)
    {
        if (name.size() == 1 && name[0] >= 'A' && name[0] <= 'Z')
            return name[0];

        static constexpr struct { const char* name; int vk; } kTable[] = {
            { "Zero", '0' }, { "One", '1' }, { "Two", '2' }, { "Three", '3' }, { "Four", '4' },
            { "Five", '5' }, { "Six", '6' }, { "Seven", '7' }, { "Eight", '8' }, { "Nine", '9' },
            { "F1", VK_F1 }, { "F2", VK_F2 }, { "F3", VK_F3 }, { "F4", VK_F4 },
            { "F5", VK_F5 }, { "F6", VK_F6 }, { "F7", VK_F7 }, { "F8", VK_F8 },
            { "F9", VK_F9 }, { "F10", VK_F10 }, { "F11", VK_F11 }, { "F12", VK_F12 },
            { "LeftShift", VK_LSHIFT }, { "RightShift", VK_RSHIFT },
            { "LeftControl", VK_LCONTROL }, { "RightControl", VK_RCONTROL },
            { "LeftAlt", VK_LMENU }, { "RightAlt", VK_RMENU },
            { "SpaceBar", VK_SPACE }, { "Tab", VK_TAB }, { "CapsLock", VK_CAPITAL },
            { "LeftMouseButton", VK_LBUTTON }, { "RightMouseButton", VK_RBUTTON },
            { "MiddleMouseButton", VK_MBUTTON },
            { "ThumbMouseButton", VK_XBUTTON1 }, { "ThumbMouseButton2", VK_XBUTTON2 },
        };

        for (const auto& entry : kTable)
            if (name == entry.name)
                return entry.vk;

        return 0;
    }

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
            const int vk = VkFromKeyName(name);
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
