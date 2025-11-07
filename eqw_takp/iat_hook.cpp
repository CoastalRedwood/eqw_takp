#include "iat_hook.h"

#include <iostream>
#include <string>

IATHook::IATHook(HMODULE hmodule, const std::string& dll_name, const std::string& function_name, LPVOID new_function,
                 bool debug) {
  if (new_function_ == new_function) {  // Already hooked
    if (orig_function_ == nullptr)      // Error if this isn't the same already initialized object.
      std::cout << "IATHoor error: Double-hooking " << dll_name << "::" << function_name << std::endl;
    return;
  }
  new_function_ = new_function;
  orig_function_ = ReplaceIATFunction(hmodule, dll_name, function_name, new_function, debug);
}

LPVOID IATHook::ReplaceIATFunction(HMODULE hmodule, const std::string& dll_name, const std::string& function_name,
                                   LPVOID new_function, bool debug) {
  PIMAGE_DOS_HEADER dos_header = (PIMAGE_DOS_HEADER)hmodule;
  PIMAGE_NT_HEADERS nt_headers = (PIMAGE_NT_HEADERS)((DWORD_PTR)hmodule + dos_header->e_lfanew);
  PIMAGE_IMPORT_DESCRIPTOR import_descriptor =
      (PIMAGE_IMPORT_DESCRIPTOR)((DWORD_PTR)hmodule +
                                 nt_headers->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);

  if (debug) {
    char module_name[MAX_PATH];
    DWORD result = ::GetModuleFileNameA(hmodule, module_name, MAX_PATH);
    std::cout << "Attempting to hook: " << function_name << " in " << dll_name
              << " by replacing the pointer in the import address table for module: " << module_name << std::endl;
  }
  while (import_descriptor->Name != 0) {
    char* module_name = (char*)((DWORD_PTR)hmodule + import_descriptor->Name);
    if (debug) std::cout << "Current dll: " << module_name << std::endl;
    if (_stricmp(module_name, dll_name.c_str()) == 0) {
      if (debug) std::cout << "DLL found: " << module_name << std::endl;
      PIMAGE_THUNK_DATA thunk_iat = (PIMAGE_THUNK_DATA)((DWORD_PTR)hmodule + import_descriptor->FirstThunk);
      PIMAGE_THUNK_DATA thunk_int = (PIMAGE_THUNK_DATA)((DWORD_PTR)hmodule + import_descriptor->OriginalFirstThunk);
      while (thunk_int->u1.AddressOfData != 0) {
        if (debug) std::cout << "Current function: " << module_name << std::endl;
        if (!(thunk_int->u1.Ordinal & IMAGE_ORDINAL_FLAG)) {
          PIMAGE_IMPORT_BY_NAME importByName =
              (PIMAGE_IMPORT_BY_NAME)((DWORD_PTR)hmodule + thunk_int->u1.AddressOfData);
          const char* func_name = (const char*)importByName->Name;
          if (_stricmp(func_name, function_name.c_str()) == 0) {
            DWORD old_protect;
            ::VirtualProtect(&thunk_iat->u1.Function, sizeof(LPVOID), PAGE_READWRITE, &old_protect);
            LPVOID original_function = (LPVOID)thunk_iat->u1.Function;
            thunk_iat->u1.Function = (ULONGLONG)new_function;
            ::VirtualProtect(&thunk_iat->u1.Function, sizeof(LPVOID), old_protect, &old_protect);
            ::FlushInstructionCache(GetCurrentProcess(), (PVOID*)&thunk_iat->u1.Function, sizeof(LPVOID));
            return original_function;  // Return the original function pointer
          }
        }
        ++thunk_iat;
        ++thunk_int;
      }
    }
    ++import_descriptor;
  }
  if (debug) std::cout << "DLL/Function not found";
  return nullptr;  // Function or module not found
}
