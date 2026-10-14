#include "Rules.h"

#include "Brand.h"

#include <objbase.h>
#include <shlobj.h>

#include <utility>

namespace {

std::wstring AppDataRulesPath(const wchar_t* folder) {
  PWSTR appData = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) || appData == nullptr) {
    return L"rules.txt";
  }
  std::wstring directory = appData;
  CoTaskMemFree(appData);
  directory += L"\\";
  directory += folder;
  CreateDirectoryW(directory.c_str(), nullptr);
  return directory + L"\\rules.txt";
}

std::wstring RulesPath() { return AppDataRulesPath(kAppDataDir); }

std::wstring LegacyRulesPath() { return AppDataRulesPath(kLegacyAppDataDir); }

bool FileExists(const std::wstring& path) {
  const DWORD attributes = GetFileAttributesW(path.c_str());
  return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

std::string ReadFileBytes(const std::wstring& path) {
  HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return {};
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > 8 * 1024 * 1024) {
    CloseHandle(file);
    return {};
  }
  std::string data(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const BOOL ok = ReadFile(file, data.data(), static_cast<DWORD>(data.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok) return {};
  data.resize(read);
  return data;
}

bool WriteFileBytes(const std::wstring& path, const std::string& data) {
  const std::wstring temp = path + L".tmp";
  HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) return false;
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  CloseHandle(file);
  if (!ok || written != data.size()) {
    DeleteFileW(temp.c_str());
    return false;
  }
  if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    DeleteFileW(temp.c_str());
    return false;
  }
  return true;
}

std::wstring AfterEquals(const std::wstring& line) {
  const auto pos = line.find(L'=');
  if (pos == std::wstring::npos) return {};
  return line.substr(pos + 1);
}

bool StartsWith(const std::wstring& text, const wchar_t* prefix) {
  const size_t length = wcslen(prefix);
  return text.size() >= length && text.compare(0, length, prefix) == 0;
}

std::wstring Sanitize(std::wstring text) {
  for (wchar_t& ch : text) {
    if (ch == L'\r' || ch == L'\n') ch = L' ';
  }
  return text;
}

}  // namespace

bool Rules::Load() {
  path_ = RulesPath();
  loaded = true;
  rules_.clear();
  seenCloseHint = false;

  std::string bytes = ReadFileBytes(path_);
  if (bytes.empty()) {
    const std::wstring legacy = LegacyRulesPath();
    if (FileExists(legacy)) bytes = ReadFileBytes(legacy);
  }
  if (bytes.empty()) return true;

  std::string_view view(bytes);
  if (view.size() >= 3 && static_cast<unsigned char>(view[0]) == 0xEF && static_cast<unsigned char>(view[1]) == 0xBB &&
      static_cast<unsigned char>(view[2]) == 0xBF) {
    view.remove_prefix(3);
  }
  const std::wstring text = Utf8ToWide(view);
  if (text.empty() && !view.empty()) return false;

  bool header = true;
  bool magic = false;
  bool inRecord = false;
  Rule current;
  auto push = [&] {
    if (!current.file.empty() || !current.guid.empty()) rules_.push_back(std::move(current));
    current = {};
  };

  size_t cursor = 0;
  while (cursor <= text.size()) {
    size_t end = text.find(L'\n', cursor);
    if (end == std::wstring::npos) end = text.size();
    std::wstring line = text.substr(cursor, end - cursor);
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    cursor = end + 1;

    if (header) {
      if (line == kRulesMagic || line == kLegacyRulesMagic) {
        magic = true;
        continue;
      }
      if (line == L"---") {
        header = false;
        inRecord = true;
        continue;
      }
      if (StartsWith(line, L"seenCloseHint=")) seenCloseHint = AfterEquals(line) == L"1";
      continue;
    }
    if (line == L"---") {
      if (inRecord) push();
      inRecord = true;
      continue;
    }
    if (!inRecord || line.empty()) continue;
    if (StartsWith(line, L"file=")) current.file = ToLower(AfterEquals(line));
    else if (StartsWith(line, L"product=")) current.product = ToLower(AfterEquals(line));
    else if (StartsWith(line, L"guid=")) current.guid = ToLower(AfterEquals(line));
    else if (StartsWith(line, L"uid=")) current.uid = static_cast<std::uint32_t>(wcstoul(AfterEquals(line).c_str(), nullptr, 10));
    else if (StartsWith(line, L"hasUid=")) current.hasUid = AfterEquals(line) == L"1";
    else if (StartsWith(line, L"name=")) current.name = AfterEquals(line);
  }
  if (inRecord) push();
  if (!magic) {
    rules_.clear();
    return false;
  }
  return true;
}

bool Rules::Save() const {
  std::string out = "\xEF\xBB\xBF";
  out += WideToUtf8(kRulesMagic) + "\r\n";
  out += seenCloseHint ? "seenCloseHint=1\r\n" : "seenCloseHint=0\r\n";
  out += "---\r\n";
  for (const Rule& rule : rules_) {
    out += "file=" + WideToUtf8(rule.file) + "\r\n";
    out += "product=" + WideToUtf8(Sanitize(rule.product)) + "\r\n";
    out += "guid=" + WideToUtf8(rule.guid) + "\r\n";
    out += "uid=" + std::to_string(rule.uid) + "\r\n";
    out += rule.hasUid ? "hasUid=1\r\n" : "hasUid=0\r\n";
    out += "name=" + WideToUtf8(Sanitize(rule.name)) + "\r\n";
    out += "---\r\n";
  }
  return WriteFileBytes(path_.empty() ? RulesPath() : path_, out);
}

bool Rules::Contains(const Rule& id) const {
  for (const Rule& rule : rules_) {
    if (SameApp(rule, id)) return true;
  }
  return false;
}

void Rules::Upsert(const Rule& id) {
  for (Rule& rule : rules_) {
    if (!SameApp(rule, id)) continue;
    rule = id;
    Save();
    return;
  }
  rules_.push_back(id);
  Save();
}

void Rules::Remove(const Rule& id) {
  std::vector<Rule> kept;
  kept.reserve(rules_.size());
  for (const Rule& rule : rules_) {
    if (!SameApp(rule, id)) kept.push_back(rule);
  }
  rules_.swap(kept);
  Save();
}
