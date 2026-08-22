#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "station.h"

int main(void)
{
    char callsign[13];
    char grid[7];

    assert(station_normalize_callsign(callsign, sizeof(callsign), "n0call/p"));
    assert(strcmp(callsign, "N0CALL/P") == 0);
    assert(!station_normalize_callsign(callsign, sizeof(callsign), "NOCALL"));
    assert(!station_normalize_callsign(callsign, sizeof(callsign), "N0//P"));
    assert(station_normalize_grid(grid, sizeof(grid), "io91WM"));
    assert(strcmp(grid, "IO91wm") == 0);
    assert(station_normalize_grid(grid, sizeof(grid), "FN20"));
    assert(!station_normalize_grid(grid, sizeof(grid), "ZZ99zz"));
    puts("station: callsign and Maidenhead grid validation verified");
    return 0;
}
