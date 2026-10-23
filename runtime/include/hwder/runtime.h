// HWDER runtime services shared by the core, kernel and services.
#pragma once
#include <stdarg.h>
#include "hwder/cpu.h"
#include "hwder/dispatch.h"

#ifdef __cplusplus
#include <string>
extern "C" {
#endif

void hw_log(const char* fmt, ...);
void hw_fatal(const char* why);
void hw_sleep_ns(s64 ns);
// Guest time scale (F2 turbo): >1 makes the guest clock run faster and guest sleeps/timeouts shorter.
void hw_set_time_scale(double scale);
s64 hw_scaled_ns(s64 guest_ns);  // a guest duration in host nanoseconds

#ifdef __cplusplus
}
void hw_config_load(const std::string& dir);
const std::string& hw_romfs_path();
#endif

#define GUESTP(T, a) ((T*)(uintptr_t)(a))
