#include "function_hook.h"

#include <memory>

#include "instruction_length.h"
#include "logger.h"

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "psapi.lib")

void protected_mem_set(int target, int val, int size, BYTE *buffer = nullptr) {
  DWORD oldprotect;
  VirtualProtect((PVOID *)target, size, PAGE_EXECUTE_READWRITE, &oldprotect);
  if (buffer) memcpy(buffer, (void *)target, size);
  memset((void *)target, val, size);
  VirtualProtect((PVOID *)target, size, oldprotect, &oldprotect);
  FlushInstructionCache(GetCurrentProcess(), (PVOID *)target, size);
}

template <typename T>
static void protected_mem_write(int target, const T &value) {
  DWORD oldprotect;
  size_t size = sizeof(value);
  ::VirtualProtect(reinterpret_cast<PVOID *>(target), size, PAGE_EXECUTE_READWRITE, &oldprotect);
  memcpy(reinterpret_cast<T *>(target), &value, size);
  ::VirtualProtect(reinterpret_cast<PVOID *>(target), size, oldprotect, &oldprotect);
  ::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<PVOID *>(target), size);
}

static void protected_mem_copy(int target, BYTE *source, int size, BYTE *buffer = nullptr) {
  DWORD oldprotect;
  ::VirtualProtect((PVOID *)target, size, PAGE_EXECUTE_READWRITE, &oldprotect);
  if (buffer) memcpy((void *)buffer, (const void *)target, size);
  memcpy((void *)target, (const void *)source, size);
  ::VirtualProtect((PVOID *)target, size, oldprotect, &oldprotect);
  ::FlushInstructionCache(GetCurrentProcess(), (PVOID *)target, size);
}

static int protected_instruction_to_absolute_address(int instruction_address)  // assumes 32 bit
{
  int end_of_instruction = instruction_address + 0x5;
  DWORD oldprotect;
  ::VirtualProtect((PVOID *)instruction_address, 5, PAGE_EXECUTE_READWRITE, &oldprotect);
  BYTE instruction = *(BYTE *)instruction_address;
  int r = 0;
  if (instruction == 0xE8 || instruction == 0xE9)
    r = (*(int *)(instruction_address + 0x1)) + end_of_instruction;
  else if (instruction == 0xFF) {
    r = (*(int *)(instruction_address + 0x2));
    r = *(int *)r;
  }
  ::VirtualProtect((PVOID *)instruction_address, 5, oldprotect, &oldprotect);
  ::FlushInstructionCache(GetCurrentProcess(), (PVOID *)instruction_address, 5);
  return r;
}

static void write_relative_jump(int jump_instr_addr, int relative_jump_size) {
  *reinterpret_cast<uint8_t *>(jump_instr_addr) = 0xE9;
  *reinterpret_cast<int *>(jump_instr_addr + 1) = relative_jump_size;
  DWORD old_protect;
  ::VirtualProtect((LPVOID)jump_instr_addr, 5, PAGE_EXECUTE_READWRITE, &old_protect);
  ::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<PVOID *>(jump_instr_addr), 5);
}

// Fixes the relative address values of standard call and jump opcodes that were moved to the
// trampoline buffer.
void FunctionHook::patch_trampoline_relative_jumps() {
  int i = 0;
  unsigned char *opcodes = reinterpret_cast<unsigned char *>(trampoline);
  while (i < orig_byte_count) {
    if (opcodes[i] == 0xe8 || opcodes[i] == 0xe9) {
      if (i + 5 > orig_byte_count) fatal_error();  // Must be mis-aligned.
      int32_t *relative_address = reinterpret_cast<int32_t *>(&opcodes[i + 1]);
      *relative_address += patch_address_ - trampoline;  // Just modify with delta.
    }
    i += InstructionLength::InstructionLength(&opcodes[i]);  // Stay opcode aligned.
  }
}

// We FunctionHook everything at startup, so just use an obvious fatal error with the address to ID.
void FunctionHook::fatal_error() {
  Logger::Error("Fatal hook wrapper patching: 0x%x", patch_address_);
  throw std::bad_alloc();  // Will crash out the program.
}

