#ifndef ODA_RECORDER_H
#define ODA_RECORDER_H

#include <gtk/gtk.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct Recorder Recorder;

struct Recorder {
    char *runtime_dir;
    char *recordings_dir;
    char *partial_path;
    char *final_path;
    char *sink_name;
    char *source_name;
    int lock_fd;
    int control_fd;
    int ffmpeg_stdin;
    GByteArray *encoder_queue;
    guint encoder_watch;
    guint finalize_watch;
    guint output_check_watch;
    pa_glib_mainloop *pulse_glib;
    pid_t ffmpeg_pid;
    pa_mainloop *pulse_loop;
    pa_context *pulse_context;
    pa_stream *pulse_stream;
    GtkApplication *gtk_app;
    GtkWidget *window;
    GtkWidget *drawing;
    GQueue *envelopes;
    gboolean stopping;
    gboolean failed;
    gboolean capture_ready;
    gboolean capture_started;
    gboolean silence;
    gboolean finalized;
    gboolean output_changed;
    gboolean failure_notified;
    uint64_t samples_written;
    uint8_t channels;
    uint32_t sink_index;
    uint32_t frame_seq;
    double peak_dbfs;
    char error[512];
};

int recorder_command(const char *command);
int recorder_worker(void);
int recorder_doctor(void);
int recorder_reveal(const char *path);

gboolean storage_prepare(Recorder *r);
gboolean storage_publish(Recorder *r);
void storage_recover(Recorder *r);
void storage_recover_stale(Recorder *r);
void storage_notify(Recorder *r);
void storage_error_notify(Recorder *r);

gboolean capture_start(Recorder *r);
void capture_stop(Recorder *r);
void capture_fail(Recorder *r, const char *message);

void ui_start(Recorder *r);
void ui_stop(Recorder *r);
void ui_push_frame(Recorder *r, double min_level, double max_level, double peak_dbfs);

#endif
