#pragma once
#include <stdint.h>
#include "ff.h"

// Run the on-device identification flow.
// - Reads 12 MHz averages from benchmark.csv
// - Reads spichips.csv reference table
// - Prints Top-3 candidates with scores
void identify_chip_from_bench_12mhz(void);
