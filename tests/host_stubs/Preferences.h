#pragma once
#include <cstddef>
class Preferences {
 public:
  bool begin(const char*,bool=false){return true;}
  bool getBool(const char*,bool fallback=false) const {return fallback;}
  size_t putBool(const char*,bool){return 1;}
  void end(){}
};
