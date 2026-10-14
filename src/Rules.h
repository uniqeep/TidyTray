#pragma once

#include "Types.h"

class Rules {
 public:
  bool Load();
  bool Save() const;
  bool Contains(const Rule& id) const;
  void Upsert(const Rule& id);
  void Remove(const Rule& id);
  const std::vector<Rule>& All() const { return rules_; }
  bool seenCloseHint = false;
  bool loaded = false;

 private:
  std::vector<Rule> rules_;
  std::wstring path_;
};
