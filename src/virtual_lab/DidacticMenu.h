#pragma once

#include <Arduino.h>
#include <vector>

namespace virtual_lab {

struct DidacticEntry {
  String key;
  String title;
  String text;
};

class DidacticMenu {
 public:
  void addEntry(const String &key, const String &title, const String &text);
  bool findEntry(const String &key, DidacticEntry &entry) const;
  void clear();
  const std::vector<DidacticEntry> &entries() const { return entries_; }

 private:
  std::vector<DidacticEntry> entries_;
};

}  // namespace virtual_lab
