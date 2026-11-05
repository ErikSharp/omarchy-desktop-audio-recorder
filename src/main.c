#include "recorder.h"
#include <stdio.h>
#include <signal.h>
#include <string.h>

static void usage(FILE *f) {
    fprintf(f, "Usage: omarchy-desktop-recorder <press|release|stop|status|doctor|reveal PATH>\n");
}

int main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--worker") == 0) return recorder_worker();
    if (argc == 2 && strcmp(argv[1], "doctor") == 0) return recorder_doctor();
    if (argc == 3 && strcmp(argv[1], "reveal") == 0) return recorder_reveal(argv[2]);
    if (argc == 2 && (!strcmp(argv[1], "press") || !strcmp(argv[1], "release") ||
                      !strcmp(argv[1], "stop") || !strcmp(argv[1], "status")))
        return recorder_command(argv[1]);
    usage(stderr);
    return 2;
}
