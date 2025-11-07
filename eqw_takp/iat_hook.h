#pragma once
#include <windows.h>

#include <string>

// Replaces an imported function lookup table pointer with a new function pointer. The original
// one is cached for use by the new function. The code allows repeated patching with the same
// new_function.

#define czVOID(c) (void)c

class IATHook {
 public:
  IATHook() = default;
  IATHook(HMODULE hmodule, const std::string& dll_name, const std::string& function_name, LPVOID new_function,
          bool debug = false);

  template <typename T>
  T original(T fnType) {
    czVOID(fnType);
    return (T)orig_function_;
  }

  LPVOID new_function_ = nullptr;
  LPVOID orig_function_ = nullptr;

 private:
  LPVOID ReplaceIATFunction(HMODULE hmodule, const std::string& dll_name, const std::string& function_name,
                            LPVOID new_function, bool debug);
};
