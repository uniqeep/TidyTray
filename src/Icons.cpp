#include "Icons.h"

#include "Brand.h"

#include <knownfolders.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <tlhelp32.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cwchar>
#include <map>
#include <optional>
#include <vector>

namespace {

constexpr wchar_t kNotifyKey[] = L"Control Panel\\NotifyIconSettings";
bool EnsureFactory();

template <typename T>
struct ComPtr {
  T* pointer = nullptr;
  ~ComPtr() {
    if (pointer) pointer->Release();
  }
  T** Put() { return &pointer; }
  T* operator->() const { return pointer; }
  explicit operator bool() const { return pointer != nullptr; }
};

IWICImagingFactory* g_factory = nullptr;

std::wstring ReadString(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return {};
  if ((type != REG_SZ && type != REG_EXPAND_SZ) || bytes < sizeof(wchar_t) || bytes > 1024 * 1024) return {};
  std::wstring value(bytes / sizeof(wchar_t), L'\0');
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &bytes) != ERROR_SUCCESS) return {};
  const auto end = value.find(L'\0');
  if (end != std::wstring::npos) value.resize(end);
  if (type == REG_EXPAND_SZ) {
    std::wstring expanded(32768, L'\0');
    const DWORD count = ExpandEnvironmentStringsW(value.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (count > 0 && count < expanded.size()) {
      expanded.resize(count - 1);
      return expanded;
    }
  }
  return value;
}

std::optional<std::uint32_t> ReadDword(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = sizeof(DWORD);
  DWORD value = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(&value), &bytes) != ERROR_SUCCESS) return std::nullopt;
  if (type != REG_DWORD) return std::nullopt;
  return value;
}

std::vector<std::uint8_t> ReadBinary(HKEY key, const wchar_t* name) {
  DWORD type = 0;
  DWORD bytes = 0;
  if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) != ERROR_SUCCESS) return {};
  if (type != REG_BINARY || bytes == 0 || bytes > 2 * 1024 * 1024) return {};
  std::vector<std::uint8_t> data(bytes);
  if (RegQueryValueExW(key, name, nullptr, &type, data.data(), &bytes) != ERROR_SUCCESS) return {};
  data.resize(bytes);
  return data;
}

std::wstring ExpandPath(std::wstring path) {
  if (path.find(L'%') != std::wstring::npos) {
    std::wstring expanded(32768, L'\0');
    const DWORD count = ExpandEnvironmentStringsW(path.c_str(), expanded.data(), static_cast<DWORD>(expanded.size()));
    if (count > 0 && count < expanded.size()) {
      expanded.resize(count - 1);
      path = std::move(expanded);
    }
  }
  if (path.size() < 3 || path[0] != L'{') return path;
  const auto end = path.find(L'}');
  if (end == std::wstring::npos) return path;
  GUID guid{};
  if (FAILED(CLSIDFromString(path.substr(0, end + 1).c_str(), &guid))) return path;
  PWSTR known = nullptr;
  if (FAILED(SHGetKnownFolderPath(guid, 0, nullptr, &known)) || known == nullptr) return path;
  std::wstring resolved = known;
  CoTaskMemFree(known);
  resolved += path.substr(end + 1);
  return resolved;
}

std::wstring FileNameOf(const std::wstring& path) {
  const auto pos = path.find_last_of(L"\\/");
  if (pos == std::wstring::npos) return path;
  return path.substr(pos + 1);
}

bool FileExists(const std::wstring& path) {
  if (path.empty()) return false;
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

struct VersionStrings {
  std::wstring product;
  std::wstring description;
};

VersionStrings ReadVersion(const std::wstring& path) {
  DWORD ignored = 0;
  const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
  if (bytes == 0) return {};
  std::vector<BYTE> data(bytes);
  if (!GetFileVersionInfoW(path.c_str(), 0, bytes, data.data())) return {};

  struct Translation {
    WORD language;
    WORD codePage;
  };
  Translation* translations = nullptr;
  UINT size = 0;
  VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&translations), &size);
  const UINT count = size / sizeof(Translation);

  auto query = [&](WORD language, WORD codePage, const wchar_t* field) -> std::wstring {
    wchar_t hex[16];
    swprintf(hex, 16, L"%04x%04x", language, codePage);
    const std::wstring block = std::wstring(L"\\StringFileInfo\\") + hex + L"\\" + field;
    void* value = nullptr;
    UINT valueSize = 0;
    if (!VerQueryValueW(data.data(), block.c_str(), &value, &valueSize) || value == nullptr || valueSize == 0) return {};
    return Trim(std::wstring(static_cast<wchar_t*>(value)));
  };

  VersionStrings best;
  for (UINT i = 0; i < count; ++i) {
    const std::wstring product = query(translations[i].language, translations[i].codePage, L"ProductName");
    const std::wstring description = query(translations[i].language, translations[i].codePage, L"FileDescription");
    if (!product.empty() || !description.empty()) return {product, description};
  }
  const WORD fallbacks[][2] = {{0x0804, 1200}, {0x0409, 1200}, {0x0409, 1252}};
  for (const auto& item : fallbacks) {
    best.product = query(item[0], item[1], L"ProductName");
    best.description = query(item[0], item[1], L"FileDescription");
    if (!best.product.empty() || !best.description.empty()) return best;
  }
  return {};
}

std::wstring LettersOnly(std::wstring text) {
  std::wstring out;
  out.reserve(text.size());
  for (wchar_t ch : ToLower(std::move(text))) {
    if (ch == L'®' || ch == L'™' || ch == L'©' || ch == L' ') continue;
    out.push_back(ch);
  }
  return out;
}

bool IsGenericName(const std::wstring& text) {
  const std::wstring lower = ToLower(Trim(text));
  const std::wstring plain = LettersOnly(lower);
  if (lower.empty() || lower == L"windows explorer" || lower == L"explorer" || lower == L"microsoft windows" ||
      lower == L"windows operating system" || lower == L"windows 资源管理器") {
    return true;
  }
  if (plain.find(L"microsoftwindows") != std::wstring::npos &&
      (plain.find(L"operatingsystem") != std::wstring::npos || plain.find(L"操作系统") != std::wstring::npos)) {
    return true;
  }
  if (lower.find(L"rundll32") != std::wstring::npos &&
      (lower.find(L"主进程") != std::wstring::npos || lower.find(L"host process") != std::wstring::npos)) {
    return true;
  }
  return false;
}

std::wstring CleanDisplay(std::wstring text) {
  text = Trim(std::move(text));
  while (!text.empty() && (text.back() == L':' || text.back() == L'：')) text.pop_back();
  return Trim(std::move(text));
}

