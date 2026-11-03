// Halo Map Porter - the dark theme (Windows). Settings: Light, Dark, or as Windows is set; read once, at start.
//
// init() before any window is made. From then on every window the app makes on this thread is themed as it's made:
// title bars dark, buttons, lists, combo boxes, scroll bars and context menus in Windows' own dark styles, tabs drawn
// dark, and the colours windows ask for (WM_CTLCOLOR..., syscolor()) dark ones. Message boxes and the file pickers are
// Windows' own and left as Windows draws them.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace dark {

// 0 light, 1 dark, 2 as Windows' "app mode" is set.
void init(int mode);
bool on();

// GetSysColor / GetSysColorBrush as the theme has them (the window and button faces dark, their text light).
COLORREF syscolor(int index);
HBRUSH sysbrush(int index);

}   // namespace dark
