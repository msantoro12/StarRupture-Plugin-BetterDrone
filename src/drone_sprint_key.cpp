#include "drone_sprint_key.h"
#include "drone_key_vk.h"
#include "drone_fields.h"
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
    SDK::UInputAction* FindInputActionFor(const SDK::UClass* handlerClass)
    {
        // The input rows are a feature of their own: off, no sprint or drone
        // move key can be looked up.
        if (!DroneFields::Features::SprintKey.On())
            return nullptr;

        auto* objects = SDK::UObject::GObjects.GetTypedPtr();
        if (!objects)
            return nullptr;

        const SDK::UClass* configClass = SDK::UCrInputConfig::StaticClass();
        if (!configClass || !handlerClass)
            return nullptr;

        const int32_t count = objects->Num();
        for (int32_t i = 0; i < count; ++i)
        {
            SDK::UObject* obj = objects->GetByIndex(i);
            if (!obj || obj->IsDefaultObject() || !obj->IsA(configClass))
                continue;

            auto* config = static_cast<SDK::UCrInputConfig*>(obj);
            auto* actions = DroneFields::NativeInputActions.In(config);
            if (!actions)
                continue;

            const int32_t entryCount = actions->Num();
            for (int32_t j = 0; j < entryCount; ++j)
            {
                const SDK::FCrNativeInputAction& entry = (*actions)[j];
                SDK::UInputAction*         inputAction = DroneFields::ValueOr(DroneFields::NativeInputAction, &entry);
                SDK::UCrNativeInputHandler* handler    = DroneFields::ValueOr(DroneFields::NativeInputHandler, &entry);
                if (inputAction && handler && handler->IsA(handlerClass))
                    return inputAction;
            }
        }

        return nullptr;
    }

    // Looks for a keyboard or mouse key among `mappings` bound to `action`.
    // `boundToOther` is set when the action shows up with a key this plugin
    // cannot poll.
    bool FindKeyFor(const SDK::TArray<SDK::FEnhancedActionKeyMapping>& mappings, const SDK::UInputAction* action,
                    char* outKeyName, size_t outKeyNameSize, int& outVk, bool& boundToOther)
    {
        const int32_t count = mappings.Num();
        for (int32_t i = 0; i < count; ++i)
        {
            const SDK::FEnhancedActionKeyMapping& mapping = mappings[i];
            if (mapping.Action != action)
                continue;

            const std::string name = mapping.Key.KeyName.ToString();
            const int vk = KeyNameToVk(name.c_str());
            if (vk == 0)
            {
                boundToOther = true;
                continue;
            }

            if (outKeyName && outKeyNameSize > 0)
                snprintf(outKeyName, outKeyNameSize, "%s", name.c_str());
            outVk = vk;
            return true;
        }

        return false;
    }

    // The local player's applied mappings, or null (with `why` set) while
    // there is no world, player controller or Enhanced Input yet.
    const SDK::TArray<SDK::FEnhancedActionKeyMapping>* AppliedMappings(const char*& why)
    {
        SDK::UWorld* world = PluginSdkRuntime::GetWorld();
        if (!world)
        {
            why = "no world yet";
            return nullptr;
        }

        SDK::APlayerController* pc = SDK::UGameplayStatics::GetPlayerController(world, 0);
        if (!pc)
        {
            why = "no player controller yet";
            return nullptr;
        }

        SDK::UPlayerInput* playerInput = pc->PlayerInput;
        if (!playerInput || !playerInput->IsA(SDK::UEnhancedPlayerInput::StaticClass()))
        {
            why = "the player has no Enhanced Input yet";
            return nullptr;
        }

        return &static_cast<SDK::UEnhancedPlayerInput*>(playerInput)->EnhancedActionMappings;
    }

    // The key `action` is given by the game's input mapping contexts, for
    // when it is not among the applied mappings: the context holding it is
    // not applied yet (the world is still loading) or is swapped out for
    // the moment. A key the player rebound is only in the applied mappings,
    // which are always tried first.
    bool FindDefaultKeyFor(const SDK::UInputAction* action, char* outKeyName, size_t outKeyNameSize,
                           int& outVk, bool& boundToOther)
    {
        auto* objects = SDK::UObject::GObjects.GetTypedPtr();
        const SDK::UClass* contextClass = SDK::UInputMappingContext::StaticClass();
        if (!objects || !contextClass)
            return false;

        const int32_t count = objects->Num();
        for (int32_t i = 0; i < count; ++i)
        {
            SDK::UObject* obj = objects->GetByIndex(i);
            if (!obj || obj->IsDefaultObject() || !obj->IsA(contextClass))
                continue;

            auto* context = static_cast<SDK::UInputMappingContext*>(obj);
            if (FindKeyFor(context->Mappings, action, outKeyName, outKeyNameSize, outVk, boundToOther))
                return true;
        }

        return false;
    }
}

// Deliberately reads mappings as plain data rather than calling
// IEnhancedInputSubsystemInterface::QueryKeysMappedToAction: that
// Dumper-7 wrapper resolves its UFunction with
// AsUObject()->Class->GetFunction("EnhancedInputSubsystemInterface", ...),
// but an interface's UFunctions are never in the *implementing* class's own
// super chain, so the lookup returns null and the wrapper's unchecked
// Func->FunctionFlags dereferences a null pointer -- an access violation,
// which try/catch cannot catch, crashing the game every time boost tried to
// follow Sprint. A mapping array is data on an object already in hand, so
// no UFunction call -- and no version of this crash -- is possible.
int ResolveInputHandlerVk(const SDK::UClass* handlerClass, char* outKeyName, size_t outKeyNameSize,
                          InputLookup* outStatus, const char** outWhy)
{
    if (outKeyName && outKeyNameSize > 0)
        outKeyName[0] = '\0';

    InputLookup status = InputLookup::NotReady;
    const char* why    = "no result";
    int vk             = 0;

    try
    {
        SDK::UInputAction* action = FindInputActionFor(handlerClass);
        if (!action)
        {
            why = "the game's input config is not loaded yet";
        }
        else
        {
            bool boundToOther = false;

            // The live applied mappings first, so an in-game rebind shows up
            // here without any extra work on this plugin's part.
            const SDK::TArray<SDK::FEnhancedActionKeyMapping>* applied = AppliedMappings(why);
            if (applied && FindKeyFor(*applied, action, outKeyName, outKeyNameSize, vk, boundToOther))
            {
                status = InputLookup::Found;
            }
            else if (FindDefaultKeyFor(action, outKeyName, outKeyNameSize, vk, boundToOther))
            {
                status = InputLookup::FoundDefault;
            }
            else if (boundToOther)
            {
                status = InputLookup::NoKeyboardBinding;
                why    = "it is bound to no keyboard or mouse key";
            }
            else if (applied)
            {
                why = "no mapping context holds it yet";
            }
        }
    }
    catch (...)
    {
        status = InputLookup::NotReady;
        why    = "reading the mappings raised an exception";
        vk     = 0;
    }

    const bool found = status == InputLookup::Found || status == InputLookup::FoundDefault;
    if (!found && outKeyName && outKeyNameSize > 0)
        outKeyName[0] = '\0';
    if (outStatus)
        *outStatus = status;
    if (outWhy)
        *outWhy = found ? nullptr : why;
    return found ? vk : 0;
}

int ResolveSprintVk(char* outKeyName, size_t outKeyNameSize, InputLookup* outStatus, const char** outWhy)
{
    return ResolveInputHandlerVk(SDK::UCrInputSprint::StaticClass(), outKeyName, outKeyNameSize, outStatus, outWhy);
}