const wchar_t* KnownIconName(const std::wstring& guid) {
  const std::wstring id = ToLower(guid);
  if (id == L"{6da68f06-00f1-4e6e-8158-7f1ffd4f9db9}") return L"蓝牙";
  if (id == L"{16d5471a-2359-44b0-9585-e95e189dbb3f}") return L"打印机";
  if (id == L"{7820ae78-23e3-4229-82c1-e41cb67d5b9c}") return L"安全删除硬件";
  if (id == L"{7820ae81-23e3-4229-82c1-e41cb67d5b9c}") return L"Windows 更新";
  if (id == L"{ba82e2dc-f405-47ad-b032-cf0faa0e3933}") return L"Windows 安全中心";
  return nullptr;
}

bool HasCjk(const std::wstring& text) {
  for (wchar_t ch : text) {
    if (ch >= 0x4E00 && ch <= 0x9FFF) return true;
  }
  return false;
}

std::wstring DisplayName(const Member& member) {
  const std::wstring tooltip = CleanDisplay(FirstLine(member.tooltip));
  const std::wstring product = CleanDisplay(member.product);
  const std::wstring description = CleanDisplay(member.fileDescription);
  if (!IsGenericName(tooltip) && tooltip.size() <= 24) return Truncate(tooltip, 80);
  if (!IsGenericName(description) && description.size() <= 24 &&
      (IsGenericName(product) || (HasCjk(description) && !HasCjk(product)) ||
       (ContainsIgnoreCase(description, product) && description.size() <= product.size() + 12))) {
    return Truncate(description, 80);
  }
  if (!IsGenericName(product)) return Truncate(product, 80);
  if (const wchar_t* known = KnownIconName(member.guid)) return known;
  if (member.hasUid && member.uid == 1124 && EqualsIgnoreCase(member.fileName, L"explorer.exe")) return L"安全删除硬件";
  if (member.hasUid && member.uid == 1124 && EqualsIgnoreCase(member.fileName, L"rundll32.exe")) return L"可移动硬件";
  if (!IsGenericName(description)) return Truncate(description, 80);
  if (!IsGenericName(tooltip)) return Truncate(tooltip, 80);
  if (!member.fileName.empty()) return member.fileName;
  if (!member.guid.empty()) return L"系统图标";
  return L"未命名图标";
}

int MemberScore(const Member& member) {
  int score = 0;
  if (member.exists) score += 8;
  if (member.place == Place::Taskbar) score += 4;
  else if (member.place == Place::Overflow) score += 2;
  if (!FirstLine(member.tooltip).empty()) score += 2;
  if (!member.snapshot.empty()) score += 1;
  return score;
}

bool ParseGuid(const std::wstring& text, GUID* out) {
  if (!out || text.empty()) return false;
  return SUCCEEDED(CLSIDFromString(text.c_str(), out));
}

std::wstring ProcessImage(DWORD pid) {
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (!process) return {};
  std::wstring path(32768, L'\0');
  DWORD size = static_cast<DWORD>(path.size());
  const BOOL ok = QueryFullProcessImageNameW(process, 0, path.data(), &size);
  CloseHandle(process);
  if (!ok) return {};
  path.resize(size);
  return path;
}

void CollectWindows(DWORD pid, std::vector<HWND>* windows) {
  if (!windows) return;
  struct Pack {
    DWORD pid;
    std::vector<HWND>* windows;
  } pack{pid, windows};
  EnumWindows(
      [](HWND hwnd, LPARAM param) -> BOOL {
        auto* pack = reinterpret_cast<Pack*>(param);
        DWORD owner = 0;
        GetWindowThreadProcessId(hwnd, &owner);
        if (owner == pack->pid) pack->windows->push_back(hwnd);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&pack));

  HWND hwnd = nullptr;
  while ((hwnd = FindWindowExW(HWND_MESSAGE, hwnd, nullptr, nullptr)) != nullptr) {
    DWORD owner = 0;
    GetWindowThreadProcessId(hwnd, &owner);
    if (owner == pid) windows->push_back(hwnd);
  }
}

std::vector<DWORD> FindPids(const std::wstring& expandedPath, const std::wstring& fileName) {
  std::vector<DWORD> exact;
  std::vector<DWORD> byName;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap == INVALID_HANDLE_VALUE) return {};
  PROCESSENTRY32W entry{};
  entry.dwSize = sizeof(entry);
  if (Process32FirstW(snap, &entry)) {
    do {
      if (!EqualsIgnoreCase(entry.szExeFile, fileName)) continue;
      byName.push_back(entry.th32ProcessID);
      if (!expandedPath.empty()) {
        const std::wstring image = ProcessImage(entry.th32ProcessID);
        if (!image.empty() && EqualsIgnoreCase(image, expandedPath)) exact.push_back(entry.th32ProcessID);
      }
    } while (Process32NextW(snap, &entry));
  }
  CloseHandle(snap);
  return exact.empty() ? byName : exact;
}

bool HasScreenRect(HRESULT hr, const RECT* rect) {
  return hr == S_OK && rect && rect->right > rect->left && rect->bottom > rect->top;
}

bool RectForGuid(const GUID& guid, RECT* rect) {
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof(id);
  id.guidItem = guid;
  return HasScreenRect(Shell_NotifyIconGetRect(&id, rect), rect);
}

bool RectForUid(HWND hwnd, UINT uid, RECT* rect) {
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof(id);
  id.hWnd = hwnd;
  id.uID = uid;
  return HasScreenRect(Shell_NotifyIconGetRect(&id, rect), rect);
}

bool SetGuidState(const GUID& guid, bool hidden) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.uFlags = NIF_GUID | NIF_STATE;
  data.guidItem = guid;
  data.dwState = hidden ? NIS_HIDDEN : 0;
  data.dwStateMask = NIS_HIDDEN;
  return Shell_NotifyIconW(NIM_MODIFY, &data) == TRUE;
}

bool SetUidState(HWND hwnd, UINT uid, bool hidden) {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = hwnd;
  data.uID = uid;
  data.uFlags = NIF_STATE;
  data.dwState = hidden ? NIS_HIDDEN : 0;
  data.dwStateMask = NIS_HIDDEN;
  return Shell_NotifyIconW(NIM_MODIFY, &data) == TRUE;
}

bool LooksLikeNvidiaHost(const Member& member) {
  return ContainsIgnoreCase(member.fileName, L"nvcontainer") || ContainsIgnoreCase(member.fileName, L"nvdisplay") ||
         ContainsIgnoreCase(member.product, L"nvidia");
}

