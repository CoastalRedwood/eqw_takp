#pragma once
#include <windows.h>

// Supports hooking virtual function table entries with new functions and includes
// an internal mapping cache so that multiple hooks to the same table with the
// same function end up with the correct original function.

#define czVOID(c) (void)c

class VTableHook {
 public:
  VTableHook() = default;
  VTableHook(void** object_vtable, size_t index, LPVOID new_function, bool debug = false);

  template <typename T>
  T original(T fnType) {
    czVOID(fnType);
    return (T)original_function_;
  }

  LPVOID new_function_ = nullptr;
  LPVOID original_function_ = nullptr;

 private:
  LPVOID ReplaceVTableFunction(void** object_vtable, size_t index, LPVOID new_function, bool debug = false);
};
