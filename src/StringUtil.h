#pragma once

#include <string>
#include <string_view>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

inline std::wstring Utf8ToWide(std::string_view text) {
  if (text.empty()) return {};
  int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
  DWORD flags = MB_ERR_INVALID_CHARS;
  if (count <= 0) {
    flags = 0;
    count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
  }
  if (count <= 0) return {};
  std::wstring wide(static_cast<size_t>(count), L'\0');
  MultiByteToWideChar(CP_UTF8, flags, text.data(), static_cast<int>(text.size()), wide.data(), count);
  return wide;
}

inline std::string WideToUtf8(std::wstring_view text) {
  if (text.empty()) return {};
  int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
  if (count <= 0) return {};
  std::string utf8(static_cast<size_t>(count), '\0');
  WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), utf8.data(), count, nullptr, nullptr);
  return utf8;
}

inline std::wstring Trim(std::wstring text) {
  auto space = [](wchar_t ch) {
    return ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n';
  };
  while (!text.empty() && space(text.front())) text.erase(text.begin());
  while (!text.empty() && space(text.back())) text.pop_back();
  return text;
}

inline std::wstring FirstLine(std::wstring text) {
  const auto pos = text.find_first_of(L"\r\n");
  if (pos != std::wstring::npos) text.erase(pos);
  return Trim(std::move(text));
}

inline std::wstring ToLower(std::wstring text) {
  if (!text.empty()) CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
  return text;
}

inline bool EqualsIgnoreCase(const std::wstring& left, const std::wstring& right) {
  return CompareStringOrdinal(left.c_str(), static_cast<int>(left.size()), right.c_str(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

inline bool ContainsIgnoreCase(const std::wstring& haystack, const std::wstring& needle) {
  if (needle.empty()) return true;
  const std::wstring left = ToLower(haystack);
  const std::wstring right = ToLower(needle);
  return left.find(right) != std::wstring::npos;
}

inline std::wstring Truncate(std::wstring text, size_t limit) {
  if (text.size() <= limit) return text;
  if (limit > 0) {
    const wchar_t ch = text[limit - 1];
    if (ch >= 0xD800 && ch <= 0xDBFF) --limit;
  }
  text.resize(limit);
  text += L"…";
  return text;
}

inline void CopyZ(wchar_t* dest, size_t capacity, const std::wstring& text) {
  if (capacity == 0) return;
  const size_t count = text.size() < capacity - 1 ? text.size() : capacity - 1;
  if (count > 0) memcpy(dest, text.data(), count * sizeof(wchar_t));
  dest[count] = L'\0';
}
