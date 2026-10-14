#pragma once

#include "Brand.h"

#include <windows.h>

constexpr UINT WM_APP_REGISTRY = WM_APP + 1;
constexpr UINT WM_APP_SHOW = WM_APP + 2;
constexpr UINT WM_APP_EXIT = WM_APP + 3;

int RunApp(HINSTANCE instance, bool showWindow);
