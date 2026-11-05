#include "recorder.h"
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static gboolean run_ok(char **argv, char **out) {
    GError *error = NULL;
    gint status = 0;
    gboolean ok = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH,
                               NULL, NULL, out, NULL, &status, &error);
    if (!ok) {
        g_clear_error(&error);
        return FALSE;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

gboolean storage_prepare(Recorder *r) {
    if (g_mkdir_with_parents(r->recordings_dir, 0755) < 0) {
        g_snprintf(r->error, sizeof r->error, "Cannot create %s: %s", r->recordings_dir, g_strerror(errno));
        return FALSE;
    }
    GDateTime *now = g_date_time_new_now_local();
    char *stamp = g_date_time_format(now, "%Y-%m-%d-%H-%M-%S");
    g_date_time_unref(now);
    for (unsigned i = 0; i < 1000; ++i) {
        char *base = i ? g_strdup_printf("Desktop-Audio-%s-%u.flac", stamp, i)
                       : g_strdup_printf("Desktop-Audio-%s.flac", stamp);
        char *final = g_build_filename(r->recordings_dir, base, NULL);
        char *partial_name = g_strdup_printf(".%s.%08x.partial", base, g_random_int());
        char *partial = g_build_filename(r->recordings_dir, partial_name, NULL);
        int fd = open(partial, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
        g_free(base); g_free(partial_name);
        if (fd >= 0) {
            close(fd); r->final_path = final; r->partial_path = partial;
            g_free(stamp);
            return TRUE;
        }
        g_free(final); g_free(partial);
        if (errno != EEXIST) break;
    }
    g_snprintf(r->error, sizeof r->error, "Cannot reserve output file: %s", g_strerror(errno));
    g_free(stamp);
    return FALSE;
}

gboolean storage_publish(Recorder *r) {
    char *test_argv[] = {"flac", "-t", "--silent", r->partial_path, NULL};
    if (!run_ok(test_argv, NULL)) {
        g_strlcpy(r->error, "FLAC integrity check failed; partial saved for recovery", sizeof r->error);
        return FALSE;
    }
    int fd = open(r->partial_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || fsync(fd) < 0) {
        g_snprintf(r->error, sizeof r->error, "Cannot flush recording: %s", g_strerror(errno));
        if (fd >= 0) close(fd);
        return FALSE;
    }
    close(fd);
    if (link(r->partial_path, r->final_path) < 0) {
        g_snprintf(r->error, sizeof r->error, "Cannot publish recording without overwrite: %s", g_strerror(errno));
        return FALSE;
    }
    if (unlink(r->partial_path) < 0) {
        g_warning("Recording is saved, but temporary cleanup failed: %s", r->partial_path);
    }
    int dirfd = open(r->recordings_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dirfd >= 0) { (void)fsync(dirfd); close(dirfd); }
    r->finalized = TRUE;
    return TRUE;
}

void storage_recover(Recorder *r) {
    if (!r->partial_path || !g_file_test(r->partial_path, G_FILE_TEST_EXISTS)) return;
    char *recovery = g_build_filename(r->recordings_dir, ".desktop-recorder-recovery", NULL);
    if (g_mkdir_with_parents(recovery, 0700) == 0) {
        char *base = g_path_get_basename(r->partial_path);
        char *unique = g_strdup_printf("%s-%08x", base, g_random_int());
        char *target = g_build_filename(recovery, unique, NULL);
        (void)rename(r->partial_path, target);
        char *note = g_strdup_printf("%s.error", target);
        g_file_set_contents(note, r->error[0] ? r->error : "Recording did not complete", -1, NULL);
        g_free(note); g_free(target); g_free(unique); g_free(base);
    }
    g_free(recovery);
}

void storage_recover_stale(Recorder *r) {
    GError *error = NULL;
    GRegex *pattern = g_regex_new("^\\.Desktop-Audio-[0-9]{4}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}-[0-9]{2}\\.flac\\.[0-9a-fA-F]{8}\\.partial$",
                                  G_REGEX_OPTIMIZE, 0, &error);
    if (!pattern) { g_clear_error(&error); return; }
    GDir *dir = g_dir_open(r->recordings_dir, 0, NULL);
    if (!dir) { g_regex_unref(pattern); return; }
    char *recovery = g_build_filename(r->recordings_dir, ".desktop-recorder-recovery", NULL);
    unsigned moved = 0;
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        if (!g_regex_match(pattern, name, 0, NULL)) continue;
        char *source = g_build_filename(r->recordings_dir, name, NULL);
        if (!g_file_test(source, G_FILE_TEST_IS_REGULAR)) { g_free(source); continue; }
        if (g_mkdir_with_parents(recovery, 0700) == 0) {
            char *unique = g_strdup_printf("%s-%08x", name, g_random_int());
            char *target = g_build_filename(recovery, unique, NULL);
            if (rename(source, target) == 0) {
                char *note = g_strdup_printf("%s.error", target);
                if (g_file_set_contents(note, "Previous recorder worker ended before finalization; file may be incomplete.", -1, NULL)) ++moved;
                g_free(note);
            }
            g_free(target); g_free(unique);
        }
        g_free(source);
    }
    if (moved) {
        char *body = g_strdup_printf("Moved %u incomplete recording file%s to the recovery folder.", moved, moved == 1 ? "" : "s");
        char *argv[] = {"omarchy", "notification", "send", "--app-name", "desktop-audio-recorder",
                        "--urgency", "normal", "Recorder recovery", body, NULL};
        if (!run_ok(argv, NULL)) g_warning("Could not report stale recorder files moved to recovery");
        g_free(body);
    }
    g_free(recovery); g_dir_close(dir); g_regex_unref(pattern);
}

void storage_notify(Recorder *r) {
    char *exe = g_file_read_link("/proc/self/exe", NULL);
    if (!exe) return;
    char *base = g_path_get_basename(r->final_path);
    char *body = r->output_changed
        ? g_strdup_printf("Output changed; recording ended: %s", base)
        : g_strdup_printf("Recording saved: %s", base);
    char *argv[] = {"omarchy", "notification", "send", "--app-name", "desktop-audio-recorder",
                    "--urgency", "normal", "Desktop Audio Recorded", body,
                    "--exec", exe, "reveal", r->final_path, NULL};
    if (!run_ok(argv, NULL)) g_warning("Recording saved, but Omarchy notification delivery failed");
    g_free(base); g_free(body); g_free(exe);
}

void storage_error_notify(Recorder *r) {
    if (!r->error[0]) return;
    char *argv[] = {"omarchy", "notification", "send", "--app-name", "desktop-audio-recorder",
                    "--urgency", "critical", "Desktop Audio Recorder", r->error, NULL};
    if (!run_ok(argv, NULL)) g_warning("Recorder failure notification could not be delivered: %s", r->error);
}