void OwnerWindows(const Member& member, std::vector<HWND>* windows) {
  if (!windows) return;
  if (!member.fileName.empty()) {
    for (DWORD pid : FindPids(member.expandedPath, member.fileName)) CollectWindows(pid, windows);
  }
  // NVIDIA Settings may be hosted by either container executable.
  if (LooksLikeNvidiaHost(member)) {
    for (DWORD pid : FindPids(L"", L"nvcontainer.exe")) CollectWindows(pid, windows);
    for (DWORD pid : FindPids(L"", L"NVDisplay.Container.exe")) CollectWindows(pid, windows);
  }
}

void ApplyState(const Member& member, bool hidden) {
  GUID guid{};
  if (ParseGuid(member.guid, &guid)) SetGuidState(guid, hidden);
  if (!member.hasUid) return;
  std::vector<HWND> windows;
  OwnerWindows(member, &windows);
  for (HWND hwnd : windows) SetUidState(hwnd, member.uid, hidden);
}

void NudgeOwners(const Member& member) {
  const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
  if (message == 0) return;
  std::vector<HWND> windows;
  OwnerWindows(member, &windows);
  for (HWND hwnd : windows) PostMessageW(hwnd, message, 0, 0);
}

void ProbeLive(std::vector<Group>& groups) {
  std::map<DWORD, std::wstring> pidFile;
  HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
      do {
        pidFile.emplace(entry.th32ProcessID, ToLower(entry.szExeFile));
      } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
  }

  std::map<std::wstring, std::vector<HWND>> windowsByFile;
  struct Pack {
    const std::map<DWORD, std::wstring>* pidFile;
    std::map<std::wstring, std::vector<HWND>>* windowsByFile;
  } pack{&pidFile, &windowsByFile};
  auto add = [](HWND hwnd, LPARAM param) -> BOOL {
    auto* pack = reinterpret_cast<Pack*>(param);
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    const auto found = pack->pidFile->find(pid);
    if (found != pack->pidFile->end()) pack->windowsByFile->operator[](found->second).push_back(hwnd);
    return TRUE;
  };
  EnumWindows(add, reinterpret_cast<LPARAM>(&pack));
  for (HWND hwnd = nullptr; (hwnd = FindWindowExW(HWND_MESSAGE, hwnd, nullptr, nullptr)) != nullptr;) add(hwnd, reinterpret_cast<LPARAM>(&pack));

  for (Group& group : groups) {
    group.anyVisible = false;
    group.anyOnTaskbar = false;
    for (Member& member : group.members) {
      const Place recorded = member.place;
      bool live = false;
      RECT rect{};
      GUID guid{};
      if (ParseGuid(member.guid, &guid) && RectForGuid(guid, &rect)) live = true;
      if (!live && member.hasUid) {
        const auto windows = windowsByFile.find(ToLower(member.fileName));
        if (windows != windowsByFile.end()) {
          for (HWND hwnd : windows->second) {
            if (RectForUid(hwnd, member.uid, &rect)) {
              live = true;
              break;
            }
          }
        }
      }
      if (!live) continue;
      if (recorded != Place::Taskbar) member.place = Place::Overflow;
      group.anyVisible = true;
      if (member.place == Place::Taskbar) group.anyOnTaskbar = true;
    }
  }
}

enum class IconPresence { Missing, Hidden, Shown };

IconPresence PresenceFrom(HRESULT hr, const RECT& rect) {
  if (HasScreenRect(hr, &rect)) return IconPresence::Shown;
  if (hr == S_FALSE) return IconPresence::Hidden;
  return IconPresence::Missing;
}

HRESULT ReadGuidRect(const GUID& guid, RECT* rect) {
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof(id);
  id.guidItem = guid;
  return Shell_NotifyIconGetRect(&id, rect);
}

HRESULT ReadUidRect(HWND hwnd, UINT uid, RECT* rect) {
  NOTIFYICONIDENTIFIER id{};
  id.cbSize = sizeof(id);
  id.hWnd = hwnd;
  id.uID = uid;
  return Shell_NotifyIconGetRect(&id, rect);
}

IconPresence MemberPresence(const Member& member) {
  IconPresence presence = IconPresence::Missing;
  auto note = [&](HRESULT hr, const RECT& rect) {
    const IconPresence next = PresenceFrom(hr, rect);
    if (next == IconPresence::Shown) presence = IconPresence::Shown;
    else if (next == IconPresence::Hidden && presence == IconPresence::Missing) presence = IconPresence::Hidden;
  };
  RECT rect{};
  GUID guid{};
  if (ParseGuid(member.guid, &guid)) note(ReadGuidRect(guid, &rect), rect);
  if (presence == IconPresence::Shown || !member.hasUid || member.fileName.empty()) return presence;
  for (DWORD pid : FindPids(member.expandedPath, member.fileName)) {
    std::vector<HWND> windows;
    CollectWindows(pid, &windows);
    for (HWND hwnd : windows) {
      note(ReadUidRect(hwnd, member.uid, &rect), rect);
      if (presence == IconPresence::Shown) return presence;
    }
  }
  return presence;
}

IconPresence GroupPresence(const Group& group) {
  IconPresence presence = IconPresence::Missing;
  for (const Member& member : group.members) {
    const IconPresence next = MemberPresence(member);
    if (next == IconPresence::Shown) return next;
    if (next == IconPresence::Hidden) presence = next;
  }
  return presence;
}

bool MemberVisible(const Member& member, RECT* rect) {
  GUID guid{};
  if (ParseGuid(member.guid, &guid) && RectForGuid(guid, rect)) return true;
  if (!member.hasUid || member.fileName.empty()) return false;
  for (DWORD pid : FindPids(member.expandedPath, member.fileName)) {
    std::vector<HWND> windows;
    CollectWindows(pid, &windows);
    for (HWND hwnd : windows) {
      if (RectForUid(hwnd, member.uid, rect)) return true;
    }
  }
  return false;
}

Rule RuleFrom(const Member& member) {
  Rule rule;
  rule.file = ToLower(member.fileName);
  rule.product = IsGenericName(member.product) ? std::wstring() : ToLower(member.product);
  rule.guid = ToLower(member.guid);
  rule.uid = member.uid;
  rule.hasUid = member.hasUid;
  rule.name = DisplayName(member);
  return rule;
}

