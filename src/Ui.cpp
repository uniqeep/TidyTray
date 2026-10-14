#include "Ui.h"

#include "Icons.h"
#include "Rules.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <wchar.h>

namespace {

constexpr int IDC_REFRESH = 1002;
constexpr int IDC_BAR_TITLE = 1003;
constexpr int IDC_OVERFLOW_TITLE = 1004;
constexpr int IDC_BAR_LIST = 1005;
constexpr int IDC_OVERFLOW_LIST = 1006;
constexpr int IDC_AUTOSTART = 1007;
constexpr int IDC_STATUS = 1009;
constexpr int IDC_BAR_SCROLL = 1010;
constexpr int IDC_OVERFLOW_SCROLL = 1011;
constexpr wchar_t kScrollClass[] = L"TidyTrayListScroll";
constexpr int ID_OPEN = 2001;
constexpr int ID_AUTOSTART = 2002;
constexpr int ID_EXIT = 2003;
constexpr int ID_DETAILS = 2004;
constexpr int ID_MOVE_BAR = 2005;
constexpr int ID_MOVE_OVERFLOW = 2006;
constexpr int ID_TOGGLE_HIDE = 2007;
constexpr int kTimer = 42;
constexpr int kSweepTimer = 43;
constexpr int kResumeTimer = 45;
constexpr UINT kTrayId = 1;
constexpr UINT WM_TRAY = WM_APP + 10;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kNotifyKey[] = L"Control Panel\\NotifyIconSettings";

constexpr COLORREF kBg = RGB(242, 242, 247);
constexpr COLORREF kCard = RGB(255, 255, 255);
constexpr COLORREF kText = RGB(28, 28, 30);
constexpr COLORREF kSecondary = RGB(110, 110, 115);
constexpr COLORREF kDropRing = RGB(0, 122, 255);
constexpr int kSwitchOnR = 52;
constexpr int kSwitchOnG = 199;
constexpr int kSwitchOnB = 89;
constexpr int kSwitchOffR = 229;
constexpr int kSwitchOffG = 229;
constexpr int kSwitchOffB = 234;

enum class Zone { None, Taskbar, Overflow };

struct App {
  HWND window = nullptr;
  HWND barTitle = nullptr;
  HWND overflowTitle = nullptr;
  HWND barList = nullptr;
  HWND overflowList = nullptr;
  HWND barScroll = nullptr;
  HWND overflowScroll = nullptr;
  HWND refresh = nullptr;
  HWND autostart = nullptr;
  HWND status = nullptr;
  HFONT font = nullptr;
  HFONT titleFont = nullptr;
  HBRUSH bgBrush = nullptr;
  HBRUSH cardBrush = nullptr;
  HBRUSH selectBrush = nullptr;
  HBITMAP switchOn = nullptr;
  HBITMAP switchOff = nullptr;
  int switchCacheW = 0;
  int switchCacheH = 0;
  HICON iconBig = nullptr;
  HICON iconSmall = nullptr;
  HIMAGELIST images = nullptr;
  Rules rules;
  std::vector<Group> groups;
  bool suppress = false;
  bool barColumns = false;
  bool overflowColumns = false;
  bool closing = false;
  bool dragging = false;
  bool dragImage = false;
  Zone dropHover = Zone::None;
  int dragIndex = -1;
  int dragRow = -1;
  Zone dragSource = Zone::None;
  HWND scrollDragList = nullptr;
  int scrollDragOffset = 0;
  int barWheelLeftover = 0;
  int overflowWheelLeftover = 0;
  int contextIndex = -1;
  Zone contextZone = Zone::Overflow;
  ULONGLONG quietUntil = 0;
  UINT taskbarCreated = 0;
  std::wstring error;
  HANDLE stopEvent = nullptr;
  std::thread watch;
};

App g;

void Layout();
void Reload(bool forceImages);
void ShowAndActivate();
void ShowCloseHint();
void UpdateStatus();
void UpdateTip();
void FillRows();
void SetHidden(int index, bool hidden);
void MoveToZone(int index, Zone zone);
void RestoreHiddenOnExit();
void RequestExit(bool confirm);

int Scale(int value) {
  return MulDiv(value, GetDpiForWindow(g.window), 96);
}

std::wstring ExePath() {
  std::wstring path(32768, L'\0');
  const DWORD count = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
  path.resize(count);
  return path;
}

bool IsAutostart() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) return false;
  wchar_t buffer[32768];
  DWORD bytes = sizeof(buffer);
  DWORD type = 0;
  auto match = [&](const wchar_t* value) {
    bytes = sizeof(buffer);
    type = 0;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<BYTE*>(buffer), &bytes) != ERROR_SUCCESS) return false;
    if (type != REG_SZ && type != REG_EXPAND_SZ) return false;
    return StrStrIW(buffer, kAppExe) != nullptr || StrStrIW(buffer, kLegacyExe) != nullptr;
  };
  const bool enabled = match(kRunValue) || match(kLegacyRunValue);
  RegCloseKey(key);
  return enabled;
}

void SetAutostart(bool enable) {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, kRunKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS) return;
  RegDeleteValueW(key, kLegacyRunValue);
  if (!enable) {
    RegDeleteValueW(key, kRunValue);
  } else {
    const std::wstring command = L"\"" + ExePath() + L"\" /silent";
    RegSetValueExW(key, kRunValue, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()),
                   static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t)));
  }
  RegCloseKey(key);
}

void SyncAutostart() {
  if (g.autostart) SendMessageW(g.autostart, BM_SETCHECK, IsAutostart() ? BST_CHECKED : BST_UNCHECKED, 0);
}

