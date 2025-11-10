#include "cpu_timestamp_fix.h"

#include "function_hook.h"
#include "ini.h"
#include "logger.h"

// Optional high frequency cpu patch hooks.
FunctionHook hook_GetTimebase_((LONGLONG(__cdecl *)())(nullptr));
FunctionHook hook_GetCpuSpeed2_((LONGLONG(__stdcall *)())(nullptr));
FunctionHook hook_GetCpuSpeed3_((LONGLONG(__stdcall *)())(nullptr));

// The first fix replaces the rdtsc call with a more time deterministic QPC call. This is
// called repeatedly by the game's timebase call. The i64FirstTimeStampTicks is set by a
// call to this function very early in boot.
LONGLONG __cdecl GetTimebaseHook() {
  static LARGE_INTEGER s_cpu_ticks_timestamp = {0};
  LONGLONG first_timestamp = *reinterpret_cast<long long *>(0x008092c8);  // i64FirstTimeStampTicks
  LARGE_INTEGER cpu_ticks;
  QueryPerformanceCounter(&cpu_ticks);
  return (cpu_ticks.QuadPart - first_timestamp);
}

// The second fix reports an accurate frequency in timebase ticks per millisecond.
// The client stores this in a 64-bit global used as a division factor in the timebase call.
LONGLONG __stdcall GetCpuSpeed2Hook() {
  LARGE_INTEGER frequency;
  QueryPerformanceFrequency(&frequency);
  LONGLONG ticks_per_ms = frequency.QuadPart / 1000;
  ::Sleep(1000u);  // TODO: This delay is probably not necessary.
  return ticks_per_ms;
}

// Optionally installs the hooks to fix the timebase.
void CpuTimestampFix::Initialize(const std::filesystem::path &ini_file) {
  bool disable = Ini::GetValue<bool>("EqwGeneral", "DisableCpuTimebaseFix", false, ini_file.string().c_str());
  if (disable) return;

  LARGE_INTEGER dummy;  // Perform an OS support check before hooking.
  if (!QueryPerformanceCounter(&dummy) || !QueryPerformanceFrequency(&dummy))
    return;  // Unsupported so just fallback to the original versions.

  auto dll = GetModuleHandleA("eqgfx_dx8.dll");
  FARPROC get_speed_cpu2 = dll ? GetProcAddress(dll, "GetCpuSpeed2") : nullptr;
  FARPROC get_speed_cpu3 = dll ? GetProcAddress(dll, "GetCpuSpeed3") : nullptr;

  if (!get_speed_cpu2 || !get_speed_cpu3) return;  // Something is wrong, bail out.

  Logger::Info("Enabling CPU timebase fix");

  // First install the hook in game.exe for the primary GetTimebase.
  hook_GetTimebase_.Initialize(0x00559bf4, GetTimebaseHook, FunctionHook::HookType::Detour);

  // And then install the hooks in eqgfx_dx8 that fetch the cpu speed. These are both
  // called by the client and it compares and picks one. Just use the same call for both.

  hook_GetCpuSpeed2_.Initialize(reinterpret_cast<int>(get_speed_cpu2), GetCpuSpeed2Hook,
                                FunctionHook::HookType::Detour);
  hook_GetCpuSpeed3_.Initialize(reinterpret_cast<int>(get_speed_cpu3), GetCpuSpeed2Hook,
                                FunctionHook::HookType::Detour);
}