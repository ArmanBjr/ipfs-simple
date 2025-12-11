#include <stdio.h>
#include "engine.h"

void engine_run(void) {
    // TODO: thread pool + IPC will be implemented here.
    puts("engine started");
}

int main(void) {
    engine_run();
    return 0;
}