void Finalize(Group& group) {
  const Member* best = &group.members.front();
  int bestScore = -1;
  for (const Member& member : group.members) {
    const int score = MemberScore(member);
    if (score > bestScore) {
      bestScore = score;
      best = &member;
    }
  }

  group.id.name = DisplayName(*best);
  group.fileDisplay = best->fileName.empty() ? group.id.file : best->fileName;
  group.iconPath.clear();
  group.snapshot.clear();
  for (const Member& member : group.members) {
    if (group.id.product.empty() && !IsGenericName(member.product)) group.id.product = ToLower(member.product);
    if (group.snapshot.empty() && !member.snapshot.empty() && member.exists) group.snapshot = member.snapshot;
    if (group.iconPath.empty() && member.exists) group.iconPath = member.expandedPath;
  }
  if (group.snapshot.empty()) {
    for (const Member& member : group.members) {
      if (!member.snapshot.empty()) {
        group.snapshot = member.snapshot;
        break;
      }
    }
  }
  if (group.iconPath.empty()) {
    for (const Member& member : group.members) {
      if (!member.expandedPath.empty()) {
        group.iconPath = member.expandedPath;
        break;
      }
    }
  }

  std::vector<std::wstring> paths;
  auto add = [&](const std::wstring& path) {
    if (path.empty()) return;
    for (const std::wstring& existing : paths) {
      if (EqualsIgnoreCase(existing, path)) return;
    }
    paths.push_back(path);
  };
  for (const Member& member : group.members) {
    if (member.exists) add(member.expandedPath);
  }
  for (const Member& member : group.members) {
    if (!member.exists) add(member.expandedPath.empty() ? member.rawPath : member.expandedPath);
  }
  if (!paths.empty()) {
    group.pathShown = paths[0];
    if (paths.size() > 1) group.pathShown += L"  (+" + std::to_wstring(paths.size() - 1) + L")";
  }
  for (Member& member : group.members) member.snapshot.clear();
  RefreshGroupFlags(group);
}

void Disambiguate(std::vector<Group>& groups) {
  std::map<std::wstring, std::vector<size_t>> buckets;
  for (size_t i = 0; i < groups.size(); ++i) {
    buckets[ToLower(groups[i].id.name)].push_back(i);
  }
  for (const auto& entry : buckets) {
    if (entry.second.size() < 2) continue;
    int number = 1;
    for (size_t index : entry.second) {
      if (number > 1) groups[index].id.name += L" (" + std::to_wstring(number) + L")";
      ++number;
    }
  }
}

bool LessGroup(const Group& left, const Group& right) {
  const int name = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, left.id.name.c_str(), -1, right.id.name.c_str(), -1);
  if (name == CSTR_LESS_THAN) return true;
  if (name == CSTR_GREATER_THAN) return false;
  const int file = CompareStringW(LOCALE_USER_DEFAULT, NORM_IGNORECASE, left.fileDisplay.c_str(), -1, right.fileDisplay.c_str(), -1);
  if (file == CSTR_LESS_THAN) return true;
  if (file == CSTR_GREATER_THAN) return false;
  if (left.id.uid != right.id.uid) return left.id.uid < right.id.uid;
  return CompareStringOrdinal(left.id.guid.c_str(), -1, right.id.guid.c_str(), -1, TRUE) == CSTR_LESS_THAN;
}

float Clampf(float value, float low, float high) {
  if (value < low) return low;
  if (value > high) return high;
  return value;
}

float RoundRectAlpha(float px, float py, float size, float radius) {
  const float half = size * 0.5f;
  float qx = std::fabs(px - half) - (half - radius);
  float qy = std::fabs(py - half) - (half - radius);
  const float ox = qx > 0.0f ? qx : 0.0f;
  const float oy = qy > 0.0f ? qy : 0.0f;
  const float inside = (qx > qy ? qx : qy) < 0.0f ? (qx > qy ? qx : qy) : 0.0f;
  const float distance = std::sqrt(ox * ox + oy * oy) + inside - radius;
  return Clampf(0.5f - distance, 0.0f, 1.0f);
}

void SoftRoundPixels(int size, std::vector<std::uint32_t>* pixels) {
  if (!pixels || size <= 0) return;
  const float radius = static_cast<float>(size) * 0.22f;
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float cover = RoundRectAlpha(x + 0.5f, y + 0.5f, static_cast<float>(size), radius);
      auto& pixel = (*pixels)[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)];
      const std::uint32_t src = pixel;
      const int alpha = static_cast<int>(((src >> 24) & 0xFF) * cover + 0.5f);
      if (alpha <= 0) {
        pixel = 0;
        continue;
      }
      const int red = static_cast<int>(((src >> 16) & 0xFF) * cover + 0.5f);
      const int green = static_cast<int>(((src >> 8) & 0xFF) * cover + 0.5f);
      const int blue = static_cast<int>((src & 0xFF) * cover + 0.5f);
      pixel = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(red) << 16) |
              (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
    }
  }
}

HICON IconFromPixels(int size, const std::uint32_t* pixels) {
  ICONINFO info{};
  info.fIcon = TRUE;
  info.hbmMask = CreateBitmap(size, size, 1, 1, nullptr);

  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;

  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  info.hbmColor = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  HICON icon = nullptr;
  if (info.hbmMask && info.hbmColor && bits) {
    memcpy(bits, pixels, static_cast<size_t>(size) * static_cast<size_t>(size) * 4);
    icon = CreateIconIndirect(&info);
  }
  if (info.hbmMask) DeleteObject(info.hbmMask);
  if (info.hbmColor) DeleteObject(info.hbmColor);
  return icon;
}

bool PixelsFromIcon(HICON icon, int size, std::vector<std::uint32_t>* pixels) {
  if (!icon || size <= 0 || !pixels) return false;
  pixels->assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = -size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  void* bits = nullptr;
  HDC screen = GetDC(nullptr);
  HDC memory = CreateCompatibleDC(screen);
  HBITMAP bitmap = CreateDIBSection(screen, reinterpret_cast<BITMAPINFO*>(&header), DIB_RGB_COLORS, &bits, nullptr, 0);
  ReleaseDC(nullptr, screen);
  if (!memory || !bitmap || !bits) {
    if (memory) DeleteDC(memory);
    if (bitmap) DeleteObject(bitmap);
    return false;
  }
  HGDIOBJ old = SelectObject(memory, bitmap);
  SetBkMode(memory, TRANSPARENT);
  DrawIconEx(memory, 0, 0, icon, size, size, 0, nullptr, DI_NORMAL);
  SelectObject(memory, old);
  DeleteDC(memory);
  memcpy(pixels->data(), bits, pixels->size() * 4);
  DeleteObject(bitmap);

  bool hasAlpha = false;
  for (std::uint32_t pixel : *pixels) {
    if ((pixel >> 24) != 0) {
      hasAlpha = true;
      break;
    }
  }
  if (!hasAlpha) {
    for (std::uint32_t& pixel : *pixels) {
      if ((pixel & 0x00FFFFFF) == 0) pixel = 0;
      else pixel |= 0xFF000000u;
    }
  } else {
    for (std::uint32_t& pixel : *pixels) {
      const int alpha = static_cast<int>((pixel >> 24) & 0xFF);
      if (alpha == 0 || alpha == 255) continue;
      int red = (pixel >> 16) & 0xFF;
      int green = (pixel >> 8) & 0xFF;
      int blue = pixel & 0xFF;
      red = red * alpha / 255;
      green = green * alpha / 255;
      blue = blue * alpha / 255;
      pixel = (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(red) << 16) |
              (static_cast<std::uint32_t>(green) << 8) | static_cast<std::uint32_t>(blue);
    }
  }
  return true;
}