void FunctionHook::detour() {
  // First determine how many aligned instruction bytes are required to move to our trampoline.
  orig_byte_count = InstructionLength::InstructionLength((unsigned char *)patch_address_);
  while (orig_byte_count < 5)  // you need 5 bytes for a jmp
    orig_byte_count += InstructionLength::InstructionLength((unsigned char *)(patch_address_ + orig_byte_count));

  // Save the original bytes for restoration upon deletion.
  if (orig_byte_count > sizeof(original_bytes)) {
    fatal_error();  // Will crash out the program.
    return;
  }
  memcpy(original_bytes, (void *)patch_address_, orig_byte_count);

  // Create the trampoline.
  DWORD dummy_protect;
  int trampoline_size = orig_byte_count + 5;
  ::VirtualProtect(trampoline_bytes, trampoline_size, PAGE_EXECUTE_READWRITE, &dummy_protect);

  // Copy the original bytes over to the trampoline start and patch their relative addresses.
  // The address patching will play nice with already hooked functions or early jumps/calls.
  // Note that if it is already hooked with a jump, our appended jump is redundant.
  memcpy(trampoline_bytes, original_bytes, orig_byte_count);
  patch_trampoline_relative_jumps();

  // And append the jump back to the original function after the moved bytes.
  int jump_instr_addr = trampoline + orig_byte_count;
  int relative_jump_size = (patch_address_ + orig_byte_count) - (jump_instr_addr + 5);
  *reinterpret_cast<uint8_t *>(jump_instr_addr) = 0xE9;
  *reinterpret_cast<int *>(jump_instr_addr + 1) = relative_jump_size;
  ::FlushInstructionCache(GetCurrentProcess(), trampoline_bytes, trampoline_size);

  // And finally patch the original code location with the jump to the detour.
  DWORD old_protect;
  ::VirtualProtect((LPVOID)patch_address_, orig_byte_count, PAGE_EXECUTE_READWRITE, &old_protect);
  int relative_jump_to_callee = replacement_callee_addr - (patch_address_ + 5);
  *reinterpret_cast<uint8_t *>(patch_address_) = 0xE9;
  *reinterpret_cast<int *>(patch_address_ + 1) = relative_jump_to_callee;

  // If there are more than 5 bytes of original instructions, fill the gap with NOPs
  if (orig_byte_count > 5) protected_mem_set(patch_address_ + 5, 0x90, orig_byte_count - 5);

  ::VirtualProtect((LPVOID)patch_address_, orig_byte_count, old_protect, &old_protect);
  ::FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<PVOID *>(patch_address_), orig_byte_count);
}

void FunctionHook::replace_call() {
  DWORD old;
  ::VirtualProtect((LPVOID)patch_address_, 0x5, PAGE_EXECUTE_READWRITE, &old);

  if (*(BYTE *)patch_address_ != 0xE9 && *(BYTE *)patch_address_ != 0xE8) fatal_error();  // Will crash out the program.

  // Create a trampoline in case an ->original() call is made.
  int orig_jmp_dest_addr = protected_instruction_to_absolute_address(patch_address_);
  int trampoline_to_orig_jmp_dest = orig_jmp_dest_addr - (trampoline + 5);
  write_relative_jump(trampoline, trampoline_to_orig_jmp_dest);

  // Save the original destination for restoration at deletion
  // and then update the relative jump size to the new target.
  orig_byte_count = 5;
  memcpy(original_bytes, (LPVOID)patch_address_, orig_byte_count);
  int relative_jump_size = replacement_callee_addr - (patch_address_ + 5);
  *(int *)(patch_address_ + 1) = relative_jump_size;

  ::VirtualProtect((LPVOID)patch_address_, 0x5, old, &old);
  ::FlushInstructionCache(GetCurrentProcess(), (LPVOID)patch_address_, 0x5);
}

void FunctionHook::replace_vtable() {
  // Create a trampoline in case an ->original() call is made.
  int orig_jmp_addr = *reinterpret_cast<int *>(patch_address_);
  int relative_jump_size = orig_jmp_addr - (trampoline + 5);
  write_relative_jump(trampoline, relative_jump_size);

  // Save the original destination for restoration at deletion.
  orig_byte_count = 4;
  memcpy(original_bytes, &patch_address_, orig_byte_count);

  // Finally replace the vtable entry.
  protected_mem_write<int>(patch_address_, replacement_callee_addr);
}

FunctionHook::~FunctionHook() { protected_mem_copy(patch_address_, original_bytes, orig_byte_count); }
