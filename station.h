#pragma once

#include <stdbool.h>
#include <stddef.h>

bool station_normalize_callsign(char *output,
                                size_t output_size,
                                const char *input);
bool station_normalize_grid(char *output,
                            size_t output_size,
                            const char *input);
