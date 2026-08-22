#pragma once

#include <stddef.h>
#include <stdio.h>

/* The upstream library logs every candidate at INFO. Keep daemon output useful. */
#define LOG_PRINTF(...) ((void)0)