HICON SoftRoundIcon(HICON source, int size) {
  std::vector<std::uint32_t> pixels;
  if (!PixelsFromIcon(source, size, &pixels)) return nullptr;
  SoftRoundPixels(size, &pixels);
  return IconFromPixels(size, pixels.data());
}

bool EnsureFactory() {
  if (g_factory) return true;
  const HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_IWICImagingFactory,
                                 reinterpret_cast<void**>(&g_factory));
  return SUCCEEDED(hr) && g_factory != nullptr;
}

HICON IconFromPng(const std::vector<std::uint8_t>& png, int size, bool softRound) {
  if (png.size() < 8 || size <= 0 || !EnsureFactory()) return nullptr;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(g_factory->CreateStream(stream.Put()))) return nullptr;
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))) return nullptr;
  if (FAILED(g_factory->CreateDecoderFromStream(stream.pointer, nullptr, WICDecodeMetadataCacheOnLoad, decoder.Put()))) return nullptr;
  if (FAILED(decoder->GetFrame(0, frame.Put()))) return nullptr;
  if (FAILED(g_factory->CreateBitmapScaler(scaler.Put()))) return nullptr;
  if (FAILED(scaler->Initialize(frame.pointer, static_cast<UINT>(size), static_cast<UINT>(size), WICBitmapInterpolationModeFant))) {
    return nullptr;
  }
  if (FAILED(g_factory->CreateFormatConverter(converter.Put()))) return nullptr;
  if (FAILED(converter->Initialize(scaler.pointer, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }

  std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
  const UINT stride = static_cast<UINT>(size) * 4;
  if (FAILED(converter->CopyPixels(nullptr, stride, stride * static_cast<UINT>(size), reinterpret_cast<BYTE*>(pixels.data())))) return nullptr;
  if (softRound) SoftRoundPixels(size, &pixels);
  return IconFromPixels(size, pixels.data());
}

HICON IconFromPng(const std::vector<std::uint8_t>& png, int size) { return IconFromPng(png, size, true); }

void KnockoutNearBlack(std::vector<std::uint32_t>* pixels) {
  if (!pixels) return;
  for (std::uint32_t& pixel : *pixels) {
    const int alpha = static_cast<int>((pixel >> 24) & 0xFF);
    const int red = static_cast<int>((pixel >> 16) & 0xFF);
    const int green = static_cast<int>((pixel >> 8) & 0xFF);
    const int blue = static_cast<int>(pixel & 0xFF);
    if (alpha == 0) continue;
    if (red <= 16 && green <= 16 && blue <= 16) pixel = 0;
  }
}

std::vector<std::uint8_t> ReadFileBytes(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8 * 1024 * 1024) {
    CloseHandle(file);
    return {};
  }
  std::vector<std::uint8_t> data(static_cast<size_t>(size.QuadPart));
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok) return {};
  data.resize(read);
  return data;
}

std::vector<std::uint8_t> LoadTrayPngBytes() {
  HMODULE module = GetModuleHandleW(nullptr);
  HRSRC resource = FindResourceW(module, MAKEINTRESOURCEW(201), RT_RCDATA);
  if (resource) {
    HGLOBAL global = LoadResource(module, resource);
    const DWORD bytes = SizeofResource(module, resource);
    const void* data = global ? LockResource(global) : nullptr;
    if (data && bytes > 0) {
      const auto* begin = static_cast<const std::uint8_t*>(data);
      return std::vector<std::uint8_t>(begin, begin + bytes);
    }
  }

  std::wstring modulePath(MAX_PATH, L'\0');
  const DWORD count = GetModuleFileNameW(nullptr, modulePath.data(), static_cast<DWORD>(modulePath.size()));
  modulePath.resize(count);
  const auto slash = modulePath.find_last_of(L"\\/");
  const std::wstring directory = slash == std::wstring::npos ? std::wstring() : modulePath.substr(0, slash + 1);
  const std::wstring candidates[] = {
      directory + L"Tray.png",
      directory + L"..\\src\\Tray.png",
      directory + L"..\\..\\src\\Tray.png",
  };
  for (const std::wstring& path : candidates) {
    auto bytes = ReadFileBytes(path);
    if (!bytes.empty()) return bytes;
  }
  return {};
}

HICON IconFromTrayPng(int size) {
  const auto png = LoadTrayPngBytes();
  if (png.empty() || size <= 0 || !EnsureFactory()) return nullptr;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICBitmapFrameDecode> frame;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(g_factory->CreateStream(stream.Put()))) return nullptr;
  if (FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())))) return nullptr;
  if (FAILED(g_factory->CreateDecoderFromStream(stream.pointer, nullptr, WICDecodeMetadataCacheOnLoad, decoder.Put()))) return nullptr;
  if (FAILED(decoder->GetFrame(0, frame.Put()))) return nullptr;
  if (FAILED(g_factory->CreateBitmapScaler(scaler.Put()))) return nullptr;
  if (FAILED(scaler->Initialize(frame.pointer, static_cast<UINT>(size), static_cast<UINT>(size), WICBitmapInterpolationModeFant))) {
    return nullptr;
  }
  if (FAILED(g_factory->CreateFormatConverter(converter.Put()))) return nullptr;
  if (FAILED(converter->Initialize(scaler.pointer, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }
  std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
  const UINT stride = static_cast<UINT>(size) * 4;
  if (FAILED(converter->CopyPixels(nullptr, stride, stride * static_cast<UINT>(size), reinterpret_cast<BYTE*>(pixels.data())))) return nullptr;
  KnockoutNearBlack(&pixels);
  return IconFromPixels(size, pixels.data());
}

HICON IconFromFile(const std::wstring& path, const std::wstring& fileName, int size) {
  HICON raw = nullptr;
  if (!path.empty() && FileExists(path)) {
    SHDefExtractIconW(path.c_str(), 0, 0, &raw, nullptr, static_cast<UINT>(size));
  }
  if (!raw) {
    SHFILEINFOW info{};
    const std::wstring probe = !fileName.empty() ? fileName : L"app.exe";
    const UINT flags = size > 20 ? (SHGFI_ICON | SHGFI_LARGEICON | SHGFI_USEFILEATTRIBUTES)
                                 : (SHGFI_ICON | SHGFI_SMALLICON | SHGFI_USEFILEATTRIBUTES);
    if (SHGetFileInfoW(probe.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), flags)) raw = info.hIcon;
  }
  if (!raw) return nullptr;
  HICON rounded = SoftRoundIcon(raw, size);
  DestroyIcon(raw);
  return rounded ? rounded : nullptr;
}

