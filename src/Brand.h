#pragma once

// Product name: TidyTray — tidies the notification area by hiding unused icons.
inline constexpr wchar_t kAppName[] = L"TidyTray";
inline constexpr wchar_t kAppExe[] = L"TidyTray.exe";
inline constexpr wchar_t kWindowClass[] = L"TidyTray.Main";
inline constexpr wchar_t kMutexName[] = L"Local\\TidyTray.SingleInstance";
inline constexpr wchar_t kRunValue[] = L"TidyTray";
inline constexpr wchar_t kLegacyRunValue[] = L"HideTray";
inline constexpr wchar_t kLegacyExe[] = L"HideTray.exe";
inline constexpr wchar_t kAppDataDir[] = L"TidyTray";
inline constexpr wchar_t kLegacyAppDataDir[] = L"HideTray";
inline constexpr wchar_t kRulesMagic[] = L"TidyTrayRules 1";
inline constexpr wchar_t kLegacyRulesMagic[] = L"HideTrayRules 1";
