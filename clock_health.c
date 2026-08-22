#ifdef __linux__
#include <sys/timex.h>
#endif

#include "clock_health.h"

bool wspr_clock_is_synchronized(void)
{
#ifdef __linux__
    struct timex state = {0};
    int result = adjtimex(&state);

    return result >= 0 && result != TIME_ERROR && (state.status & STA_UNSYNC) == 0;
#else
    /* macOS keeps time synchronization behind system services without a
       read-only adjtimex equivalent. Boot supervision remains responsible. */
    return true;
#endif
}
