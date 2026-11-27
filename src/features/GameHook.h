#pragma once
#include "common.h"

// Hooks the RVA configured as [Addresses] Tick and applies movement features each call.
class GameHook
{
public:
    static bool Install();
    static void Remove();
    static bool IsInstalled();
    static void Refresh();
};
