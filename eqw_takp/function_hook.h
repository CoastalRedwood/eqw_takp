#pragma once
#include <windows.h>

// Supports hooking absolute address functions by inserting trampolines as required.

#define czVOID(c) (void)c

class FunctionHook {
 public:
  // Types of supported hooks. All hooks store the original bytes for restoration at deletion and their
  // original() methods execute the original code (stored in the trampoline).
  enum HookType {
    ReplaceCall,  // Replace an 0xe8 relative call or 0xe9 relative jump (middle of function).
    Detour,       // More general insertion of a detour hook at start of function.
    Vtable,       // Replace a vtable int address entry.
  };

  FunctionHook() = delete;                      // No simple implicit construction.
  FunctionHook(const FunctionHook &) = delete;  // Copies not allowed.
  FunctionHook(FunctionHook &&) = delete;       // Moves not allowed.

  ~FunctionHook();  // Copies back original code.

  template <typename T>
  explicit FunctionHook(T replacement_function_dummy_ptr) {}

  template <typename T>
  FunctionHook(int patch_address, T replacement_function_ptr, HookType hooktype = Detour) {
    Initialize(patch_address, replacement_function_ptr, hooktype);
  }

  template <typename T>
  void Initialize(int patch_address, T replacement_function_ptr, HookType hooktype = Detour) {
    if (patch_address_ != 0) return;  // Bail out if attempting to reinitialize.
    patch_address_ = patch_address;
    replacement_callee_addr = reinterpret_cast<int>(replacement_function_ptr);
    hook_type_ = hooktype;

    switch (hook_type_) {
      case Detour: {
        detour();
        break;
      }
      case ReplaceCall: {
        replace_call();
        break;
      }
      case Vtable: {
        replace_vtable();
        break;
      }
      default:
        fatal_error();
        break;
    }
  }

  template <typename T>
  T original(T fnType) {
    czVOID(fnType);
    return (T)trampoline;
  }

 private:
  void detour();
  void replace_call();
  void replace_vtable();
  void fatal_error();
  void patch_trampoline_relative_jumps();

  int patch_address_ = 0;                                   // Address to patch to jump to the callee.
  int replacement_callee_addr = 0;                          // Address of the replacement function.
  HookType hook_type_ = HookType::ReplaceCall;              // Type of replacement.
  int orig_byte_count = 0;                                  // Number of original patch bytes modified.
  BYTE original_bytes[11] = {0};                            // 5 byte jump with 6 more bytes for opcode boundary.
  BYTE trampoline_bytes[sizeof(original_bytes) + 5] = {0};  // Additional space for an 0xe9 jump opcode at end.
  int trampoline = reinterpret_cast<int>(&trampoline_bytes[0]);
};
