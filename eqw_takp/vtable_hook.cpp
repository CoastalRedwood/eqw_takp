#include "vtable_hook.h"

#include <iostream>
#include <string>
#include <unordered_map>

// This global map stores the hooked pairs to support "double hooking" where a new
// object is trying to repeat the hook in a shared table. This will happen if the
// DInput stack returns the same device LUT for both mouse and keyboard.
static std::unordered_map<void*, void*> vtable_hook_map;

VTableHook::VTableHook(void** object_vtable, size_t index, LPVOID new_function, bool debug) {
  ReplaceVTableFunction(object_vtable, index, new_function, debug);
}

void* VTableHook::ReplaceVTableFunction(void** object_vtable, size_t index, LPVOID new_function, bool debug) {
  if (!object_vtable) return nullptr;

  new_function_ = new_function;
  if (object_vtable[index] == new_function_) {
    if (debug) std::cout << "Vtable already pointing at your function!" << std::endl;
    if (vtable_hook_map.count(new_function_))
      original_function_ = vtable_hook_map[new_function_];
    else
      std::cout << "Vtable Error: Invalid double-hooking!" << std::endl;
    return original_function_;
  }

  if (debug) {
    std::cout << "Create vtable hook on: 0x" << std::hex << object_vtable << " index: " << index;
    std::cout << " -> 0x" << object_vtable[index] << " to new function 0x" << new_function_ << std::dec << std::endl;
  }

  original_function_ = object_vtable[index];
  vtable_hook_map[new_function_] = original_function_;

  // Replace the function pointer in the vtable
  DWORD old_protect;
  ::VirtualProtect(&object_vtable[index], sizeof(void*), PAGE_EXECUTE_READWRITE, &old_protect);
  object_vtable[index] = new_function_;
  ::VirtualProtect(&object_vtable[index], sizeof(void*), old_protect, &old_protect);
  ::FlushInstructionCache(GetCurrentProcess(), (PVOID*)&object_vtable[index], sizeof(new_function_));

  return original_function_;
}
