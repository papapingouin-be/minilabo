#include "virtual_lab/DidacticMenu.h"

namespace virtual_lab {

void DidacticMenu::addEntry(const String &key, const String &title,
                            const String &text) {
  for (auto &entry : entries_) {
    if (entry.key == key) {
      entry.title = title;
      entry.text = text;
      return;
    }
  }
  DidacticEntry entry;
  entry.key = key;
  entry.title = title;
  entry.text = text;
  entries_.push_back(entry);
}

bool DidacticMenu::findEntry(const String &key, DidacticEntry &entry) const {
  for (const auto &item : entries_) {
    if (item.key == key) {
      entry = item;
      return true;
    }
  }
  return false;
}

void DidacticMenu::clear() { entries_.clear(); }

}  // namespace virtual_lab