void ApplyFont() {
  const int dpi = GetDpiForWindow(g.window);
  HFONT font = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                            CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  HFONT title = CreateFontW(-MulDiv(12, dpi, 96), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                             CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
  if (!font) return;
  if (g.font) DeleteObject(g.font);
  if (g.titleFont && g.titleFont != g.font) DeleteObject(g.titleFont);
  g.font = font;
  g.titleFont = title ? title : font;
  EnumChildWindows(
      g.window,
      [](HWND child, LPARAM param) -> BOOL {
        const int id = GetDlgCtrlID(child);
        HFONT use = (id == IDC_BAR_TITLE || id == IDC_OVERFLOW_TITLE) ? g.titleFont : reinterpret_cast<HFONT>(param);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(use), TRUE);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(g.font));
}

bool IsSelfGroup(const Group& group) {
  if (EqualsIgnoreCase(group.fileDisplay, kAppExe) || EqualsIgnoreCase(group.fileDisplay, kLegacyExe)) return true;
  if (IsSelfPath(group.iconPath)) return true;
  for (const Member& member : group.members) {
    if (IsSelfPath(member.expandedPath)) return true;
  }
  return false;
}

bool IsListed(const Group& group) {
  if (IsSelfGroup(group)) return false;
  if (g.rules.Contains(group.id)) return true;
  return group.anyVisible;
}

bool InTaskbarZone(const Group& group) {
  if (!IsListed(group) || g.rules.Contains(group.id)) return false;
  return group.anyOnTaskbar;
}

bool InOverflowZone(const Group& group) {
  if (!IsListed(group)) return false;
  if (g.rules.Contains(group.id)) return true;
  return group.anyVisible && !group.anyOnTaskbar;
}

int SwitchWidth() { return Scale(38); }
int SwitchHeight() { return Scale(18); }
int SwitchLane() { return Scale(20) + SwitchWidth() + Scale(10); }

int ScrollSlot() { return Scale(10); }

HWND ScrollForList(HWND list) {
  if (list == g.barList) return g.barScroll;
  if (list == g.overflowList) return g.overflowScroll;
  return nullptr;
}

HWND ListForScroll(HWND scroll) {
  if (scroll == g.barScroll) return g.barList;
  if (scroll == g.overflowScroll) return g.overflowList;
  return nullptr;
}

void StripListScrollStyles(HWND list) {
  if (!list) return;
  const LONG_PTR style = GetWindowLongPtrW(list, GWL_STYLE);
  if (style & (WS_VSCROLL | WS_HSCROLL)) {
    SetWindowLongPtrW(list, GWL_STYLE, style & ~(WS_VSCROLL | WS_HSCROLL));
  }
}

void HideListScrollBars(HWND list) {
  if (!list) return;
  StripListScrollStyles(list);
  ShowScrollBar(list, SB_HORZ, FALSE);
  ShowScrollBar(list, SB_VERT, FALSE);
}

int ContentRight(HWND list) {
  RECT client{};
  GetClientRect(list, &client);
  int right = static_cast<int>(client.right);
  if (right < Scale(80)) right = Scale(80);
  return right;
}

void ClearListSelection(HWND keep = nullptr) {
  if (g.barList && g.barList != keep) {
    ListView_SetItemState(g.barList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  }
  if (g.overflowList && g.overflowList != keep) {
    ListView_SetItemState(g.overflowList, -1, 0, LVIS_SELECTED | LVIS_FOCUSED);
  }
}

void FitNameColumn(HWND list, bool) {
  if (!list) return;
  HideListScrollBars(list);
  RECT rect{};
  GetClientRect(list, &rect);
  int width = static_cast<int>(rect.right);
  if (width < 1) width = 1;
  // Keep column exactly client-wide so ListView never creates an HSCROLL.
  ListView_SetColumnWidth(list, 0, width);
  HideListScrollBars(list);
}

bool ListRowHeight(HWND list, int* heightOut) {
  if (!list || !heightOut || ListView_GetItemCount(list) <= 0) return false;
  RECT row{};
  if (!ListView_GetItemRect(list, 0, &row, LVIR_BOUNDS)) return false;
  *heightOut = static_cast<int>(row.bottom - row.top);
  return *heightOut > 0;
}

struct ListScrollInfo {
  int top = 0;
  int maxTop = 0;
  int itemHeight = 0;
  int trackTop = 0;
  int trackBottom = 0;
  RECT thumb{};
};

bool GetListScrollInfo(HWND list, HWND scroll, ListScrollInfo* info) {
  if (!list || !scroll || !info) return false;
  RECT listClient{};
  GetClientRect(list, &listClient);
  RECT scrollClient{};
  GetClientRect(scroll, &scrollClient);
  int itemHeight = 0;
  if (!ListRowHeight(list, &itemHeight)) return false;
  const int count = ListView_GetItemCount(list);
  int visible = static_cast<int>(listClient.bottom) / itemHeight;
  if (visible < 1) visible = 1;
  if (count <= visible) return false;
  int maxTop = count - visible;
  if (maxTop < 0) maxTop = 0;
  int top = ListView_GetTopIndex(list);
  if (top < 0) top = 0;
  if (top > maxTop) top = maxTop;
  const int trackTop = Scale(4);
  const int trackBottom = static_cast<int>(scrollClient.bottom) - Scale(4);
  const int trackHeight = trackBottom - trackTop;
  if (trackHeight < Scale(16)) return false;
  int thumbHeight = MulDiv(visible, trackHeight, count);
  if (thumbHeight < Scale(16)) thumbHeight = Scale(16);
  if (thumbHeight > trackHeight) thumbHeight = trackHeight;
  const int travel = trackHeight - thumbHeight;
  const int thumbTop = trackTop + (maxTop > 0 ? MulDiv(top, travel, maxTop) : 0);
  const int thickness = Scale(3);
  info->top = top;
  info->maxTop = maxTop;
  info->itemHeight = itemHeight;
  info->trackTop = trackTop;
  info->trackBottom = trackBottom;
  info->thumb.left = (static_cast<int>(scrollClient.right) - thickness) / 2;
  info->thumb.right = info->thumb.left + thickness;
  info->thumb.top = thumbTop;
  info->thumb.bottom = thumbTop + thumbHeight;
  return true;
}

void RefreshListScroll(HWND list) {
  if (!list) return;
  HWND scroll = ScrollForList(list);
  if (scroll) InvalidateRect(scroll, nullptr, FALSE);
}

void ScrollListByPixels(HWND list, int dy) {
  if (!list || dy == 0) return;
  ListView_Scroll(list, 0, dy);
  RefreshListScroll(list);
}

void ScrollListByRows(HWND list, int deltaRows) {
  if (!list || deltaRows == 0) return;
  int itemHeight = 0;
  if (!ListRowHeight(list, &itemHeight)) return;
  ScrollListByPixels(list, deltaRows * itemHeight);
}

void OnListMouseWheel(HWND list, WPARAM wParam) {
  if (!list) return;
  int itemHeight = 0;
  if (!ListRowHeight(list, &itemHeight)) return;
  // One notch ≈ one row; high-resolution wheels accumulate fractional notches.
  int& leftover = list == g.barList ? g.barWheelLeftover : g.overflowWheelLeftover;
  const int delta = GET_WHEEL_DELTA_WPARAM(wParam) + leftover;
  leftover = delta % WHEEL_DELTA;
  const int notches = delta / WHEEL_DELTA;
  if (notches == 0) return;
  ScrollListByPixels(list, -notches * itemHeight);
}

void ScrollListToThumbY(HWND list, HWND scroll, int clientY, int grabOffset) {
  ListScrollInfo info{};
  if (!GetListScrollInfo(list, scroll, &info) || info.maxTop <= 0) return;
  const int thumbHeight = info.thumb.bottom - info.thumb.top;
  const int travel = (info.trackBottom - info.trackTop) - thumbHeight;
  if (travel <= 0) return;
  int pos = clientY - grabOffset - info.trackTop;
  if (pos < 0) pos = 0;
  if (pos > travel) pos = travel;
  ScrollListByRows(list, MulDiv(pos, info.maxTop, travel) - info.top);
}

void PaintListScroll(HWND scroll, HDC dc) {
  HWND list = ListForScroll(scroll);
  if (!list || !dc) return;
  RECT client{};
  GetClientRect(scroll, &client);
  if (!g.cardBrush) g.cardBrush = CreateSolidBrush(kCard);
  FillRect(dc, &client, g.cardBrush);
  ListScrollInfo info{};
  if (!GetListScrollInfo(list, scroll, &info)) return;
  const int radius = info.thumb.right - info.thumb.left;
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(DC_BRUSH));
  HGDIOBJ oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
  SetDCBrushColor(dc, RGB(170, 170, 175));
  RoundRect(dc, info.thumb.left, info.thumb.top, info.thumb.right, info.thumb.bottom, radius, radius);
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
}

LRESULT CALLBACK ListScrollProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
  switch (message) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(hwnd, &paint);
      PaintListScroll(hwnd, dc);
      EndPaint(hwnd, &paint);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      HWND list = ListForScroll(hwnd);
      if (!list) break;
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      ListScrollInfo info{};
      if (!GetListScrollInfo(list, hwnd, &info)) break;
      RECT hit = info.thumb;
      InflateRect(&hit, Scale(4), Scale(2));
      if (PtInRect(&hit, pt)) {
        g.scrollDragList = list;
        g.scrollDragOffset = pt.y - info.thumb.top;
        SetCapture(hwnd);
        return 0;
      }
      // Click track: jump so thumb centers on the click.
      const int thumbHeight = info.thumb.bottom - info.thumb.top;
      ScrollListToThumbY(list, hwnd, pt.y, thumbHeight / 2);
      return 0;
    }
    case WM_MOUSEMOVE: {
      if (g.scrollDragList && GetCapture() == hwnd && (wParam & MK_LBUTTON)) {
        ScrollListToThumbY(g.scrollDragList, hwnd, GET_Y_LPARAM(lParam), g.scrollDragOffset);
        return 0;
      }
      break;
    }
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED: {
      if (g.scrollDragList && (GetCapture() == hwnd || message == WM_CAPTURECHANGED)) {
        if (message == WM_LBUTTONUP && GetCapture() == hwnd) ReleaseCapture();
        g.scrollDragList = nullptr;
        return 0;
      }
      break;
    }
    case WM_MOUSEWHEEL: {
      HWND list = ListForScroll(hwnd);
      if (!list) break;
      OnListMouseWheel(list, wParam);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(hwnd, message, wParam, lParam);
}

void RegisterListScrollClass(HINSTANCE instance) {
  static bool ready = false;
  if (ready) return;
  WNDCLASSW cls{};
  cls.lpfnWndProc = ListScrollProc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.lpszClassName = kScrollClass;
  RegisterClassW(&cls);
  ready = true;
}

HWND CreateListScroll(HWND parent, int id) {
  const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(parent, GWLP_HINSTANCE));
  RegisterListScrollClass(instance);
  return CreateWindowExW(0, kScrollClass, L"", WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0, 0, parent,
                         reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
}

void PaneScreenRect(HWND list, RECT* out) {
  if (!list || !out) return;
  GetWindowRect(list, out);
  HWND scroll = ScrollForList(list);
  if (!scroll) return;
  RECT scrollRect{};
  GetWindowRect(scroll, &scrollRect);
  UnionRect(out, out, &scrollRect);
}

