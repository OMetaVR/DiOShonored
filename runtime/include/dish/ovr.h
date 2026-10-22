// Native overrides of hot msvcr90 functions. Every `ovr_<name>` has the GuestFn signature and reads its arguments from the guest
// stack at esp+4 (cdecl: the caller pops). The lifter config maps a guest function to one of these symbols.
//
// x87 register-argument helpers (_CIpow, _CIsin, ...): the generic form reads st(0) from guest_thread->st0_ret and st(1) from the
// first double of the runtime-private thread block (*(double *)guest_thread->runtime) -- an INTERIM convention that is documented in
// docs/abi_change_requests.md; the result is written to guest_thread->st0_ret. Lifted code that lowers these calls as intrinsics can call
// the dish_* host functions below with plain doubles instead.
#pragma once
#include "guest_abi.h"

#ifdef __cplusplus
extern "C" {
#endif

GuestRet ovr_memcpy(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_memmove(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_memset(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_memcmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_memchr(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_strlen(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_wcslen(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_strcmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_wcscmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__stricmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__strnicmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__wcsicmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__wcsnicmp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_floor(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr_ceil(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__controlfp_s(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIpow(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIfmod(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIatan2(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIsqrt(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIsin(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIcos(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CItan(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIasin(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIacos(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIatan(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIlog(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIlog10(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIexp(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIsinh(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CIcosh(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);
GuestRet ovr__CItanh(uint32_t esp, uint32_t eax, uint32_t ecx, uint32_t edx);

/* Plain-double entry points for call-site intrinsics. */
double dish_ci_pow(double base, double exponent);
double dish_ci_fmod(double x, double y);
double dish_ci_atan2(double y, double x);

#ifdef __cplusplus
}
#endif
