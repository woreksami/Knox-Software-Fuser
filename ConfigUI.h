#pragma once
// ============================================================
//  ConfigUI.h  –  Minimal Win32 launcher window
//  Mode + IP + Port only.  No adapter detection.
// ============================================================
#include "NetworkFuser.h"

// Returns false if the user closed the window without launching
bool ShowConfigUI(FuserConfig& outCfg);