std::uint32_t Premultiply(int red, int green, int blue, int alpha) {
  return (static_cast<std::uint32_t>(alpha) << 24) | (static_cast<std::uint32_t>(red * alpha / 255) << 16) |
         (static_cast<std::uint32_t>(green * alpha / 255) << 8) | static_cast<std::uint32_t>(blue * alpha / 255);
}

void BroadcastTaskbarCreated() {
  const UINT message = RegisterWindowMessageW(L"TaskbarCreated");
  if (message != 0) SendNotifyMessageW(HWND_BROADCAST, message, 0, 0);
}

}  // namespace

LoadResult LoadGroups() {
  LoadResult result;
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyKey, 0, KEY_READ, &root) != ERROR_SUCCESS) {
    result.error = L"找不到通知区设置。TidyTray 需要 Windows 11。";
    return result;
  }

  std::map<std::wstring, VersionStrings> versions;
  std::vector<Member> members;
  for (DWORD index = 0;; ++index) {
    wchar_t name[256];
    DWORD nameLength = 256;
    const LONG enumerated = RegEnumKeyExW(root, index, name, &nameLength, nullptr, nullptr, nullptr, nullptr);
    if (enumerated == ERROR_NO_MORE_ITEMS) break;
    if (enumerated != ERROR_SUCCESS) continue;

    HKEY sub = nullptr;
    if (RegOpenKeyExW(root, name, 0, KEY_READ, &sub) != ERROR_SUCCESS) continue;
    Member member;
    member.subkey = name;
    member.rawPath = ReadString(sub, L"ExecutablePath");
    member.expandedPath = ExpandPath(member.rawPath);
    member.fileName = FileNameOf(member.expandedPath.empty() ? member.rawPath : member.expandedPath);
    member.tooltip = ReadString(sub, L"InitialTooltip");
    member.publisher = ReadString(sub, L"Publisher");
    member.guid = ReadString(sub, L"IconGuid");
    member.snapshot = ReadBinary(sub, L"IconSnapshot");
    if (const auto uid = ReadDword(sub, L"UID")) {
      member.uid = *uid;
      member.hasUid = true;
    }
    if (const auto promoted = ReadDword(sub, L"IsPromoted")) {
      member.place = *promoted == 0 ? Place::Overflow : Place::Taskbar;
    } else {
      member.place = Place::Overflow;
    }
    member.exists = FileExists(member.expandedPath);
    if (member.exists) {
      auto found = versions.find(member.expandedPath);
      if (found == versions.end()) found = versions.emplace(member.expandedPath, ReadVersion(member.expandedPath)).first;
      member.product = found->second.product;
      member.fileDescription = found->second.description;
    }
    RegCloseKey(sub);
    if (member.rawPath.empty() && member.guid.empty()) continue;
    members.push_back(std::move(member));
  }
  RegCloseKey(root);

  for (Member& member : members) {
    Rule id = RuleFrom(member);
    Group* destination = nullptr;
    for (Group& group : result.groups) {
      if (!SameApp(group.id, id)) continue;
      destination = &group;
      break;
    }
    if (!destination) {
      Group group;
      group.id = std::move(id);
      group.members.push_back(std::move(member));
      result.groups.push_back(std::move(group));
      continue;
    }
    if (destination->id.product.empty() && !id.product.empty()) destination->id.product = id.product;
    if (destination->id.guid.empty() && !id.guid.empty()) destination->id.guid = id.guid;
    if (!destination->id.hasUid && id.hasUid) {
      destination->id.hasUid = true;
      destination->id.uid = id.uid;
    }
    if (destination->id.file.empty() && !id.file.empty()) destination->id.file = id.file;
    destination->members.push_back(std::move(member));
  }

  for (Group& group : result.groups) Finalize(group);
  ProbeLive(result.groups);
  std::stable_sort(result.groups.begin(), result.groups.end(), LessGroup);
  Disambiguate(result.groups);
  return result;
}

bool IsSelfPath(const std::wstring& path) {
  std::wstring self(32768, L'\0');
  const DWORD count = GetModuleFileNameW(nullptr, self.data(), static_cast<DWORD>(self.size()));
  self.resize(count);
  return !path.empty() && EqualsIgnoreCase(path, self);
}

bool IconIsVisible(const Member& member) {
  RECT rect{};
  return MemberVisible(member, &rect);
}

bool SuppressIcon(const Member& member) {
  if (IsSelfPath(member.expandedPath)) return false;
  // Never Sleep on the UI thread — ApplyState is enough; sweep will re-apply if needed.
  ApplyState(member, true);
  return true;
}

bool SuppressGroup(const Group& group) {
  if (IsSelfPath(group.iconPath) || EqualsIgnoreCase(group.fileDisplay, kAppExe) || EqualsIgnoreCase(group.fileDisplay, kLegacyExe)) {
    return false;
  }
  bool ok = true;
  for (const Member& member : group.members) {
    if (!SuppressIcon(member)) ok = false;
  }
  return ok;
}

bool WritePromoted(const Member& member, bool taskbar) {
  if (member.subkey.empty()) return false;
  HKEY root = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_SET_VALUE, &root) != ERROR_SUCCESS) {
    return false;
  }
  HKEY sub = nullptr;
  const LONG open = RegOpenKeyExW(root, member.subkey.c_str(), 0, KEY_SET_VALUE, &sub);
  RegCloseKey(root);
  if (open != ERROR_SUCCESS || !sub) return false;
  const DWORD value = taskbar ? 1u : 0u;
  const LONG wrote = RegSetValueExW(sub, L"IsPromoted", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
  RegCloseKey(sub);
  return wrote == ERROR_SUCCESS;
}

