#include "recorder.h"
#include <errno.h>
#include <fcntl.h>
#include <glib-unix.h>
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *userdata);
static void source_info(pa_context *c, const pa_source_info *i, int eol, void *userdata);
static gboolean finalize_later(gpointer data);
static gboolean encoder_writable(gint fd, GIOCondition cond, gpointer data);

static gboolean check_default_output(gpointer data) {
    Recorder *r = data;
    char *argv[] = {"pactl", "get-default-sink", NULL};
    char *out = NULL;
    gint status = 0;
    GError *error = NULL;
    gboolean spawned = g_spawn_sync(NULL, argv, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL,
                                   &out, NULL, &status, &error);
    g_clear_error(&error);
    if (!spawned || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !out) {
        capture_fail(r, "Audio server was lost while recording");
        r->output_check_watch = 0; g_free(out); return G_SOURCE_REMOVE;
    }
    g_strstrip(out);
    gboolean changed = r->sink_name && strcmp(out, r->sink_name) != 0;
    g_free(out);
    if (changed) {
        r->output_changed = TRUE;
        g_strlcpy(r->error, "Default output changed; the captured prefix was saved", sizeof r->error);
        r->stopping = TRUE;
        r->output_check_watch = 0;
        capture_stop(r);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static gboolean enqueue_encoder(Recorder *r, const uint8_t *data, size_t length) {
    if (r->encoder_queue->len + length > 4 * 1024 * 1024) {
        capture_fail(r, "PCM encoder queue overflow; recording stopped");
        return FALSE;
    }
    gboolean was_empty = r->encoder_queue->len == 0;
    g_byte_array_append(r->encoder_queue, data, (guint)length);
    if (was_empty && r->ffmpeg_stdin >= 0 && !r->encoder_watch)
        r->encoder_watch = g_unix_fd_add(r->ffmpeg_stdin, G_IO_OUT | G_IO_HUP | G_IO_ERR, encoder_writable, r);
    return TRUE;
}

static gboolean encoder_writable(gint fd, GIOCondition cond, gpointer data) {
    Recorder *r = data;
    if (cond & (G_IO_ERR | G_IO_HUP)) {
        capture_fail(r, "FLAC encoder closed its input unexpectedly");
        return G_SOURCE_REMOVE;
    }
    if (r->encoder_queue->len) {
        ssize_t n = write(fd, r->encoder_queue->data, r->encoder_queue->len);
        if (n > 0) g_byte_array_remove_range(r->encoder_queue, 0, (guint)n);
        else if (n < 0 && errno != EAGAIN && errno != EINTR) {
            capture_fail(r, "Could not write captured PCM to FLAC encoder");
            return G_SOURCE_REMOVE;
        }
    }
    if (!r->encoder_queue->len) {
        r->encoder_watch = 0;
        if (r->stopping) {
            close(r->ffmpeg_stdin); r->ffmpeg_stdin = -1;
            if (!r->finalize_watch) r->finalize_watch = g_timeout_add(50, finalize_later, r);
        }
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

static void read_stream(pa_stream *stream, size_t length, void *userdata) {
    Recorder *r = userdata;
    const void *data = NULL;
    size_t nbytes = 0;
    double minv = 0, maxv = 0, peak = 0;
    gboolean had_data = FALSE;
    while (pa_stream_peek(stream, &data, &nbytes) == 0) {
        if (nbytes == 0) break;
        had_data = TRUE;
        if (!data) {
            static const uint8_t zero[65536] = {0};
            size_t left = nbytes;
            while (left) {
                size_t part = MIN(left, sizeof zero);
                if (!enqueue_encoder(r, zero, part)) break;
                left -= part;
            }
        } else {
            if (!enqueue_encoder(r, data, nbytes)) { pa_stream_drop(stream); return; }
            const uint8_t *samples = data;
            size_t count = nbytes / 3;
            for (size_t k = 0; k < count; ++k) {
                uint32_t raw = samples[k * 3] | ((uint32_t)samples[k * 3 + 1] << 8) |
                               ((uint32_t)samples[k * 3 + 2] << 16);
                int32_t signed_value = (raw & 0x800000) ? (int32_t)(raw | 0xff000000) : (int32_t)raw;
                double value = signed_value / 8388608.0;
                minv = MIN(minv, value); maxv = MAX(maxv, value); peak = MAX(peak, fabs(value));
            }
        }
        r->samples_written += nbytes / (3 * MAX(r->channels, 1));
        pa_stream_drop(stream);
    }
    if (had_data) ui_push_frame(r, minv, maxv, peak > 0 ? 20.0 * log10(peak) : -120.0);
    (void)length;
}

static void stream_state(pa_stream *stream, void *userdata) {
    Recorder *r = userdata;
    pa_stream_state_t state = pa_stream_get_state(stream);
    if (state == PA_STREAM_READY) {
        if (r->stopping) { capture_stop(r); return; }
        r->capture_ready = TRUE;
        if (!r->output_check_watch) r->output_check_watch = g_timeout_add_seconds(1, check_default_output, r);
    } else if (state == PA_STREAM_FAILED || state == PA_STREAM_TERMINATED) {
        if (!r->stopping) capture_fail(r, "Desktop audio monitor disconnected");
    }
}

static void start_stream(Recorder *r, const pa_source_info *source) {
    if (r->stopping) return;
    if (!source) { capture_fail(r, "Default output monitor was not found"); return; }
    if (source->monitor_of_sink == PA_INVALID_INDEX || source->monitor_of_sink != r->sink_index) {
        capture_fail(r, "Selected source is not a sink monitor; microphone capture refused"); return;
    }
    if (source->sample_spec.channels < 1 || source->sample_spec.channels > 2) {
        g_snprintf(r->error, sizeof r->error,
                   "Unsupported %u-channel output layout; refusing unsafe FLAC channel mapping",
                   source->sample_spec.channels);
        r->failed = TRUE; r->stopping = TRUE; capture_stop(r); return;
    }
    pa_sample_spec spec = { .format = PA_SAMPLE_S24LE, .rate = source->sample_spec.rate,
                            .channels = source->sample_spec.channels };
    r->channels = spec.channels;
    pa_channel_map map = source->channel_map;
    if (!pa_sample_spec_valid(&spec) || !pa_channel_map_valid(&map)) {
        capture_fail(r, "Output monitor has an unsupported sample format or channel map"); return;
    }
    if ((spec.channels == 1 && map.map[0] != PA_CHANNEL_POSITION_MONO) ||
        (spec.channels == 2 && (map.map[0] != PA_CHANNEL_POSITION_FRONT_LEFT ||
                                map.map[1] != PA_CHANNEL_POSITION_FRONT_RIGHT))) {
        capture_fail(r, "Output channel positions are not a tested FLAC mono/stereo layout"); return;
    }
    char rate[16], channels[16];
    g_snprintf(rate, sizeof rate, "%u", spec.rate);
    g_snprintf(channels, sizeof channels, "%u", spec.channels);
    char *args[] = {"ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y", "-f", "s24le",
                    "-ar", rate, "-ac", channels, "-i", "pipe:0", "-c:a", "flac",
                    "-compression_level", "5", "-f", "flac", r->partial_path, NULL};
    gint child_in = -1;
    GError *error = NULL;
    if (!g_spawn_async_with_pipes(NULL, args, NULL, G_SPAWN_SEARCH_PATH | G_SPAWN_DO_NOT_REAP_CHILD,
                                  NULL, NULL, &r->ffmpeg_pid, &child_in, NULL, NULL, &error)) {
        g_snprintf(r->error, sizeof r->error, "Cannot start FFmpeg: %s", error->message);
        g_clear_error(&error); r->failed = TRUE; r->stopping = TRUE; capture_stop(r); return;
    }
    r->ffmpeg_stdin = child_in;
    fcntl(child_in, F_SETFL, fcntl(child_in, F_GETFL) | O_NONBLOCK);
    r->source_name = g_strdup(source->name);
    uint32_t frame_bytes = (uint32_t)(spec.rate / 100) * spec.channels * 3;
    pa_buffer_attr attr = { .maxlength = (uint32_t)-1, .fragsize = frame_bytes,
                            .prebuf = (uint32_t)-1, .minreq = (uint32_t)-1, .tlength = (uint32_t)-1 };
    r->pulse_stream = pa_stream_new(r->pulse_context, "Desktop Audio Recorder", &spec, &map);
    if (!r->pulse_stream) { capture_fail(r, "Cannot create PulseAudio monitor stream"); return; }
    pa_stream_set_state_callback(r->pulse_stream, stream_state, r);
    pa_stream_set_read_callback(r->pulse_stream, read_stream, r);
    pa_stream_flags_t flags = PA_STREAM_DONT_MOVE | PA_STREAM_NO_REMAP_CHANNELS |
                              PA_STREAM_NO_REMIX_CHANNELS | PA_STREAM_ADJUST_LATENCY;
    if (pa_stream_connect_record(r->pulse_stream, source->name, &attr, flags) < 0)
        capture_fail(r, "Cannot connect to the selected output monitor");
}

static void source_info(pa_context *c, const pa_source_info *i, int eol, void *userdata) {
    Recorder *r = userdata;
    (void)c;
    if (eol < 0) { capture_fail(r, "Cannot inspect the default output monitor"); return; }
    if (eol > 0) return;
    if (i && r->source_name && !strcmp(i->name, r->source_name)) start_stream(r, i);
}

static void sink_info(pa_context *c, const pa_sink_info *i, int eol, void *userdata) {
    Recorder *r = userdata;
    if (eol < 0) { capture_fail(r, "Cannot inspect the default output device"); return; }
    if (eol > 0 || !i) return;
    r->sink_name = g_strdup(i->name);
    r->sink_index = i->index;
    if (!i->monitor_source_name) { capture_fail(r, "Default output has no monitor source"); return; }
    r->source_name = g_strdup(i->monitor_source_name);
    pa_context_get_source_info_by_name(c, i->monitor_source_name, source_info, r);
}

static void server_info(pa_context *c, const pa_server_info *i, void *userdata) {
    Recorder *r = userdata;
    if (!i || !i->default_sink_name) { capture_fail(r, "No default audio output is available"); return; }
    pa_context_get_sink_info_by_name(c, i->default_sink_name, sink_info, r);
}

static void context_state(pa_context *c, void *userdata) {
    Recorder *r = userdata;
    pa_context_state_t state = pa_context_get_state(c);
    if (state == PA_CONTEXT_READY && !r->stopping)
        pa_context_get_server_info(c, server_info, r);
    else if (!r->stopping && (state == PA_CONTEXT_FAILED || state == PA_CONTEXT_TERMINATED))
        capture_fail(r, "Could not connect to the PipeWire PulseAudio server");
}

gboolean capture_start(Recorder *r) {
    if (!storage_prepare(r)) { r->failed = TRUE; r->stopping = TRUE; return FALSE; }
    r->pulse_glib = pa_glib_mainloop_new(g_main_context_default());
    if (!r->pulse_glib) { capture_fail(r, "Could not initialize PulseAudio event loop"); return FALSE; }
    r->pulse_loop = NULL;
    r->pulse_context = pa_context_new(pa_glib_mainloop_get_api(r->pulse_glib), "Omarchy Desktop Audio Recorder");
    if (!r->pulse_context) { capture_fail(r, "Could not create PulseAudio context"); return FALSE; }
    pa_context_set_state_callback(r->pulse_context, context_state, r);
    if (pa_context_connect(r->pulse_context, NULL, PA_CONTEXT_NOFLAGS, NULL) < 0) {
        capture_fail(r, "Could not connect to PipeWire PulseAudio"); return FALSE;
    }
    return TRUE;
}

void capture_fail(Recorder *r, const char *message) {
    if (!r->failed) g_strlcpy(r->error, message, sizeof r->error);
    r->failed = TRUE; r->stopping = TRUE;
    capture_stop(r);
}

static gboolean finalize_later(gpointer data) {
    Recorder *r = data;
    r->finalize_watch = 0;
    capture_stop(r);
    return G_SOURCE_REMOVE;
}

void capture_stop(Recorder *r) {
    if (r->pulse_stream) {
        pa_stream_set_read_callback(r->pulse_stream, NULL, NULL);
        pa_stream_disconnect(r->pulse_stream);
    }
    if (r->ffmpeg_stdin >= 0 && !r->encoder_watch) {
        close(r->ffmpeg_stdin); r->ffmpeg_stdin = -1;
    }
    if (!r->capture_ready && r->ffmpeg_pid == 0) {
        if (r->pulse_context) pa_context_disconnect(r->pulse_context);
        if (r->partial_path) unlink(r->partial_path);
        r->finalized = TRUE;
    }
    if (r->encoder_queue && !r->encoder_queue->len && r->ffmpeg_stdin < 0 && r->ffmpeg_pid > 0) {
        int status = 0;
        pid_t waited = waitpid(r->ffmpeg_pid, &status, WNOHANG);
        if (waited == 0 || (waited < 0 && errno == EINTR)) {
            if (!r->finalize_watch) r->finalize_watch = g_timeout_add(50, finalize_later, r);
            return;
        }
        r->finalize_watch = 0;
        if (waited < 0) {
            g_snprintf(r->error, sizeof r->error, "Could not verify FLAC encoder completion: %s", g_strerror(errno));
            r->failed = TRUE;
        }
        g_spawn_close_pid(r->ffmpeg_pid); r->ffmpeg_pid = 0;
        if (!r->failed && WIFEXITED(status) && WEXITSTATUS(status) == 0 && r->samples_written) {
            if (storage_publish(r)) storage_notify(r);
            else { r->failed = TRUE; storage_recover(r); }
        } else {
            if (!r->error[0]) g_strlcpy(r->error, "Recording stopped before a valid FLAC was written", sizeof r->error);
            r->failed = TRUE; storage_recover(r);
        }
        r->finalized = TRUE;
    }
    if (!r->ffmpeg_pid && (r->finalized || r->failed)) {
        if (r->output_check_watch) { g_source_remove(r->output_check_watch); r->output_check_watch = 0; }
        if (r->failed && !r->finalized) storage_recover(r);
        if (r->failed && !r->failure_notified) { storage_error_notify(r); r->failure_notified = TRUE; }
        r->finalized = TRUE;
        if (r->pulse_context) pa_context_disconnect(r->pulse_context);
        if (r->gtk_app) g_application_quit(G_APPLICATION(r->gtk_app));
    }
}
