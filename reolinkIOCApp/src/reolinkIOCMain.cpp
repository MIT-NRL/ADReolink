#include <stdlib.h>
#include <signal.h>

#include "epicsExit.h"
#include "epicsThread.h"
#include "iocsh.h"

static volatile sig_atomic_t stopRequested = 0;

static void requestStop(int)
{
    stopRequested = 1;
}

int main(int argc, char *argv[])
{
    if (argc >= 2) {
        iocsh(argv[1]);
        epicsThreadSleep(0.2);
    }
    signal(SIGINT, requestStop);
    signal(SIGTERM, requestStop);
    while (!stopRequested) {
        epicsThreadSleep(0.2);
    }
    epicsExit(0);
    return 0;
}