bool PromoteGroup(const Group& group, bool taskbar) {
  if (IsSelfPath(group.iconPath) || EqualsIgnoreCase(group.fileDisplay, kAppExe) || EqualsIgnoreCase(group.fileDisplay, kLegacyExe)) {
    return false;
  }
  bool ok = false;
  for (const Member& member : group.members) {
    if (WritePromoted(member, taskbar)) ok = true;
  }
  return ok;
}

RestoreResult RestoreGroup(const Group& group) {
  if (IsSelfPath(group.iconPath) || EqualsIgnoreCase(group.fileDisplay, kAppExe) || EqualsIgnoreCase(group.fileDisplay, kLegacyExe)) {
    return RestoreResult::Missing;
  }
  for (const Member& member : group.members) {
    if (!IsSelfPath(member.expandedPath)) ApplyState(member, false);
  }
  for (const Member& member : group.members) {
    if (IsSelfPath(member.expandedPath) || member.fileName.empty()) continue;
    NudgeOwners(member);
  }
  const IconPresence presence = GroupPresence(group);
  if (presence == IconPresence::Shown) return RestoreResult::Shown;
  if (presence == IconPresence::Hidden) return RestoreResult::StillHidden;
  return RestoreResult::Missing;
}

void RepairTrayIcons(const std::vector<Group>& groups, const std::vector<Rule>& hidden) {
  auto ruled = [&](const Rule& id) {
    for (const Rule& rule : hidden) {
      if (SameApp(rule, id)) return true;
    }
    return false;
  };
  // Undo accidental NIS_HIDDEN on icons that are not in the hide list (e.g. uid=0 blast).
  for (const Group& group : groups) {
    if (ruled(group.id)) continue;
    if (IsSelfPath(group.iconPath) || EqualsIgnoreCase(group.fileDisplay, kAppExe) ||
        EqualsIgnoreCase(group.fileDisplay, kLegacyExe)) {
      continue;
    }
    for (const Member& member : group.members) {
      if (!IsSelfPath(member.expandedPath)) ApplyState(member, false);
    }
  }
  BroadcastTaskbarCreated();
}

HICON IconForGroup(const Group& group, int size) {
  if (!group.snapshot.empty()) {
    if (HICON icon = IconFromPng(group.snapshot, size)) return icon;
  }
  if (HICON icon = IconFromFile(group.iconPath, group.fileDisplay, size)) return icon;
  std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float cover = RoundRectAlpha(x + 0.5f, y + 0.5f, static_cast<float>(size), static_cast<float>(size) * 0.22f);
      if (cover <= 0.0f) continue;
      pixels[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
          Premultiply(37, 99, 235, static_cast<int>(255.0f * cover + 0.5f));
    }
  }
  return IconFromPixels(size, pixels.data());
}

float ShapeCoverage(float distance) { return Clampf(0.5f - distance, 0.0f, 1.0f); }

float SdRoundBox(float px, float py, float left, float top, float right, float bottom, float radius) {
  const float cx = (left + right) * 0.5f;
  const float cy = (top + bottom) * 0.5f;
  const float halfW = (right - left) * 0.5f - radius;
  const float halfH = (bottom - top) * 0.5f - radius;
  float qx = std::fabs(px - cx) - halfW;
  float qy = std::fabs(py - cy) - halfH;
  const float ox = qx > 0.0f ? qx : 0.0f;
  const float oy = qy > 0.0f ? qy : 0.0f;
  const float inside = (qx > qy ? qx : qy) < 0.0f ? (qx > qy ? qx : qy) : 0.0f;
  return std::sqrt(ox * ox + oy * oy) + inside - radius;
}

float SdCircle(float px, float py, float cx, float cy, float radius) {
  const float dx = px - cx;
  const float dy = py - cy;
  return std::sqrt(dx * dx + dy * dy) - radius;
}

void BlendPremul(float* red, float* green, float* blue, float* alpha, float srcR, float srcG, float srcB, float srcA) {
  if (srcA <= 0.0f) return;
  const float outA = srcA + *alpha * (1.0f - srcA);
  if (outA <= 0.0f) {
    *red = *green = *blue = *alpha = 0.0f;
    return;
  }
  *red = (srcR * srcA + *red * *alpha * (1.0f - srcA)) / outA;
  *green = (srcG * srcA + *green * *alpha * (1.0f - srcA)) / outA;
  *blue = (srcB * srcA + *blue * *alpha * (1.0f - srcA)) / outA;
  *alpha = outA;
}

float SdSegment(float px, float py, float ax, float ay, float bx, float by, float thickness) {
  const float abx = bx - ax;
  const float aby = by - ay;
  const float apx = px - ax;
  const float apy = py - ay;
  const float denom = abx * abx + aby * aby;
  float t = denom > 0.0f ? (apx * abx + apy * aby) / denom : 0.0f;
  t = Clampf(t, 0.0f, 1.0f);
  const float cx = ax + abx * t;
  const float cy = ay + aby * t;
  return SdCircle(px, py, cx, cy, thickness);
}

float SdCheckGlyph(float px, float py, float s) {
  const float thick = s * 0.078f;
  const float a = SdSegment(px, py, s * 0.28f, s * 0.52f, s * 0.44f, s * 0.68f, thick);
  const float b = SdSegment(px, py, s * 0.44f, s * 0.68f, s * 0.74f, s * 0.32f, thick);
  return (std::min)(a, b);
}

void PaintBrandPixels(int size, std::vector<std::uint32_t>* pixels) {
  pixels->assign(static_cast<size_t>(size) * static_cast<size_t>(size), 0);
  const float s = static_cast<float>(size);
  const float inset = s * 0.08f;
  const float corner = s * 0.30f;

  for (int y = 0; y < size; ++y) {
    for (int x = 0; x < size; ++x) {
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float body = ShapeCoverage(SdRoundBox(px, py, inset, inset, s - inset, s - inset, corner));
      if (body <= 0.0f) continue;

      const float t = Clampf((py - inset) / (s - inset * 2.0f), 0.0f, 1.0f);
      float red = 0.0f;
      float green = 0.0f;
      float blue = 0.0f;
      float alpha = 0.0f;
      // Soft violet → sky blue, closer to iOS app tiles
      BlendPremul(&red, &green, &blue, &alpha, 95.0f - 15.0f * t, 120.0f + 55.0f * t, 250.0f - 20.0f * t, body);

      const float hx = s * 0.32f;
      const float hy = s * 0.30f;
      const float highlight = Clampf(1.0f - ((px - hx) * (px - hx) + (py - hy) * (py - hy)) / (s * s * 0.16f), 0.0f, 1.0f);
      BlendPremul(&red, &green, &blue, &alpha, 255.0f, 255.0f, 255.0f, highlight * 0.30f * body);

      // One bold white check — stays readable at tray size
      const float check = ShapeCoverage(SdCheckGlyph(px, py, s)) * body;
      BlendPremul(&red, &green, &blue, &alpha, 255.0f, 255.0f, 255.0f, check);

      (*pixels)[static_cast<size_t>(y) * static_cast<size_t>(size) + static_cast<size_t>(x)] =
          Premultiply(static_cast<int>(red + 0.5f), static_cast<int>(green + 0.5f), static_cast<int>(blue + 0.5f),
                      static_cast<int>(alpha * 255.0f + 0.5f));
    }
  }
}

