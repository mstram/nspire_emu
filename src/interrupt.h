/* Declarations for interrupt.c */

#ifndef _H_INTERRUPT
#define _H_INTERRUPT

#define INT_SERIAL   1

#define INT_WATCHDOG 3

#define INT_USB      8

#define INT_ADC      11

#define INT_POWER    15

#define INT_KEYPAD   16

#define INT_TIMER0   17

#define INT_TIMER1   18

#define INT_TIMER2   19

#define INT_LCD      21



extern struct interrupt_state {

	uint32_t active;

	uint32_t raw_status;         // .active ^ ~.noninverted

	uint32_t sticky_status;      // set on rising transition of .raw_status

	uint32_t status;             // +x04: mixture of bits from .raw_status and .sticky_status

	                        //       (determined by .sticky)

	uint32_t mask[2];            // +x08: enabled interrupts

	uint8_t  prev_pri_limit[2];  // +x28: saved .priority_limit from reading +x24

	uint8_t  priority_limit[2];  // +x2C: interrupts with priority >= this value are disabled

	uint32_t noninverted;        // +200: which interrupts not to invert in .raw_status

	uint32_t sticky;             // +204: which interrupts to use .sticky_status

	uint8_t  priority[32];       // +3xx: priority per interrupt (0=max, 7=min)

} intr;

uint32_t int_read_word(uint32_t addr);

void int_write_word(uint32_t addr, uint32_t value);

uint32_t int_cx_read_word(uint32_t addr);

void int_cx_write_word(uint32_t addr, uint32_t value);

void int_set(uint32_t int_num, bool on);

void int_reset();

void *int_save_state(size_t *size);

void int_reload_state(void *state);

#endif
