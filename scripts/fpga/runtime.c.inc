#include <stdint.h>

#define INPUT_BASE ((volatile const unsigned char *)0xc0000000UL)
#define RESULT_BASE ((volatile unsigned char *)0xcf000000UL)

static uint64_t input_offset;
static uint64_t output_offset;
static uint64_t timer_start;
static uint64_t timer_total;

static inline uint64_t read_cycle(void) {
  uint64_t value;
  __asm__ volatile("rdcycle %0" : "=r"(value));
  return value;
}

static int next_byte(void) {
  return INPUT_BASE[input_offset++];
}

static int next_non_space(void) {
  int ch;
  do {
    ch = next_byte();
  } while (ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t');
  return ch;
}

int getint(void) {
  int ch = next_non_space();
  int sign = 1;
  int value = 0;
  if (ch == '-') {
    sign = -1;
    ch = next_byte();
  } else if (ch == '+') {
    ch = next_byte();
  }
  while (ch >= '0' && ch <= '9') {
    value = value * 10 + ch - '0';
    ch = next_byte();
  }
  return sign * value;
}

int getch(void) {
  return next_byte();
}

int getarray(int *values) {
  int count = getint();
  for (int i = 0; i < count; ++i) {
    values[i] = getint();
  }
  return count;
}

void putch(int ch) {
  RESULT_BASE[0x100 + output_offset++] = (unsigned char)ch;
  *(volatile uint64_t *)(RESULT_BASE + 0x18) = output_offset;
}

void putint(int value) {
  char digits[16];
  unsigned int magnitude;
  int count = 0;
  if (value < 0) {
    putch('-');
    magnitude = 0U - (unsigned int)value;
  } else {
    magnitude = (unsigned int)value;
  }
  do {
    digits[count++] = (char)('0' + magnitude % 10U);
    magnitude /= 10U;
  } while (magnitude != 0U);
  while (count != 0) {
    putch(digits[--count]);
  }
}

void putarray(int count, int *values) {
  putint(count);
  for (int i = 0; i < count; ++i) {
    putch(' ');
    putint(values[i]);
  }
  putch('\n');
}

void _sysy_starttime(int line) {
  (void)line;
  timer_start = read_cycle();
}

void _sysy_stoptime(int line) {
  (void)line;
  timer_total += read_cycle() - timer_start;
  *(volatile uint64_t *)(RESULT_BASE + 0x08) = timer_total;
}
