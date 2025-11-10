#include "iat_hook.h"

#include <string>

#include "logger.h"

IATHook::IATHook(HMODULE hmodule, const std::string& dll_name, const std::string& function_name, LPVOID new_function,
                 bool debug) {
  if (new_function_ == new_function) {  // Already hooked
    if (orig_function_ == nullptr)      // Error if this isn't the same already initialized object.
      Logger::Error("IATHoor error: Double-hooking %s::%s", dll_name.c_str(), function_name.c_str());
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
    Logger::Info("Attemping to hook %s in %s by replacing IAT for module %s", function_name.c_str(), dll_name.c_str(),
                 module_name ? module_name : "NOT FOUND");
  }
  while (import_descriptor->Name != 0) {
    char* module_name = (char*)((DWORD_PTR)hmodule + import_descriptor->Name);
    if (debug) Logger::Info("Current dll: %s", module_name);
    if (_stricmp(module_name, dll_name.c_str()) == 0) {
      if (debug) Logger::Info("DLL found: %s", module_name);
      ;
      PIMAGE_THUNK_DATA thunk_iat = (PIMAGE_THUNK_DATA)((DWORD_PTR)hmodule + import_descriptor->FirstThunk);
      PIMAGE_THUNK_DATA thunk_int = (PIMAGE_THUNK_DATA)((DWORD_PTR)hmodule + import_descriptor->OriginalFirstThunk);
      while (thunk_int->u1.AddressOfData != 0) {
        if (debug) Logger::Info("Current function: %s", module_name);
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
  if (debug) Logger::Info("DLL/Function not found");
  return nullptr;  // Function or module not found
}
