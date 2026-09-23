#include "drone_key_vk.h"
#include <windows.h>
#include <cstring>

int KeyNameToVk(const char* name)
{
    if (!name || !name[0])
        return 0;

    if (name[1] == '\0' && name[0] >= 'A' && name[0] <= 'Z')
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
        if (std::strcmp(name, entry.name) == 0)
            return entry.vk;

    return 0;
}
