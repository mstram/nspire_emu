#ifndef _H_EMU
#define _H_EMU

#include <stdbool.h>
#include <stdint.h>
#include "types.h"

static inline uint16_t BSWAP16(uint16_t x) { return x << 8 | x >> 8; }
static inline uint32_t BSWAP32(uint32_t x) {
	if (__builtin_constant_p(x)) return x << 24 | (x << 8 & 0xFF0000) | (x >> 8 & 0xFF00) | x >> 24;
	asm ("bswap %0" : "=r" (x) : "0" (x)); return x;
}

/* Declarations for emu.c */

extern int cycle_count_delta __asm__("cycle_count_delta");

extern int throttle_delay;

extern uint32_t cpu_events __asm__("cpu_events");
#define EVENT_IRQ 1
#define EVENT_FIQ 2
#define EVENT_RESET 4
#define EVENT_DEBUG_STEP 8
#define EVENT_WAITING 16

extern bool exiting;
extern bool do_translate;
extern int product;
extern int asic_user_flags;
#define emulate_casplus (product == 0x0C0)
// 0C-0E (CAS, lab cradle, plain Nspire) use old ASIC
// 0F-12 (CX CAS, CX, CM CAS, CM) use new ASIC
#define emulate_cx (product >= 0x0F0)
extern bool turbo_mode;
extern bool is_halting;
extern bool show_speed;

enum { LOG_CPU, LOG_IO, LOG_FLASH, LOG_INTS, LOG_ICOUNT, LOG_USB, LOG_GDB, MAX_LOG };
#define LOG_TYPE_TBL "CIFQ#UG";
extern int log_enabled[MAX_LOG];
void logprintf(int type, char *str, ...);
void emuprintf(char *format, ...);

void warn(char *fmt, ...);
__attribute__((noreturn)) void error(char *fmt, ...);
void throttle_timer_on();
void throttle_timer_off();
int exec_hack();
typedef void fault_proc(uint32_t mva, uint8_t status);
fault_proc prefetch_abort, data_abort __asm__("data_abort");
void add_reset_proc(void (*proc)(void));



int emulate(int flag_debug, int flag_large_nand, int flag_large_sdram, int flag_debug_on_warn, int flag_verbosity, int port_gdb, int port_rgdb, int keypad, int product, uint32_t addr_boot2, char *path_boot1,
                char *path_boot2, char *path_flash, char *path_commands, char *path_log, char *pre_boot2, char *pre_diags, char *pre_os);


#endif