void EnsureListColumns(HWND list, bool* ready, bool withSwitch) {
  if (!list || !ready) return;
  HWND header = ListView_GetHeader(list);
  if (header) {
    while (Header_GetItemCount(header) > 1) ListView_DeleteColumn(list, 1);
  }
  if (*ready) {
    FitNameColumn(list, withSwitch);
    return;
  }
  RECT rect{};
  GetClientRect(list, &rect);
  int width = static_cast<int>(rect.right);
  if (width < Scale(120)) width = Scale(240);
  LVCOLUMNW name{};
  name.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_FMT;
  name.fmt = LVCFMT_LEFT;
  name.cx = (std::max)(Scale(80), width);
  name.pszText = const_cast<wchar_t*>(L"名称");
  ListView_InsertColumn(list, 0, &name);
  *ready = true;
  FitNameColumn(list, withSwitch);
}

#ifndef ILC_HIGHQUALITYSCALE
#define ILC_HIGHQUALITYSCALE 0x00020000
#endif

void QuietFor(DWORD ms) { g.quietUntil = GetTickCount64() + ms; }

bool IsQuiet() { return GetTickCount64() < g.quietUntil; }

void RebuildImages() {
  // Cap icon growth; modest padding lifts row height without a huge gap.
  const int size = (std::min)(18, (std::max)(16, Scale(16)));
  const int row = size + Scale(10);
  HIMAGELIST images = ImageList_Create(size, row, ILC_COLOR32 | ILC_HIGHQUALITYSCALE, static_cast<int>(g.groups.size()) + 1, 4);
  if (!images) return;
  ImageList_SetBkColor(images, CLR_NONE);
  HIMAGELIST square = ImageList_Create(size, size, ILC_COLOR32 | ILC_HIGHQUALITYSCALE, 1, 1);
  HDC screen = GetDC(nullptr);
  for (const Group& group : g.groups) {
    HICON icon = IconForGroup(group, size);
    if (!icon) icon = g.iconSmall;
    if (icon && square && screen) {
      ImageList_Remove(square, -1);
      ImageList_AddIcon(square, icon);
      BITMAPINFO info{};
      info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
      info.bmiHeader.biWidth = size;
      info.bmiHeader.biHeight = -row;
      info.bmiHeader.biPlanes = 1;
      info.bmiHeader.biBitCount = 32;
      info.bmiHeader.biCompression = BI_RGB;
      void* bits = nullptr;
      HBITMAP dib = CreateDIBSection(screen, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
      if (dib && bits) {
        std::memset(bits, 0, static_cast<size_t>(size) * static_cast<size_t>(row) * 4);
        HDC memory = CreateCompatibleDC(screen);
        HGDIOBJ old = SelectObject(memory, dib);
        ImageList_Draw(square, 0, memory, 0, (row - size) / 2, ILD_TRANSPARENT);
        SelectObject(memory, old);
        DeleteDC(memory);
        ImageList_Add(images, dib, nullptr);
        DeleteObject(dib);
      } else {
        ImageList_AddIcon(images, icon);
      }
    } else if (icon) {
      ImageList_AddIcon(images, icon);
    }
    if (icon && icon != g.iconSmall) DestroyIcon(icon);
  }
  if (screen) ReleaseDC(nullptr, screen);
  if (square) ImageList_Destroy(square);
  if (g.images) {
    ListView_SetImageList(g.barList, nullptr, LVSIL_SMALL);
    ListView_SetImageList(g.overflowList, nullptr, LVSIL_SMALL);
    ImageList_Destroy(g.images);
  }
  g.images = images;
  ListView_SetImageList(g.barList, g.images, LVSIL_SMALL);
  ListView_SetImageList(g.overflowList, g.images, LVSIL_SMALL);
}

void InsertGroupRow(HWND list, int row, int index, const Group& group, bool) {
  LVITEMW item{};
  item.mask = LVIF_TEXT | LVIF_IMAGE | LVIF_PARAM;
  item.iItem = row;
  item.pszText = const_cast<wchar_t*>(group.id.name.c_str());
  item.iImage = index;
  item.lParam = index;
  ListView_InsertItem(list, &item);
}

void FillRows() {
  if (!g.barList || !g.overflowList) return;
  g.suppress = true;
  SendMessageW(g.barList, WM_SETREDRAW, FALSE, 0);
  SendMessageW(g.overflowList, WM_SETREDRAW, FALSE, 0);
  EnsureListColumns(g.barList, &g.barColumns, false);
  EnsureListColumns(g.overflowList, &g.overflowColumns, true);
  ListView_DeleteAllItems(g.barList);
  ListView_DeleteAllItems(g.overflowList);
  int barRow = 0;
  int overflowRow = 0;
  for (int index = 0; index < static_cast<int>(g.groups.size()); ++index) {
    const Group& group = g.groups[static_cast<size_t>(index)];
    if (InTaskbarZone(group)) {
      InsertGroupRow(g.barList, barRow++, index, group, false);
    } else if (InOverflowZone(group)) {
      InsertGroupRow(g.overflowList, overflowRow++, index, group, true);
    }
  }
  FitNameColumn(g.barList, false);
  FitNameColumn(g.overflowList, true);
  SendMessageW(g.barList, WM_SETREDRAW, TRUE, 0);
  SendMessageW(g.overflowList, WM_SETREDRAW, TRUE, 0);
  InvalidateRect(g.barList, nullptr, FALSE);
  InvalidateRect(g.overflowList, nullptr, FALSE);
  RefreshListScroll(g.barList);
  RefreshListScroll(g.overflowList);
  g.suppress = false;
  UpdateStatus();
}

bool SameOrder(const std::vector<Group>& left, const std::vector<Group>& right) {
  if (left.size() != right.size()) return false;
  for (size_t i = 0; i < left.size(); ++i) {
    if (!SameApp(left[i].id, right[i].id)) return false;
  }
  return true;
}

bool RefreshVisibility(Group& group) {
  bool anyVisible = false;
  bool anyTaskbar = false;
  for (Member& member : group.members) {
    if (!IconIsVisible(member)) continue;
    anyVisible = true;
    if (member.place != Place::Taskbar) member.place = Place::Overflow;
    if (member.place == Place::Taskbar) anyTaskbar = true;
  }
  group.anyVisible = anyVisible;
  group.anyOnTaskbar = anyTaskbar;
  return anyVisible;
}

bool ApplyRules(std::vector<Group>& groups) {
  bool failed = false;
  for (Group& group : groups) {
    if (!g.rules.Contains(group.id) || IsSelfGroup(group)) continue;
    if (!SuppressGroup(group)) failed = true;
    RefreshVisibility(group);
    if (group.anyVisible) failed = true;
  }
  return failed;
}

void Reload(bool forceImages) {
  if (g.closing) return;
  const std::wstring previousError = g.error;
  LoadResult loaded = LoadGroups();
  // One-shot repair: previous builds could NIS_HIDDEN unrelated icons (uid=0 desktop scan).
  static bool repairedTray = false;
  if (!repairedTray) {
    RepairTrayIcons(loaded.groups, g.rules.All());
    repairedTray = true;
  }
  const bool failed = ApplyRules(loaded.groups);
  if (!loaded.error.empty()) g.error = loaded.error;
  else if (failed) g.error = L"有的图标没能隐藏，请再试一次。";
  else if (previousError == L"隐藏记录无法读取，本次从空白名单开始。") g.error = previousError;
  else g.error.clear();

  const bool sameOrder = !forceImages && SameOrder(g.groups, loaded.groups);
  g.groups = std::move(loaded.groups);
  if (!sameOrder) RebuildImages();
  FillRows();
  UpdateTip();
}

void UpdateStatus() {
  if (!g.status) return;
  // Counts removed — status is only for errors.
  SetWindowTextW(g.status, g.error.empty() ? L"" : g.error.c_str());
}

void UpdateTip() {
  int hidden = 0;
  for (const Group& group : g.groups) {
    if (g.rules.Contains(group.id)) ++hidden;
  }
  std::wstring tip = kAppName;
  if (hidden > 0) tip += L" — 已隐藏 " + std::to_wstring(hidden) + L" 个图标";
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  data.uFlags = NIF_TIP;
  CopyZ(data.szTip, sizeof(data.szTip) / sizeof(wchar_t), tip);
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void RemoveTray() {
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  Shell_NotifyIconW(NIM_DELETE, &data);
}

void AddTray() {
  if (!g.window) return;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
  data.uCallbackMessage = WM_TRAY;
  data.hIcon = g.iconBig ? g.iconBig : g.iconSmall;
  CopyZ(data.szTip, sizeof(data.szTip) / sizeof(wchar_t), kAppName);
  // After sleep/Explorer restart the icon slot may be half-alive — try ADD then MODIFY.
  if (!Shell_NotifyIconW(NIM_ADD, &data)) {
    Shell_NotifyIconW(NIM_MODIFY, &data);
  }
  data.uVersion = NOTIFYICON_VERSION_4;
  Shell_NotifyIconW(NIM_SETVERSION, &data);
  UpdateTip();
}

bool TrayIconAlive() {
  if (!g.window) return false;
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  data.uFlags = NIF_TIP;
  CopyZ(data.szTip, sizeof(data.szTip) / sizeof(wchar_t), kAppName);
  return Shell_NotifyIconW(NIM_MODIFY, &data) == TRUE;
}

void EnsureTray() {
  if (g.closing || !g.window) return;
  if (TrayIconAlive()) {
    UpdateTip();
    return;
  }
  RemoveTray();
  AddTray();
}

void RecoverAfterShell() {
  if (g.closing || !g.window) return;
  // Force a clean re-register after Explorer/sleep rebuilds the notification area.
  RemoveTray();
  AddTray();
  EnsureTray();
  // Re-apply hide rules; Explorer may have reset NIS_HIDDEN on wake.
  Reload(false);
}

void RestoreHiddenOnExit() {
  for (Group& group : g.groups) {
    if (!g.rules.Contains(group.id) || IsSelfGroup(group)) continue;
    RestoreGroup(group);
  }
}

void RequestExit(bool confirm) {
  if (g.closing || !g.window) return;
  if (confirm) {
    const int answer = MessageBoxW(g.window,
                                   L"退出后将恢复已隐藏的图标，下次启动会再按记录隐藏。\n\n确定退出 TidyTray？",
                                   kAppName, MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2);
    if (answer != IDOK) return;
  }
  DestroyWindow(g.window);
}

void ShowCloseHint() {
  if (g.rules.seenCloseHint) return;
  g.rules.seenCloseHint = true;
  g.rules.Save();
  NOTIFYICONDATAW data{};
  data.cbSize = sizeof(data);
  data.hWnd = g.window;
  data.uID = kTrayId;
  data.uFlags = NIF_INFO;
  data.dwInfoFlags = NIIF_INFO;
  CopyZ(data.szInfoTitle, sizeof(data.szInfoTitle) / sizeof(wchar_t), L"TidyTray 仍在运行");
  CopyZ(data.szInfo, sizeof(data.szInfo) / sizeof(wchar_t), L"窗口已关闭，程序还在托盘里。左键打开，右键可以退出。");
  Shell_NotifyIconW(NIM_MODIFY, &data);
}

void ShowAndActivate() {
  if (g.closing || !g.window) return;
  // Show first — Reload enumerates the registry/process list and felt sluggish on tray click.
  ShowWindow(g.window, SW_SHOW);
  if (IsIconic(g.window)) ShowWindow(g.window, SW_RESTORE);
  SetWindowPos(g.window, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetWindowPos(g.window, HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  SetForegroundWindow(g.window);
  SetTimer(g.window, kTimer, 1, nullptr);
}

void ShowTrayMenu() {
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, ID_OPEN, L"打开");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | (IsAutostart() ? MF_CHECKED : 0), ID_AUTOSTART, L"开机启动");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, ID_EXIT, L"退出");
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(g.window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, g.window, nullptr);
  PostMessageW(g.window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

int ItemIndexAt(HWND list, int row) {
  if (!list || row < 0) return -1;
  LVITEMW item{};
  item.mask = LVIF_PARAM;
  item.iItem = row;
  if (!ListView_GetItem(list, &item)) return -1;
  return static_cast<int>(item.lParam);
}

int FindRowByIndex(HWND list, int index) {
  if (!list || index < 0) return -1;
  const int count = ListView_GetItemCount(list);
  for (int row = 0; row < count; ++row) {
    if (ItemIndexAt(list, row) == index) return row;
  }
  return -1;
}

HWND ListForZone(Zone zone) { return zone == Zone::Taskbar ? g.barList : g.overflowList; }

void MoveRowVisual(int index, Zone from, Zone to) {
  if (index < 0 || index >= static_cast<int>(g.groups.size()) || from == to) return;
  HWND source = ListForZone(from);
  HWND destination = ListForZone(to);
  const int row = FindRowByIndex(source, index);
  if (row >= 0) ListView_DeleteItem(source, row);
  const Group& group = g.groups[static_cast<size_t>(index)];
  InsertGroupRow(destination, ListView_GetItemCount(destination), index, group, to == Zone::Overflow);
  if (to == Zone::Overflow) InvalidateRect(destination, nullptr, FALSE);
  UpdateStatus();
}

void ShowDetails(int index) {
  if (index < 0 || index >= static_cast<int>(g.groups.size())) return;
  const Group& group = g.groups[static_cast<size_t>(index)];
  const bool ruled = g.rules.Contains(group.id);
  std::wstring text;
  text += L"名称：" + group.id.name + L"\r\n";
  text += L"程序：" + group.fileDisplay + L"\r\n";
  text += L"路径：" + group.pathShown + L"\r\n";
  if (!group.id.product.empty()) text += L"产品：" + group.id.product + L"\r\n";
  if (group.id.hasUid) text += L"UID：" + std::to_wstring(group.id.uid) + L"\r\n";
  if (!group.id.guid.empty()) text += L"GUID：" + group.id.guid + L"\r\n";
  text += L"位置：";
  text += ruled ? L"已隐藏" : (group.anyOnTaskbar ? L"任务栏" : L"溢出区");
  text += L"\r\n成员：";
  text += std::to_wstring(group.members.size());
  MessageBoxW(g.window, text.c_str(), L"详细信息", MB_OK | MB_ICONINFORMATION);
}

void ShowItemMenu(HWND list, Zone zone, int row) {
  const int index = ItemIndexAt(list, row);
  if (index < 0) return;
  g.contextIndex = index;
  g.contextZone = zone;
  HMENU menu = CreatePopupMenu();
  if (!menu) return;
  AppendMenuW(menu, MF_STRING, ID_DETAILS, L"详细信息");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  if (zone == Zone::Taskbar) {
    AppendMenuW(menu, MF_STRING, ID_MOVE_OVERFLOW, L"移到溢出区");
  } else {
    AppendMenuW(menu, MF_STRING, ID_MOVE_BAR, L"移到任务栏");
    const bool ruled = g.rules.Contains(g.groups[static_cast<size_t>(index)].id);
    AppendMenuW(menu, MF_STRING, ID_TOGGLE_HIDE, ruled ? L"取消隐藏" : L"隐藏图标");
  }
  POINT point{};
  GetCursorPos(&point);
  SetForegroundWindow(g.window);
  TrackPopupMenu(menu, TPM_RIGHTBUTTON, point.x, point.y, 0, g.window, nullptr);
  PostMessageW(g.window, WM_NULL, 0, 0);
  DestroyMenu(menu);
}

void SetHidden(int index, bool hidden) {
  if (index < 0 || index >= static_cast<int>(g.groups.size())) return;
  Group& group = g.groups[static_cast<size_t>(index)];
  if (IsSelfGroup(group)) {
    g.error = L"不能隐藏 TidyTray 自己。";
    UpdateStatus();
    return;
  }
  if (hidden) {
    if (group.anyOnTaskbar) PromoteGroup(group, false);
    g.rules.Upsert(group.id);
    SuppressGroup(group);
    RefreshVisibility(group);
    g.error.clear();
  } else {
    // Always clear the rule on user "show" so sweep cannot hide it again.
    g.rules.Remove(group.id);
    RestoreGroup(group);
    RefreshVisibility(group);
    g.error.clear();
  }
  QuietFor(1500);
  if (hidden) {
    const int barRow = FindRowByIndex(g.barList, index);
    if (barRow >= 0) MoveRowVisual(index, Zone::Taskbar, Zone::Overflow);
  }
  const int overflowRow = FindRowByIndex(g.overflowList, index);
  if (overflowRow >= 0) {
    RECT rowRect{};
    ListView_GetItemRect(g.overflowList, overflowRow, &rowRect, LVIR_BOUNDS);
    InvalidateRect(g.overflowList, &rowRect, FALSE);
  } else if (!hidden) {
    FillRows();
  }
  UpdateStatus();
  UpdateTip();
}

void MoveToZone(int index, Zone zone) {
  if (index < 0 || index >= static_cast<int>(g.groups.size())) return;
  Group& group = g.groups[static_cast<size_t>(index)];
  if (IsSelfGroup(group)) return;
  const Zone from = InTaskbarZone(group) ? Zone::Taskbar : Zone::Overflow;
  if (from == zone && !(zone == Zone::Taskbar && g.rules.Contains(group.id))) return;
  if (zone == Zone::Taskbar) {
    if (g.rules.Contains(group.id)) {
      const RestoreResult back = RestoreGroup(group);
      RefreshVisibility(group);
      if (back == RestoreResult::Shown || group.anyVisible || back != RestoreResult::StillHidden) {
        g.rules.Remove(group.id);
      }
    }
    if (!PromoteGroup(group, true)) {
      g.error = L"没能移到任务栏，请再试一次。";
      UpdateStatus();
      return;
    }
    for (Member& member : group.members) member.place = Place::Taskbar;
    group.anyOnTaskbar = true;
    group.anyVisible = true;
    g.error.clear();
  } else {
    if (!PromoteGroup(group, false)) {
      g.error = L"没能移到溢出区，请再试一次。";
      UpdateStatus();
      return;
    }
    for (Member& member : group.members) {
      if (member.place != Place::Unknown) member.place = Place::Overflow;
    }
    group.anyOnTaskbar = false;
    g.error.clear();
  }
  QuietFor(1200);
  MoveRowVisual(index, from, zone);
  UpdateTip();
}

RECT SwitchRectInItem(HWND list, int row) {
  RECT item{};
  ListView_GetItemRect(list, row, &item, LVIR_BOUNDS);
  int height = SwitchHeight();
  const int width = SwitchWidth();
  const int vPad = Scale(3);
  const int rowHeight = static_cast<int>(item.bottom - item.top);
  int room = rowHeight - vPad * 2;
  if (room < Scale(8)) room = Scale(8);
  if (height > room) height = room;
  RECT sw{};
  sw.right = ContentRight(list) - Scale(8);
  sw.left = sw.right - width;
  sw.top = item.top + vPad + (room - height) / 2;
  sw.bottom = sw.top + height;
  if (sw.top < item.top + Scale(2)) sw.top = item.top + Scale(2);
  if (sw.bottom > item.bottom - Scale(2)) sw.bottom = item.bottom - Scale(2);
  return sw;
}

int RowAtY(HWND list, int y) {
  if (!list) return -1;
  LVHITTESTINFO hit{};
  hit.pt = {Scale(4), y};
  const int byHit = ListView_HitTest(list, &hit);
  if (byHit >= 0) return byHit;
  const int top = ListView_GetTopIndex(list);
  const int count = ListView_GetItemCount(list);
  for (int row = top; row < count; ++row) {
    RECT item{};
    if (!ListView_GetItemRect(list, row, &item, LVIR_BOUNDS)) break;
    if (y >= item.top && y < item.bottom) return row;
    if (item.top > y) break;
  }
  return -1;
}

bool ToggleSwitchAtPoint(HWND list, POINT pt) {
  if (list != g.overflowList) return false;
  const int row = RowAtY(list, pt.y);
  if (row < 0) return false;
  RECT sw = SwitchRectInItem(list, row);
  InflateRect(&sw, Scale(6), Scale(6));
  if (!PtInRect(&sw, pt)) return false;
  const int index = ItemIndexAt(list, row);
  if (index < 0) return false;
  SetHidden(index, !g.rules.Contains(g.groups[static_cast<size_t>(index)].id));
  return true;
}

void DestroySwitchCache() {
  if (g.switchOn) {
    DeleteObject(g.switchOn);
    g.switchOn = nullptr;
  }
  if (g.switchOff) {
    DeleteObject(g.switchOff);
    g.switchOff = nullptr;
  }
  g.switchCacheW = 0;
  g.switchCacheH = 0;
}

HBITMAP RenderSwitchBitmap(HDC ref, int width, int height, bool on) {
  BITMAPINFO info{};
  info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  info.bmiHeader.biWidth = width;
  info.bmiHeader.biHeight = -height;
  info.bmiHeader.biPlanes = 1;
  info.bmiHeader.biBitCount = 32;
  info.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP dib = CreateDIBSection(ref, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (!dib || !bits) return nullptr;
  auto* pixels = static_cast<std::uint8_t*>(bits);
  const int trackR = on ? kSwitchOnR : kSwitchOffR;
  const int trackG = on ? kSwitchOnG : kSwitchOffG;
  const int trackB = on ? kSwitchOnB : kSwitchOffB;
  const float radius = static_cast<float>(height) * 0.5f;
  const float pad = (std::max)(2.0f, static_cast<float>(height) * 0.16f);
  const float thumbR = radius - pad;
  const float thumbY = static_cast<float>(height) * 0.5f;
  const float thumbX = on ? (static_cast<float>(width) - pad - thumbR) : (pad + thumbR);
  for (int y = 0; y < height; ++y) {
    for (int x = 0; x < width; ++x) {
      std::uint8_t* pixel = pixels + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4;
      pixel[0] = 255;
      pixel[1] = 255;
      pixel[2] = 255;
      pixel[3] = 0;
      const float px = x + 0.5f;
      const float py = y + 0.5f;
      const float cx = static_cast<float>(width) * 0.5f;
      const float cy = static_cast<float>(height) * 0.5f;
      const float halfW = static_cast<float>(width) * 0.5f - radius;
      const float halfH = 0.0f;
      float qx = std::fabs(px - cx) - halfW;
      float qy = std::fabs(py - cy) - halfH;
      const float ox = qx > 0.0f ? qx : 0.0f;
      const float oy = qy > 0.0f ? qy : 0.0f;
      const float inside = (qx > qy ? qx : qy) < 0.0f ? (qx > qy ? qx : qy) : 0.0f;
      float dist = std::sqrt(ox * ox + oy * oy) + inside - radius;
      float cover = 0.5f - dist;
      if (cover < 0.0f) cover = 0.0f;
      if (cover > 1.0f) cover = 1.0f;
      if (cover > 0.0f) {
        pixel[0] = static_cast<std::uint8_t>(trackB * cover + 255.0f * (1.0f - cover));
        pixel[1] = static_cast<std::uint8_t>(trackG * cover + 255.0f * (1.0f - cover));
        pixel[2] = static_cast<std::uint8_t>(trackR * cover + 255.0f * (1.0f - cover));
        pixel[3] = 255;
      }
      const float tdx = px - thumbX;
      const float tdy = py - thumbY;
      float tdist = std::sqrt(tdx * tdx + tdy * tdy) - thumbR;
      float tcover = 0.5f - tdist;
      if (tcover < 0.0f) tcover = 0.0f;
      if (tcover > 1.0f) tcover = 1.0f;
      tcover *= cover;
      if (tcover > 0.0f) {
        pixel[0] = static_cast<std::uint8_t>(255.0f * tcover + pixel[0] * (1.0f - tcover));
        pixel[1] = static_cast<std::uint8_t>(255.0f * tcover + pixel[1] * (1.0f - tcover));
        pixel[2] = static_cast<std::uint8_t>(255.0f * tcover + pixel[2] * (1.0f - tcover));
      }
    }
  }
  return dib;
}

void EnsureSwitchCache(HDC ref) {
  const int width = SwitchWidth();
  const int height = SwitchHeight();
  if (g.switchOn && g.switchOff && g.switchCacheW == width && g.switchCacheH == height) return;
  DestroySwitchCache();
  g.switchOn = RenderSwitchBitmap(ref, width, height, true);
  g.switchOff = RenderSwitchBitmap(ref, width, height, false);
  g.switchCacheW = width;
  g.switchCacheH = height;
}

void DrawSwitch(HDC dc, const RECT& box, bool on) {
  if (!dc || box.right - box.left < 4 || box.bottom - box.top < 4) return;
  EnsureSwitchCache(dc);
  HBITMAP bitmap = on ? g.switchOn : g.switchOff;
  if (!bitmap) return;
  HDC memory = CreateCompatibleDC(dc);
  if (!memory) return;
  HGDIOBJ old = SelectObject(memory, bitmap);
  BitBlt(dc, box.left, box.top, g.switchCacheW, g.switchCacheH, memory, 0, 0, SRCCOPY);
  SelectObject(memory, old);
  DeleteDC(memory);
}

LRESULT OnCustomDraw(NMLVCUSTOMDRAW* draw) {
  const bool overflow = draw->nmcd.hdr.hwndFrom == g.overflowList;
  const bool bar = draw->nmcd.hdr.hwndFrom == g.barList;
  if (!overflow && !bar) return CDRF_DODEFAULT;
  HWND list = draw->nmcd.hdr.hwndFrom;
  if (draw->nmcd.dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
  if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    const bool selected = (ListView_GetItemState(list, row, LVIS_SELECTED) & LVIS_SELECTED) != 0;
    draw->clrText = kText;
    if (g.dragging && static_cast<int>(draw->nmcd.lItemlParam) == g.dragIndex) {
      draw->clrText = RGB(148, 163, 184);
    }
    // Soft gray selection without the system blue/hot chrome.
    draw->clrTextBk = selected ? RGB(236, 236, 240) : kCard;
    draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS | CDIS_HOT);
    return CDRF_NEWFONT | (overflow ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT);
  }
  if (overflow && draw->nmcd.dwDrawStage == CDDS_ITEMPOSTPAINT) {
    const int row = static_cast<int>(draw->nmcd.dwItemSpec);
    RECT item{};
    ListView_GetItemRect(list, row, &item, LVIR_BOUNDS);
    RECT client{};
    GetClientRect(list, &client);
    const int contentRight = ContentRight(list);
    // Keep the switch lane free of selection/text.
    RECT cover = item;
    cover.left = (std::max)(static_cast<int>(item.left), contentRight - SwitchLane());
    cover.right = contentRight;
    if (!g.cardBrush) g.cardBrush = CreateSolidBrush(kCard);
    FillRect(draw->nmcd.hdc, &cover, g.cardBrush);
    const int index = ItemIndexAt(list, row);
    if (index >= 0) {
      const bool on = !g.rules.Contains(g.groups[static_cast<size_t>(index)].id);
      DrawSwitch(draw->nmcd.hdc, SwitchRectInItem(list, row), on);
    }
    return CDRF_DODEFAULT;
  }
  return CDRF_DODEFAULT;
}

Zone ZoneFromPoint(POINT screen, HWND* listOut) {
  HWND hit = WindowFromPoint(screen);
  while (hit) {
    if (hit == g.barList || hit == g.barScroll) {
      if (listOut) *listOut = g.barList;
      return Zone::Taskbar;
    }
    if (hit == g.overflowList || hit == g.overflowScroll) {
      if (listOut) *listOut = g.overflowList;
      return Zone::Overflow;
    }
    hit = GetParent(hit);
  }
  RECT bar{}, overflow{};
  PaneScreenRect(g.barList, &bar);
  PaneScreenRect(g.overflowList, &overflow);
  if (PtInRect(&bar, screen)) {
    if (listOut) *listOut = g.barList;
    return Zone::Taskbar;
  }
  if (PtInRect(&overflow, screen)) {
    if (listOut) *listOut = g.overflowList;
    return Zone::Overflow;
  }
  if (listOut) *listOut = nullptr;
  return Zone::None;
}

void StopDragImage() {
  if (!g.dragImage) return;
  ImageList_DragLeave(GetDesktopWindow());
  ImageList_EndDrag();
  g.dragImage = false;
}

void UpdateDropHover(Zone zone) {
  // Only the opposite pane is a valid drop target; never tint the source side.
  Zone next = Zone::None;
  if (g.dragging && zone != Zone::None && zone != g.dragSource) next = zone;
  if (g.dropHover == next) return;
  g.dropHover = next;
  InvalidateRect(g.window, nullptr, FALSE);
}

void BeginDrag(HWND list, Zone zone, int row) {
  const int index = ItemIndexAt(list, row);
  if (index < 0 || !g.images) return;
  g.dragging = true;
  g.dragIndex = index;
  g.dragRow = row;
  g.dragSource = zone;
  g.dropHover = Zone::None;

  POINT cursor{};
  GetCursorPos(&cursor);
  RECT icon{};
  ListView_GetItemRect(list, row, &icon, LVIR_ICON);
  POINT hot{cursor.x, cursor.y};
  ScreenToClient(list, &hot);
  int hotX = hot.x - icon.left;
  int hotY = hot.y - icon.top;
  int cx = 0;
  int cy = 0;
  ImageList_GetIconSize(g.images, &cx, &cy);
  if (hotX < 0 || hotX >= cx) hotX = cx / 2;
  if (hotY < 0 || hotY >= cy) hotY = cy / 2;

  if (ImageList_BeginDrag(g.images, index, hotX, hotY)) {
    ImageList_DragEnter(GetDesktopWindow(), cursor.x, cursor.y);
    g.dragImage = true;
  }
  ShowCursor(FALSE);
  SetCapture(g.window);
  InvalidateRect(g.window, nullptr, FALSE);
}

void EndDrag(bool commit) {
  if (!g.dragging) return;
  const int index = g.dragIndex;
  const Zone source = g.dragSource;
  StopDragImage();
  g.dragging = false;
  g.dragIndex = -1;
  g.dragRow = -1;
  g.dragSource = Zone::None;
  UpdateDropHover(Zone::None);
  ReleaseCapture();
  ShowCursor(TRUE);
  SetCursor(LoadCursorW(nullptr, IDC_ARROW));
  InvalidateRect(g.window, nullptr, FALSE);
  if (!commit || index < 0) return;
  POINT point{};
  GetCursorPos(&point);
  const Zone target = ZoneFromPoint(point, nullptr);
  if (target == Zone::None || target == source) return;
  MoveToZone(index, target);
}

LRESULT CALLBACK ListSubclassProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR) {
  switch (message) {
    case WM_STYLECHANGING: {
      // Block ListView from re-adding system scrollbars while scrolling/updating.
      if (wParam == static_cast<WPARAM>(GWL_STYLE)) {
        auto* style = reinterpret_cast<STYLESTRUCT*>(lParam);
        if (style) style->styleNew &= ~(WS_VSCROLL | WS_HSCROLL);
      }
      break;
    }
    case WM_NCCALCSIZE: {
      // Calculate client area without reserving space for system scrollbars.
      StripListScrollStyles(hwnd);
      return DefSubclassProc(hwnd, message, wParam, lParam);
    }
    case WM_NCPAINT:
      // No non-client scrollbar chrome — scrolling UI lives in the sibling strip.
      HideListScrollBars(hwnd);
      return 0;
    case WM_SIZE: {
      const LRESULT result = DefSubclassProc(hwnd, message, wParam, lParam);
      FitNameColumn(hwnd, hwnd == g.overflowList);
      RefreshListScroll(hwnd);
      return result;
    }
    case WM_MOUSEWHEEL: {
      OnListMouseWheel(hwnd, wParam);
      return 0;
    }
    case WM_VSCROLL:
    case WM_HSCROLL:
      HideListScrollBars(hwnd);
      RefreshListScroll(hwnd);
      return 0;
    case WM_LBUTTONDOWN: {
      if (g.dragging) break;
      POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
      if (ToggleSwitchAtPoint(hwnd, pt)) return 0;
      LVHITTESTINFO hit{};
      hit.pt = pt;
      if (ListView_HitTest(hwnd, &hit) < 0) ClearListSelection();
      else ClearListSelection(hwnd);
      break;
    }
    case WM_NCDESTROY:
      RemoveWindowSubclass(hwnd, ListSubclassProc, 1);
      break;
    default:
      break;
  }
  return DefSubclassProc(hwnd, message, wParam, lParam);
}

void StyleList(HWND list) {
  ListView_SetBkColor(list, kCard);
  ListView_SetTextBkColor(list, kCard);
  ListView_SetTextColor(list, kText);
  ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP);
  SetWindowTheme(list, L"", L"");
  HideListScrollBars(list);
  SetWindowSubclass(list, ListSubclassProc, 1, 0);
}

void PaintFrame(HDC dc) {
  RECT client{};
  GetClientRect(g.window, &client);
  if (!g.bgBrush) g.bgBrush = CreateSolidBrush(kBg);
  FillRect(dc, &client, g.bgBrush);
  if (!g.barList || !g.overflowList) return;
  if (!g.dragging || g.dropHover == Zone::None) return;
  HWND list = g.dropHover == Zone::Taskbar ? g.barList : g.overflowList;
  RECT rect{};
  PaneScreenRect(list, &rect);
  MapWindowPoints(nullptr, g.window, reinterpret_cast<POINT*>(&rect), 2);
  InflateRect(&rect, Scale(1), Scale(1));
  HPEN ring = CreatePen(PS_SOLID, Scale(2), kDropRing);
  HGDIOBJ oldPen = SelectObject(dc, ring);
  HGDIOBJ oldBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
  RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, Scale(12), Scale(12));
  SelectObject(dc, oldBrush);
  SelectObject(dc, oldPen);
  DeleteObject(ring);
}

