#pragma once

#include "StringUtil.h"

#include <cstdint>
#include <vector>

// Windows 11 lists known notification icons under
// HKCU\Control Panel\NotifyIconSettings\<id>.
// IsPromoted only moves an icon between the taskbar and the overflow
// chevron. This app hides an icon in both places with Shell_NotifyIcon
// NIM_MODIFY and NIS_HIDDEN, and clears that state to put it back.
enum class Place { Unknown, Overflow, Taskbar };

struct Rule {
  std::wstring file;
  std::wstring product;
  std::wstring guid;
  std::uint32_t uid = 0;
  bool hasUid = false;
  std::wstring name;
};

struct Member {
  std::wstring subkey;
  std::wstring rawPath;
  std::wstring expandedPath;
  std::wstring fileName;
  std::wstring product;
  std::wstring fileDescription;
  std::wstring publisher;
  std::wstring tooltip;
  std::wstring guid;
  std::uint32_t uid = 0;
  bool hasUid = false;
  bool exists = false;
  Place place = Place::Unknown;
  std::vector<std::uint8_t> snapshot;
};

struct Group {
  Rule id;
  std::wstring fileDisplay;
  std::wstring pathShown;
  std::wstring iconPath;
  std::vector<std::uint8_t> snapshot;
  std::vector<Member> members;
  bool anyOnTaskbar = false;
  bool anyVisible = false;
};

inline bool SameApp(const Rule& left, const Rule& right) {
  if (!left.guid.empty() || !right.guid.empty()) {
    return !left.guid.empty() && EqualsIgnoreCase(left.guid, right.guid);
  }
  // Same tray identity can hop between helper processes (e.g. NVIDIA Container /
  // NVDisplay.Container) while keeping product + UID.
  if (left.hasUid && right.hasUid && left.uid == right.uid && !left.product.empty() &&
      EqualsIgnoreCase(left.product, right.product)) {
    return true;
  }
  if (left.file.empty() || !EqualsIgnoreCase(left.file, right.file)) return false;
  if (left.hasUid && right.hasUid && left.uid != right.uid) return false;
  if (!left.product.empty() && !right.product.empty() && !EqualsIgnoreCase(left.product, right.product)) {
    return false;
  }
  return true;
}

inline void RefreshGroupFlags(Group& group) {
  group.anyOnTaskbar = false;
  group.anyVisible = false;
  for (const Member& member : group.members) {
    if (member.place == Place::Taskbar) group.anyOnTaskbar = true;
    if (member.place == Place::Taskbar || member.place == Place::Overflow) group.anyVisible = true;
  }
}
