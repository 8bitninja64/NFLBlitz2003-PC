#include "hle.h"

#include <cstring>
#include <map>
#include <string>

namespace hle {

static std::map<std::string, guest::Fn>& registry() {
  static std::map<std::string, guest::Fn> r;  // filled by static initializers
  return r;
}

void registerFunction(const char* name, guest::Fn fn) { registry()[name] = fn; }

guest::Fn find(const char* name) {
  auto it = registry().find(name);
  return it == registry().end() ? nullptr : it->second;
}

}  // namespace hle