void Layout() {
  if (!g.window || !g.barList || !g.overflowList || !g.barScroll || !g.overflowScroll) return;
  RECT client{};
  GetClientRect(g.window, &client);
  const int margin = Scale(24);
  const int gap = Scale(16);
  const int width = std::max(0, static_cast<int>(client.right - margin * 2));
  const int titleHeight = Scale(22);
  const int rowHeight = Scale(28);
  const int buttonWidth = Scale(64);
  int y = margin;
  const int statusTop = std::max(y + Scale(160), static_cast<int>(client.bottom) - margin - rowHeight);
  const int areaHeight = std::max(Scale(120), statusTop - gap - y);
  const int paneWidth = std::max(Scale(180), (width - gap) / 2);
  y += titleHeight + Scale(8);
  const int listHeight = std::max(Scale(100), areaHeight - titleHeight - Scale(8));
  const int pad = Scale(4);
  const int slot = ScrollSlot();
  const int listH = listHeight - pad * 2;
  const int barListW = (std::max)(Scale(80), paneWidth - pad * 2 - slot);
  const int overflowListW = (std::max)(Scale(80), width - paneWidth - gap - pad * 2 - slot);
  const int barX = margin + pad;
  const int overflowX = margin + paneWidth + gap + pad;
  const int listY = y;
  const int overflowTitleW = std::max(Scale(80), width - paneWidth - gap - buttonWidth - Scale(12));
  const int statusW = std::max(0, width - Scale(190));

  HDWP hdwp = BeginDeferWindowPos(10);
  if (hdwp) {
    hdwp = DeferWindowPos(hdwp, g.refresh, nullptr, margin + width - buttonWidth, margin - Scale(2), buttonWidth, Scale(28),
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.autostart, nullptr, margin, statusTop, Scale(180), rowHeight, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.status, nullptr, margin + Scale(190), statusTop, statusW, rowHeight, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.barTitle, nullptr, margin + Scale(4), margin, paneWidth - Scale(8), titleHeight,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.overflowTitle, nullptr, margin + paneWidth + gap + Scale(4), margin, overflowTitleW, titleHeight,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.barList, nullptr, barX, listY + pad, barListW, listH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.barScroll, nullptr, barX + barListW, listY + pad, slot, listH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.overflowList, nullptr, overflowX, listY + pad, overflowListW, listH, SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    hdwp = DeferWindowPos(hdwp, g.overflowScroll, nullptr, overflowX + overflowListW, listY + pad, slot, listH,
                          SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW);
    if (hdwp) EndDeferWindowPos(hdwp);
  }
  FitNameColumn(g.barList, false);
  FitNameColumn(g.overflowList, true);
  RefreshListScroll(g.barList);
  RefreshListScroll(g.overflowList);
  RedrawWindow(g.window, nullptr, nullptr, RDW_INVALIDATE | RDW_ALLCHILDREN);
}

HWND CreateList(HWND window, int id) {
  const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
  HWND list = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL |
                                                              LVS_SHAREIMAGELISTS | LVS_NOCOLUMNHEADER,
                              0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
  StyleList(list);
  return list;
}

void CreateControls(HWND window) {
  const HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(window, GWLP_HINSTANCE));
  const DWORD child = WS_CHILD | WS_VISIBLE;
  g.refresh = CreateWindowExW(0, L"BUTTON", L"刷新", child | BS_PUSHBUTTON | WS_TABSTOP, 0, 0, 0, 0, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_REFRESH)), instance, nullptr);
  g.barTitle = CreateWindowExW(0, L"STATIC", L"任务栏", child | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_NOTIFY, 0, 0, 0, 0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_BAR_TITLE)), instance, nullptr);
  g.overflowTitle = CreateWindowExW(0, L"STATIC", L"溢出区", child | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_NOTIFY, 0, 0, 0, 0,
                                    window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OVERFLOW_TITLE)), instance, nullptr);
  g.barList = CreateList(window, IDC_BAR_LIST);
  g.overflowList = CreateList(window, IDC_OVERFLOW_LIST);
  g.barScroll = CreateListScroll(window, IDC_BAR_SCROLL);
  g.overflowScroll = CreateListScroll(window, IDC_OVERFLOW_SCROLL);
  g.autostart = CreateWindowExW(0, L"BUTTON", L"开机时自动启动", child | BS_AUTOCHECKBOX | WS_TABSTOP, 0, 0, 0, 0, window,
                                reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_AUTOSTART)), instance, nullptr);
  g.status = CreateWindowExW(0, L"STATIC", L"", child | SS_LEFT | SS_CENTERIMAGE | SS_NOPREFIX | SS_ENDELLIPSIS | SS_NOTIFY, 0, 0, 0, 0,
                             window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_STATUS)), instance, nullptr);
}

