#include "recorder.h"
#include <glib-unix.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static char *runtime_dir(void) {
    const char *base = g_get_user_runtime_dir();
    return g_build_filename(base, "omarchy-desktop-recorder", NULL);
}

static char *socket_path(void) {
    char *dir = runtime_dir();
    char *path = g_build_filename(dir, "control.sock", NULL);
    g_free(dir);
    return path;
}

static void detach_worker(gpointer unused) {
    (void)unused;
    (void)setsid();
    int nullfd = open("/dev/null", O_RDONLY | O_CLOEXEC);
    if (nullfd >= 0) {
        (void)dup2(nullfd, STDIN_FILENO);
        if (nullfd != STDIN_FILENO) close(nullfd);
    }
}

static int send_command(const char *cmd) {
    char *path = socket_path();
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); g_free(path); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    g_strlcpy(addr.sun_path, path, sizeof addr.sun_path);
    g_free(path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) < 0) { close(fd); return 1; }
    (void)write(fd, cmd, strlen(cmd));
    (void)write(fd, "\n", 1);
    char response[128] = {0};
    ssize_t n = read(fd, response, sizeof response - 1);
    close(fd);
    if (n > 0) fputs(response, stdout);
    return 0;
}

int recorder_command(const char *command) {
    char *dir = runtime_dir();
    if (g_mkdir_with_parents(dir, 0700) < 0) { perror("runtime directory"); g_free(dir); return 1; }
    chmod(dir, 0700);
    int result = send_command(command);
    if (!result) { g_free(dir); return 0; }
    if (!strcmp(command, "status")) {
        puts("idle"); g_free(dir); return 0;
    }
    if (!strcmp(command, "release")) {
        char *latch = g_build_filename(dir, "release-latch", NULL);
        int latchfd = open(latch, O_CREAT | O_WRONLY | O_TRUNC | O_CLOEXEC, 0600);
        if (latchfd >= 0) { (void)write(latchfd, "release\n", 8); close(latchfd); }
        g_free(latch); puts("release latched"); g_free(dir); return 0;
    }
    if (!strcmp(command, "stop")) {
        fprintf(stderr, "No active recorder session.\n"); g_free(dir); return 1;
    }
    if (strcmp(command, "press")) {
        fprintf(stderr, "No active recorder session.\n"); g_free(dir); return 1;
    }
    char *latchpath = g_build_filename(dir, "release-latch", NULL);
    unlink(latchpath); g_free(latchpath);
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) { perror("resolve executable"); g_free(dir); return 1; }
    char *args[] = {exe, "--worker", NULL};
    GError *error = NULL;
    if (!g_spawn_async(NULL, args, NULL, G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
                       detach_worker, NULL, NULL, &error)) {
        fprintf(stderr, "Could not start recorder: %s\n", error->message);
        g_clear_error(&error); g_free(exe); g_free(dir); return 1;
    }
    g_free(exe);
    for (int i = 0; i < 100; ++i) {
        g_usleep(10000);
        if (send_command(command) == 0) { g_free(dir); return 0; }
    }
    fprintf(stderr, "Recorder worker did not become ready.\n");
    g_free(dir);
    return 1;
}

static gboolean handle_client(gint fd, GIOCondition cond, gpointer data) {
    Recorder *r = data;
    if (cond & (G_IO_HUP | G_IO_ERR)) { close(fd); return G_SOURCE_REMOVE; }
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof buf - 1);
    if (n <= 0) { close(fd); return G_SOURCE_REMOVE; }
    g_strchomp(buf);
    if (!strcmp(buf, "press")) {
        char *latch = g_build_filename(r->runtime_dir, "release-latch", NULL);
        gboolean was_released = g_file_test(latch, G_FILE_TEST_EXISTS);
        if (was_released) unlink(latch);
        g_free(latch);
        if (was_released) {
            r->stopping = TRUE; r->finalized = TRUE;
            dprintf(fd, "released during startup\n");
            g_application_quit(G_APPLICATION(r->gtk_app)); close(fd); return G_SOURCE_REMOVE;
        }
        if (r->stopping || r->capture_started) dprintf(fd, "busy\n");
        else if ((r->capture_started = TRUE) && !capture_start(r)) {
            dprintf(fd, "failed: %s\n", r->error);
            capture_stop(r);
        }
        else dprintf(fd, "starting\n");
    } else if (!strcmp(buf, "release") || !strcmp(buf, "stop")) {
        if (r->stopping) dprintf(fd, "stopping\n");
        else { r->stopping = TRUE; capture_stop(r); dprintf(fd, "stopping\n"); }
    } else if (!strcmp(buf, "status")) {
        dprintf(fd, "%s\n", r->failed ? "failed" : r->stopping ? "stopping" :
                r->capture_ready ? "recording" : "starting");
    } else dprintf(fd, "unknown command\n");
    close(fd);
    return G_SOURCE_REMOVE;
}

