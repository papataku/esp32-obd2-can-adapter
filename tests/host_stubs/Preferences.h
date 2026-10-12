#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <vector>
class Preferences {
 public:
  inline static std::map<std::string,std::vector<uint8_t>> storage;
  inline static unsigned writes=0;
  bool begin(const char* name,bool=false){prefix_=std::string(name)+"/";return true;}
  bool getBool(const char*,bool fallback=false) const {return fallback;}
  size_t putBool(const char*,bool){return 1;}
  size_t getBytesLength(const char* key) const {auto i=storage.find(prefix_+key);return i==storage.end()?0:i->second.size();}
  size_t getBytes(const char* key,void* output,size_t n) const {
    auto i=storage.find(prefix_+key);if(i==storage.end() || i->second.size()!=n)return 0;
    std::memcpy(output,i->second.data(),n);return n;
  }
  size_t putBytes(const char* key,const void* data,size_t n){
    const auto* p=static_cast<const uint8_t*>(data);storage[prefix_+key]=std::vector<uint8_t>(p,p+n);++writes;return n;
  }
  void end(){}
 private: std::string prefix_;
};