void WatchRegistry(HWND window, HANDLE stop) {
  HKEY key = nullptr;
  RegOpenKeyExW(HKEY_CURRENT_USER, kNotifyKey, 0, KEY_NOTIFY, &key);
  HANDLE changed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  while (WaitForSingleObject(stop, 0) != WAIT_OBJECT_0) {
    bool armed = false;
    if (key && changed) {
      armed = RegNotifyChangeKeyValue(key, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, changed, TRUE) == ERROR_SUCCESS;
    }
    if (armed) {
      HANDLE waits[2] = {stop, changed};
      if (WaitForMultipleObjects(2, waits, FALSE, INFINITE) == WAIT_OBJECT_0) break;
    } else if (WaitForSingleObject(stop, 2000) == WAIT_OBJECT_0) {
      break;
    }
    PostMessageW(window, WM_APP_REGISTRY, 0, 0);
  }
  if (changed) CloseHandle(changed);
  if (key) RegCloseKey(key);
}

void StartWatcher() {
  if (g.watch.joinable()) return;
  g.stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  const HWND window = g.window;
  const HANDLE stop = g.stopEvent;
  g.watch = std::thread([window, stop] { WatchRegistry(window, stop); });
}

LRESULT HandleListNotify(const NMHDR* header) {
  const bool isBar = header->hwndFrom == g.barList;
  const bool isOverflow = header->hwndFrom == g.overflowList;
  if (!isBar && !isOverflow) return -1;
  const Zone zone = isBar ? Zone::Taskbar : Zone::Overflow;
  HWND list = isBar ? g.barList : g.overflowList;

  if (header->code == NM_CUSTOMDRAW) {
    return OnCustomDraw(reinterpret_cast<NMLVCUSTOMDRAW*>(const_cast<NMHDR*>(header)));
  }
  if (header->code == LVN_BEGINDRAG) {
    const NMLISTVIEW* info = reinterpret_cast<const NMLISTVIEW*>(header);
    BeginDrag(list, zone, info->iItem);
    return 0;
  }
  if (header->code == NM_DBLCLK && isOverflow) {
    const NMITEMACTIVATE* activate = reinterpret_cast<const NMITEMACTIVATE*>(header);
    if (activate->iItem >= 0) {
      const int index = ItemIndexAt(list, activate->iItem);
      if (index >= 0) SetHidden(index, !g.rules.Contains(g.groups[static_cast<size_t>(index)].id));
    }
    return 0;
  }
  if (header->code == NM_RCLICK) {
    const NMITEMACTIVATE* activate = reinterpret_cast<const NMITEMACTIVATE*>(header);
    if (activate->iItem >= 0) ShowItemMenu(list, zone, activate->iItem);
    return 0;
  }
  return 0;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  // Explorer recreates the notification area after crash/restart/sleep — re-register our icon.
  if (g.taskbarCreated != 0 && message == g.taskbarCreated) {
    SetTimer(window, kResumeTimer, 300, nullptr);
    return 0;
  }
  switch (message) {
    case WM_CREATE: {
      g.window = window;
      g.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
      if (g.taskbarCreated) {
        ChangeWindowMessageFilterEx(window, g.taskbarCreated, MSGFLT_ALLOW, nullptr);
      }
      CreateControls(window);
      ApplyFont();
      Layout();
      if (!g.rules.Load()) g.error = L"隐藏记录无法读取，本次从空白名单开始。";
      if (IsAutostart()) SetAutostart(true);
      SyncAutostart();
      Reload(true);
      AddTray();
      StartWatcher();
      SetTimer(window, kSweepTimer, 8000, nullptr);
      return 0;
    }
    case WM_POWERBROADCAST: {
      if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND || wParam == PBT_APMRESUMECRITICAL) {
        // Shell/tray may not be ready on the first resume tick.
        SetTimer(window, kResumeTimer, 800, nullptr);
      }
      return TRUE;
    }
    case WM_ERASEBKGND: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      RECT client{};
      GetClientRect(window, &client);
      if (!g.bgBrush) g.bgBrush = CreateSolidBrush(kBg);
      FillRect(dc, &client, g.bgBrush);
      return 1;
    }
    case WM_PAINT: {
      PAINTSTRUCT paint{};
      HDC dc = BeginPaint(window, &paint);
      PaintFrame(dc);
      EndPaint(window, &paint);
      return 0;
    }
    case WM_SIZE:
      Layout();
      return 0;
    case WM_LBUTTONDOWN:
      ClearListSelection();
      break;
    case WM_DPICHANGED: {
      const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
      SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left, suggested->bottom - suggested->top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
      g.barColumns = false;
      g.overflowColumns = false;
      while (ListView_DeleteColumn(g.barList, 0)) {
      }
      while (ListView_DeleteColumn(g.overflowList, 0)) {
      }
      DestroySwitchCache();
      ApplyFont();
      Reload(true);
      Layout();
      return 0;
    }
    case WM_GETMINMAXINFO: {
      MINMAXINFO* info = reinterpret_cast<MINMAXINFO*>(lParam);
      info->ptMinTrackSize.x = Scale(720);
      info->ptMinTrackSize.y = Scale(480);
      return 0;
    }
    case WM_COMMAND: {
      const int id = LOWORD(wParam);
      const int code = HIWORD(wParam);
      if ((id == IDC_BAR_TITLE || id == IDC_OVERFLOW_TITLE || id == IDC_STATUS) && code == STN_CLICKED) {
        ClearListSelection();
        return 0;
      }
      if (id == IDC_REFRESH || id == ID_OPEN) {
        ClearListSelection();
        if (id == ID_OPEN) ShowAndActivate();
        else Reload(true);
        return 0;
      }
      if (id == IDC_AUTOSTART && code == BN_CLICKED) {
        ClearListSelection();
        SetAutostart(SendMessageW(g.autostart, BM_GETCHECK, 0, 0) == BST_CHECKED);
        return 0;
      }
      if (id == ID_AUTOSTART) {
        SetAutostart(!IsAutostart());
        SyncAutostart();
        return 0;
      }
      if (id == ID_DETAILS) {
        ShowDetails(g.contextIndex);
        return 0;
      }
      if (id == ID_MOVE_BAR) {
        MoveToZone(g.contextIndex, Zone::Taskbar);
        return 0;
      }
      if (id == ID_MOVE_OVERFLOW) {
        MoveToZone(g.contextIndex, Zone::Overflow);
        return 0;
      }
      if (id == ID_TOGGLE_HIDE) {
        if (g.contextIndex >= 0 && g.contextIndex < static_cast<int>(g.groups.size())) {
          SetHidden(g.contextIndex, !g.rules.Contains(g.groups[static_cast<size_t>(g.contextIndex)].id));
        }
        return 0;
      }
      if (id == ID_EXIT) {
        RequestExit(true);
        return 0;
      }
      return 0;
    }
    case WM_NOTIFY: {
      const NMHDR* header = reinterpret_cast<const NMHDR*>(lParam);
      const LRESULT handled = HandleListNotify(header);
      if (handled != -1) return handled;
      break;
    }
    case WM_LBUTTONUP:
      if (g.dragging) EndDrag(true);
      return 0;
    case WM_CAPTURECHANGED:
      if (g.dragging && reinterpret_cast<HWND>(lParam) != window) EndDrag(false);
      return 0;
    case WM_MOUSEMOVE:
      if (g.dragging) {
        POINT point{};
        GetCursorPos(&point);
        if (g.dragImage) ImageList_DragMove(point.x, point.y);
        UpdateDropHover(ZoneFromPoint(point, nullptr));
        return 0;
      }
      break;
    case WM_TRAY: {
      const UINT event = LOWORD(lParam);
      if (event == NIN_SELECT || event == NIN_KEYSELECT) ShowAndActivate();
      else if (event == WM_CONTEXTMENU) ShowTrayMenu();
      return 0;
    }
    case WM_APP_REGISTRY:
      if (!g.closing && !IsQuiet()) SetTimer(window, kTimer, 300, nullptr);
      return 0;
    case WM_TIMER:
      if (wParam == kResumeTimer) {
        KillTimer(window, kResumeTimer);
        RecoverAfterShell();
      } else if (wParam == kTimer) {
        KillTimer(window, kTimer);
        if (!IsQuiet()) Reload(false);
      } else if (wParam == kSweepTimer && !g.closing && !IsQuiet()) {
        for (Group& group : g.groups) {
          if (!g.rules.Contains(group.id) || IsSelfGroup(group)) continue;
          SuppressGroup(group);
        }
      }
      return 0;
    case WM_APP_SHOW:
      ShowAndActivate();
      return 0;
    case WM_APP_EXIT:
      RequestExit(false);
      return 0;
    case WM_SYSCOMMAND:
      if ((wParam & 0xFFF0) == SC_MINIMIZE) {
        ShowWindow(window, SW_HIDE);
        ShowCloseHint();
        return 0;
      }
      break;
    case WM_CLOSE:
      ShowWindow(window, SW_HIDE);
      ShowCloseHint();
      return 0;
    case WM_CTLCOLORSTATIC: {
      const HWND child = reinterpret_cast<HWND>(lParam);
      if (child == g.status || child == g.barTitle || child == g.overflowTitle || child == g.autostart) {
        HDC dc = reinterpret_cast<HDC>(wParam);
        if (!g.bgBrush) g.bgBrush = CreateSolidBrush(kBg);
        SetBkColor(dc, kBg);
        SetTextColor(dc, (child == g.barTitle || child == g.overflowTitle || child == g.status) ? kSecondary : kText);
        return reinterpret_cast<LRESULT>(g.bgBrush);
      }
      break;
    }
    case WM_CTLCOLORBTN: {
      HDC dc = reinterpret_cast<HDC>(wParam);
      if (!g.bgBrush) g.bgBrush = CreateSolidBrush(kBg);
      SetBkColor(dc, kBg);
      return reinterpret_cast<LRESULT>(g.bgBrush);
    }
    case WM_DESTROY: {
      g.closing = true;
      if (g.dragging) EndDrag(false);
      KillTimer(window, kTimer);
      KillTimer(window, kSweepTimer);
      KillTimer(window, kResumeTimer);
      if (g.stopEvent) SetEvent(g.stopEvent);
      if (g.watch.joinable()) g.watch.join();
      if (g.stopEvent) {
        CloseHandle(g.stopEvent);
        g.stopEvent = nullptr;
      }
      RestoreHiddenOnExit();
      RemoveTray();
      if (g.barList) ListView_SetImageList(g.barList, nullptr, LVSIL_SMALL);
      if (g.overflowList) ListView_SetImageList(g.overflowList, nullptr, LVSIL_SMALL);
      if (g.images) {
        ImageList_Destroy(g.images);
        g.images = nullptr;
      }
      if (g.font) {
        DeleteObject(g.font);
        g.font = nullptr;
      }
      if (g.titleFont && g.titleFont != g.font) DeleteObject(g.titleFont);
      g.titleFont = nullptr;
      if (g.bgBrush) {
        DeleteObject(g.bgBrush);
        g.bgBrush = nullptr;
      }
      if (g.cardBrush) {
        DeleteObject(g.cardBrush);
        g.cardBrush = nullptr;
      }
      if (g.selectBrush) {
        DeleteObject(g.selectBrush);
        g.selectBrush = nullptr;
      }
      DestroySwitchCache();
      if (g.iconBig) DestroyIcon(g.iconBig);
      if (g.iconSmall) DestroyIcon(g.iconSmall);
      g.iconBig = nullptr;
      g.iconSmall = nullptr;
      ShutdownImaging();
      PostQuitMessage(0);
      return 0;
    }
    default:
      break;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int RunApp(HINSTANCE instance, bool showWindow) {
  const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  const bool uninitialize = SUCCEEDED(com);
  INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES};
  InitCommonControlsEx(&controls);

  const int dpi = GetDpiForSystem();
  g.iconBig = CreateBrandIcon(GetSystemMetricsForDpi(SM_CXICON, static_cast<UINT>(dpi)));
  g.iconSmall = CreateBrandIcon(GetSystemMetricsForDpi(SM_CXSMICON, static_cast<UINT>(dpi)));

  WNDCLASSEXW cls{};
  cls.cbSize = sizeof(cls);
  cls.lpfnWndProc = WindowProc;
  cls.hInstance = instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hbrBackground = CreateSolidBrush(kBg);
  cls.lpszClassName = kWindowClass;
  cls.hIcon = g.iconBig;
  cls.hIconSm = g.iconSmall;
  if (!RegisterClassExW(&cls)) {
    if (uninitialize) CoUninitialize();
    return 1;
  }

  RECT rect{0, 0, MulDiv(880, dpi, 96), MulDiv(560, dpi, 96)};
  const DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
  AdjustWindowRectExForDpi(&rect, style, FALSE, 0, static_cast<UINT>(dpi));
  RECT work{};
  SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
  const int width = rect.right - rect.left;
  const int height = rect.bottom - rect.top;
  const int x = work.left + std::max(0, static_cast<int>(work.right - work.left - width) / 2);
  const int y = work.top + std::max(0, static_cast<int>(work.bottom - work.top - height) / 2);
  HWND window = CreateWindowExW(0, kWindowClass, kAppName, style, x, y, width, height, nullptr, nullptr, instance, nullptr);
  if (!window) {
    if (uninitialize) CoUninitialize();
    return 1;
  }
  if (showWindow) {
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
  }

  MSG msg{};
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && GetAncestor(msg.hwnd, GA_ROOT) == window) {
      ShowWindow(window, SW_HIDE);
      ShowCloseHint();
      continue;
    }
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }
  if (uninitialize) CoUninitialize();
  return static_cast<int>(msg.wParam);
}