static gboolean safety_timeout(gpointer data) {
    Recorder *r = data;
    if (!r->stopping) {
        g_strlcpy(r->error, "60-minute safety limit reached", sizeof r->error);
        r->stopping = TRUE;
        capture_stop(r);
    }
    return G_SOURCE_REMOVE;
}

static gboolean accept_clients(gint fd, GIOCondition cond, gpointer data) {
    Recorder *r = data;
    (void)cond;
    int client = accept4(fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client >= 0) g_unix_fd_add(client, G_IO_IN | G_IO_HUP | G_IO_ERR, handle_client, r);
    return G_SOURCE_CONTINUE;
}

static void on_activate(GtkApplication *app, gpointer user_data) {
    Recorder *r = user_data;
    g_unix_fd_add(r->control_fd, G_IO_IN, accept_clients, r);
    g_timeout_add_seconds(3600, safety_timeout, r);
    ui_start(r);
    g_application_hold(G_APPLICATION(app));
}

int recorder_worker(void) {
    Recorder r = {0};
    r.lock_fd = -1; r.control_fd = -1; r.ffmpeg_stdin = -1;
    r.runtime_dir = runtime_dir();
    r.envelopes = g_queue_new();
    r.encoder_queue = g_byte_array_new();
    r.recordings_dir = g_build_filename(g_get_home_dir(), "Music", "Recordings", NULL);
    if (g_mkdir_with_parents(r.runtime_dir, 0700) < 0) return 1;
    chmod(r.runtime_dir, 0700);
    char *lockpath = g_build_filename(r.runtime_dir, "lock", NULL);
    r.lock_fd = open(lockpath, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    g_free(lockpath);
    if (r.lock_fd < 0 || flock(r.lock_fd, LOCK_EX | LOCK_NB) < 0) return 1;
    storage_recover_stale(&r);

    char *sockpath = socket_path();
    unlink(sockpath);
    r.control_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (r.control_fd < 0) { perror("control socket"); return 1; }
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    g_strlcpy(addr.sun_path, sockpath, sizeof addr.sun_path);
    g_free(sockpath);
    if (bind(r.control_fd, (struct sockaddr *)&addr, sizeof addr) < 0 || listen(r.control_fd, 8) < 0) {
        perror("bind/listen"); return 1;
    }
    char *bound_path = socket_path();
    chmod(bound_path, 0600);
    g_free(bound_path);
    r.gtk_app = gtk_application_new("org.omarchy.DesktopAudioRecorder", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(r.gtk_app, "activate", G_CALLBACK(on_activate), &r);
    int status = g_application_run(G_APPLICATION(r.gtk_app), 0, NULL);
    ui_stop(&r);
    if (r.pulse_stream) pa_stream_unref(r.pulse_stream);
    if (r.pulse_context) pa_context_unref(r.pulse_context);
    if (r.pulse_glib) pa_glib_mainloop_free(r.pulse_glib);
    if (r.pulse_loop) pa_mainloop_free(r.pulse_loop);
    close(r.control_fd);
    unlink(bound_path);
    g_free(bound_path);
    if (r.lock_fd >= 0) close(r.lock_fd);
    g_free(r.runtime_dir); g_free(r.recordings_dir);
    g_byte_array_unref(r.encoder_queue); g_queue_free_full(r.envelopes, g_free);
    g_object_unref(r.gtk_app);
    return status;
}
