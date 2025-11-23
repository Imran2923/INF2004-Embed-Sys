#pragma once

#include <stdint.h>
#include <stdbool.h>

// match bench.h
typedef void (*printf_func_t)(const char *fmt, ...);

void identify_chip_from_bench_12mhz(void);
void identify_chip_from_bench_12mhz_with_output(printf_func_t out);