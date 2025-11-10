#include "vtable_hook.h"

#include <unordered_map>

#include "logger.h"

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
    if (debug) Logger::Info("Vtable already pointing at your function!");
    if (vtable_hook_map.count(new_function_))
      original_function_ = vtable_hook_map[new_function_];
    else
      Logger::Error("Vtable Error: Invalid double-hooking!");
    return original_function_;
  }

  if (debug) {
    Logger::Info("Create vtable hook on: 0x%x index: %d -> 0x%x to new function 0x%x", (DWORD)object_vtable, index,
                 (DWORD)object_vtable[index], (DWORD)new_function_);
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
