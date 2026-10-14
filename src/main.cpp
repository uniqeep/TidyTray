#include "Brand.h"
#include "Icons.h"
#include "Ui.h"

#include <objbase.h>
#include <shellapi.h>

#include <cstring>
#include <string>

namespace {

using RtlGetVersionPtr = LONG(WINAPI*)(OSVERSIONINFOW*);

bool IsWindows11() {
  const FARPROC procedure = GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
  RtlGetVersionPtr query = nullptr;
  if (procedure) memcpy(&query, &procedure, sizeof(query));
  if (!query) return false;
  OSVERSIONINFOW info{};
  info.dwOSVersionInfoSize = sizeof(info);
  if (query(&info) != 0) return false;
  return info.dwMajorVersion > 10 || (info.dwMajorVersion == 10 && info.dwBuildNumber >= 22000);
}

bool EqualsArg(const wchar_t* text, const wchar_t* option) {
  return _wcsicmp(text, option) == 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  int argc = 0;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  bool silent = false;
  bool exitRequested = false;
  std::wstring dumpPath;
  std::wstring iconPreview;
  for (int i = 1; argv && i < argc; ++i) {
    if (EqualsArg(argv[i], L"/silent") || EqualsArg(argv[i], L"--silent")) {
      silent = true;
    } else if (EqualsArg(argv[i], L"/exit") || EqualsArg(argv[i], L"--exit")) {
      exitRequested = true;
    } else if (EqualsArg(argv[i], L"/dump") || EqualsArg(argv[i], L"--dump")) {
      if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-') dumpPath = argv[++i];
      else dumpPath = L"dump.txt";
    } else if (EqualsArg(argv[i], L"/icon-preview") || EqualsArg(argv[i], L"--icon-preview")) {
      if (i + 1 < argc && argv[i + 1][0] != L'/' && argv[i + 1][0] != L'-') iconPreview = argv[++i];
      else iconPreview = L"brand_preview.bmp";
    }
  }
  if (argv) LocalFree(argv);

  if (exitRequested) {
    HWND window = FindWindowW(kWindowClass, nullptr);
    if (window) PostMessageW(window, WM_APP_EXIT, 0, 0);
    return 0;
  }

  if (!iconPreview.empty()) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool wrote = WriteBrandPreview(iconPreview, 256);
    if (SUCCEEDED(com)) CoUninitialize();
    return wrote ? 0 : 1;
  }

  if (!dumpPath.empty()) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const LoadResult loaded = LoadGroups();
    const bool wrote = loaded.error.empty() && WriteDump(dumpPath, loaded.groups);
    if (SUCCEEDED(com)) CoUninitialize();
    return wrote ? 0 : 1;
  }

  if (!IsWindows11()) {
    MessageBoxW(nullptr, L"TidyTray 只能在 Windows 11 上整理通知区图标。", kAppName, MB_OK | MB_ICONINFORMATION);
    return 1;
  }

  HANDLE mutex = CreateMutexW(nullptr, FALSE, kMutexName);
  if (!mutex) return 1;
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    if (!silent) {
      HWND window = FindWindowW(kWindowClass, nullptr);
      if (window) PostMessageW(window, WM_APP_SHOW, 0, 0);
    }
    CloseHandle(mutex);
    return 0;
  }

  const int code = RunApp(instance, !silent);
  CloseHandle(mutex);
  return code;
}
