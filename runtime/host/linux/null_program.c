/* Placeholder used when no lifter output is available: an empty program, so the host executable still builds and can run
 * --dump-hle / --check. Replaced automatically by generated/program.c when it exists (see CMakeLists.txt). */
#include "guest_abi.h"
const GuestProgram guest_program = { GUEST_ABI_VERSION, 0, 0, 0, 0, 0, 0 };
GuestFn guest_lookup(uint32_t addr) { (void)addr; return (GuestFn)0; }