HICON DownscalePixels(const std::vector<std::uint32_t>& source, int sourceSize, int size) {
  if (sourceSize == size) return IconFromPixels(size, source.data());
  if (!EnsureFactory()) return IconFromPixels(size, source.data());
  ComPtr<IWICBitmap> bitmap;
  ComPtr<IWICBitmapScaler> scaler;
  ComPtr<IWICFormatConverter> converter;
  if (FAILED(g_factory->CreateBitmapFromMemory(static_cast<UINT>(sourceSize), static_cast<UINT>(sourceSize), GUID_WICPixelFormat32bppPBGRA,
                                               static_cast<UINT>(sourceSize) * 4,
                                               static_cast<UINT>(sourceSize) * static_cast<UINT>(sourceSize) * 4,
                                               reinterpret_cast<BYTE*>(const_cast<std::uint32_t*>(source.data())), bitmap.Put()))) {
    return nullptr;
  }
  if (FAILED(g_factory->CreateBitmapScaler(scaler.Put()))) return nullptr;
  if (FAILED(scaler->Initialize(bitmap.pointer, static_cast<UINT>(size), static_cast<UINT>(size), WICBitmapInterpolationModeFant))) {
    return nullptr;
  }
  if (FAILED(g_factory->CreateFormatConverter(converter.Put()))) return nullptr;
  if (FAILED(converter->Initialize(scaler.pointer, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return nullptr;
  }
  std::vector<std::uint32_t> pixels(static_cast<size_t>(size) * static_cast<size_t>(size));
  const UINT stride = static_cast<UINT>(size) * 4;
  if (FAILED(converter->CopyPixels(nullptr, stride, stride * static_cast<UINT>(size), reinterpret_cast<BYTE*>(pixels.data())))) {
    return nullptr;
  }
  return IconFromPixels(size, pixels.data());
}

HICON CreateBrandIcon(int size) {
  if (size <= 0) size = 32;
  if (HICON icon = IconFromTrayPng(size)) return icon;
  const int work = (std::max)(size * 3, 96);
  std::vector<std::uint32_t> pixels;
  PaintBrandPixels(work, &pixels);
  if (HICON icon = DownscalePixels(pixels, work, size)) return icon;
  PaintBrandPixels(size, &pixels);
  return IconFromPixels(size, pixels.data());
}

bool WriteBrandPreview(const std::wstring& path, int size) {
  if (size <= 0) size = 256;
  const int work = (std::max)(size * 3, 96);
  std::vector<std::uint32_t> hi;
  PaintBrandPixels(work, &hi);
  HICON scaled = DownscalePixels(hi, work, size);
  std::vector<std::uint32_t> pixels;
  if (scaled && PixelsFromIcon(scaled, size, &pixels)) {
    DestroyIcon(scaled);
  } else {
    if (scaled) DestroyIcon(scaled);
    PaintBrandPixels(size, &pixels);
  }
  BITMAPV5HEADER header{};
  header.bV5Size = sizeof(header);
  header.bV5Width = size;
  header.bV5Height = size;
  header.bV5Planes = 1;
  header.bV5BitCount = 32;
  header.bV5Compression = BI_BITFIELDS;
  header.bV5RedMask = 0x00FF0000;
  header.bV5GreenMask = 0x0000FF00;
  header.bV5BlueMask = 0x000000FF;
  header.bV5AlphaMask = 0xFF000000;
  BITMAPFILEHEADER file{};
  file.bfType = 0x4D42;
  file.bfOffBits = sizeof(file) + sizeof(header);
  file.bfSize = file.bfOffBits + static_cast<DWORD>(pixels.size() * 4);
  std::vector<std::uint32_t> bottomUp(pixels.size());
  for (int y = 0; y < size; ++y) {
    memcpy(bottomUp.data() + static_cast<size_t>(y) * static_cast<size_t>(size),
           pixels.data() + static_cast<size_t>(size - 1 - y) * static_cast<size_t>(size), static_cast<size_t>(size) * 4);
  }
  HANDLE handle = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (handle == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  WriteFile(handle, &file, sizeof(file), &written, nullptr);
  WriteFile(handle, &header, sizeof(header), &written, nullptr);
  WriteFile(handle, bottomUp.data(), static_cast<DWORD>(bottomUp.size() * 4), &written, nullptr);
  CloseHandle(handle);
  return true;
}

void ShutdownImaging() {
  if (g_factory) {
    g_factory->Release();
    g_factory = nullptr;
  }
}

bool WriteUtf8File(const std::wstring& path, const std::string& data) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  CloseHandle(file);
  return ok && written == data.size();
}

bool WriteDump(const std::wstring& path, const std::vector<Group>& groups) {
  std::string out = "\xEF\xBB\xBF";
  out += "count=" + std::to_string(groups.size()) + "\r\n";
  int index = 1;
  for (const Group& group : groups) {
    out += "[" + std::to_string(index++) + "] name=" + WideToUtf8(group.id.name);
    out += " file=" + WideToUtf8(group.id.file);
    out += " product=" + WideToUtf8(group.id.product);
    out += " uid=" + (group.id.hasUid ? std::to_string(group.id.uid) : std::string("-"));
    out += " guid=" + WideToUtf8(group.id.guid);
    out += " members=" + std::to_string(group.members.size());
    out += group.anyOnTaskbar ? " bar=1" : " bar=0";
    out += group.anyVisible ? " visible=1\r\n" : " visible=0\r\n";
    for (const Member& member : group.members) {
      out += "    place=";
      out += member.place == Place::Taskbar ? "taskbar" : member.place == Place::Overflow ? "overflow" : "unknown";
      out += member.exists ? " exists=1 " : " exists=0 ";
      out += "desc=" + WideToUtf8(member.fileDescription) + " ";
      out += WideToUtf8(member.expandedPath.empty() ? member.rawPath : member.expandedPath);
      out += "\r\n";
    }
  }
  return WriteUtf8File(path, out);
}
