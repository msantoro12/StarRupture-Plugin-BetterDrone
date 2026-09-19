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
    // five mouse buttons -- the realistic set QueryKeysMappedToAction can
    // return for a keyboard-and-mouse Sprint binding. Anything else (a
    // gamepad key, most likely) is left for the next candidate.
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

        SDK::ULocalPlayerSubsystem* subsystem =
            SDK::USubsystemBlueprintLibrary::GetLocalPlayerSubSystemFromPlayerController(
                pc, SDK::UEnhancedInputLocalPlayerSubsystem::StaticClass());
        if (!subsystem)
            return 0;

        SDK::UInputAction* sprintAction = FindSprintInputAction();
        if (!sprintAction)
            return 0;

        // IEnhancedInputSubsystemInterface is a Dumper-7 native-interface
        // wrapper: no vtable or members of its own, just methods that call
        // back through the UObject they are cast from.
        auto* iface = reinterpret_cast<SDK::IEnhancedInputSubsystemInterface*>(subsystem);
        SDK::TArray<SDK::FKey> keys = iface->QueryKeysMappedToAction(sprintAction);

        const int32_t keyCount = keys.Num();
        for (int32_t i = 0; i < keyCount; ++i)
        {
            const std::string name = keys[i].KeyName.ToString();
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
