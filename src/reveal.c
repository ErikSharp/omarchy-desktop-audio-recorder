#include "recorder.h"
#include <gio/gio.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>

int recorder_reveal(const char *path) {
    GFile *file = g_file_new_for_commandline_arg(path);
    if (!g_file_query_exists(file, NULL)) {
        char *parent = g_path_get_dirname(path);
        char *argv[] = {"xdg-open", parent, NULL};
        GError *error = NULL;
        gboolean ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                                    NULL, NULL, NULL, &error);
        if (!ok) fprintf(stderr, "File is missing and directory could not be opened: %s\n", error->message);
        g_clear_error(&error); g_free(parent); g_object_unref(file);
        return ok ? 0 : 1;
    }
    char *uri = g_file_get_uri(file);
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    gboolean shown = FALSE;
    if (bus) {
        const char *items[] = {uri, NULL};
        GVariant *reply = g_dbus_connection_call_sync(bus, "org.freedesktop.FileManager1",
            "/org/freedesktop/FileManager1", "org.freedesktop.FileManager1", "ShowItems",
            g_variant_new("(^ass)", items, ""), NULL, G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &error);
        shown = reply != NULL;
        if (reply) g_variant_unref(reply);
        g_object_unref(bus);
    }
    g_clear_error(&error);
    if (!shown) {
        char *args[] = {"nautilus", "--select", (char *)path, NULL};
        shown = g_spawn_async(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_STDOUT_TO_DEV_NULL,
                              NULL, NULL, NULL, &error);
        if (!shown) {
            g_clear_error(&error);
            char *parent = g_path_get_dirname(path);
            char *fallback[] = {"xdg-open", parent, NULL};
            shown = g_spawn_async(NULL, fallback, NULL, G_SPAWN_SEARCH_PATH,
                                  NULL, NULL, NULL, &error);
            g_free(parent);
        }
    }
    if (!shown) fprintf(stderr, "Could not reveal recording: %s\n", error ? error->message : "file manager unavailable");
    g_clear_error(&error); g_free(uri); g_object_unref(file);
    return shown ? 0 : 1;
}

int recorder_doctor(void) {
    const char *commands[] = {"pactl", "ffmpeg", "ffprobe", "flac", "omarchy", "nautilus", NULL};
    gboolean missing = FALSE;
    printf("Omarchy Desktop Audio Recorder capability check\n");
    for (size_t i = 0; commands[i]; ++i) {
        char *path = g_find_program_in_path(commands[i]);
        printf("%-8s %s\n", commands[i], path ? path : "missing");
        if (!path && strcmp(commands[i], "nautilus")) missing = TRUE;
        g_free(path);
    }
    const char *wayland = g_getenv("WAYLAND_DISPLAY");
    printf("Wayland  %s\n", wayland ? wayland : "not detected");
    char *argv[] = {"pactl", "get-default-sink", NULL};
    char *sink = NULL;
    GError *error = NULL;
    gint child_status = 0;
    if (g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, &sink, NULL, &child_status, &error) &&
        sink && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0 && *g_strstrip(sink)) {
        g_strstrip(sink); printf("Default output: %s\n", sink);
    } else {
        printf("Default output: unavailable (%s)\n", error ? error->message : "unknown error");
        missing = TRUE;
    }
    g_clear_error(&error); g_free(sink);
    printf("Channel policy: mono/stereo accepted; unqualified multichannel layouts are refused\n");
    return missing ? 1 : 0;
}
