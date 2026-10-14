#pragma once

#include "Types.h"

struct LoadResult {
  std::vector<Group> groups;
  std::wstring error;
};

LoadResult LoadGroups();
bool IconIsVisible(const Member& member);
bool SuppressIcon(const Member& member);
bool SuppressGroup(const Group& group);
void RepairTrayIcons(const std::vector<Group>& groups, const std::vector<Rule>& hidden);

enum class RestoreResult { Shown, StillHidden, Missing };
RestoreResult RestoreGroup(const Group& group);
bool PromoteGroup(const Group& group, bool taskbar);
HICON IconForGroup(const Group& group, int size);
HICON CreateBrandIcon(int size);
bool WriteBrandPreview(const std::wstring& path, int size);
void ShutdownImaging();
bool WriteDump(const std::wstring& path, const std::vector<Group>& groups);
bool IsSelfPath(const std::wstring& path);
