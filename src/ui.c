#include "recorder.h"
#include <gtk4-layer-shell/gtk4-layer-shell.h>
#include <math.h>

typedef struct { double minv, maxv, db; } Envelope;

#define WAVEFORM_HISTORY_FRAMES 300U

static void draw(GtkDrawingArea *area, cairo_t *cr, int width, int height, gpointer data) {
    Recorder *r = data;
    GdkRGBA foreground = {0.90, 0.91, 0.92, 1.0};
    GdkRGBA background = {0.08, 0.09, 0.11, 0.86};
    GdkRGBA accent = {1.0, 0.58, 0.06, 1.0};
    gtk_widget_get_color(GTK_WIDGET(area), &foreground);
    GdkRGBA theme_accent = {0, 0, 0, 1};
    G_GNUC_BEGIN_IGNORE_DEPRECATIONS
    gboolean has_theme_accent = gtk_style_context_lookup_color(gtk_widget_get_style_context(GTK_WIDGET(area)),
                                                               "accent_color", &theme_accent);
    G_GNUC_END_IGNORE_DEPRECATIONS
    double accent_contrast = MAX(MAX(fabs(theme_accent.red - background.red),
                                     fabs(theme_accent.green - background.green)),
                                 fabs(theme_accent.blue - background.blue));
    if (has_theme_accent && accent_contrast > 0.35) accent = theme_accent;
    cairo_set_source_rgba(cr, background.red, background.green, background.blue, 0.86);
    cairo_rectangle(cr, 0, 0, width, height); cairo_fill(cr);
    cairo_select_font_face(cr, "sans-serif", CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, foreground.alpha);
    cairo_text_extents_t ext;
    cairo_text_extents(cr, "Desktop", &ext);
    cairo_move_to(cr, (width - ext.width) / 2.0 - ext.x_bearing, 15);
    cairo_show_text(cr, "Desktop");

    const double left = 0, top = 24, wave_w = width * 0.92, wave_h = height - top;
    const double wave_center = top + wave_h / 2.0;
    guint count = g_queue_get_length(r->envelopes);
    if (count) {
        guint index = 0;
        const guint first_slot = WAVEFORM_HISTORY_FRAMES - MIN(count, WAVEFORM_HISTORY_FRAMES);
        cairo_new_path(cr);
        for (GList *node = r->envelopes->head; node; node = node->next, ++index) {
            Envelope *e = node->data;
            guint slot = first_slot + index;
            double x = left + wave_w * slot / (WAVEFORM_HISTORY_FRAMES - 1);
            double upper = CLAMP(e->maxv * 1.6, -1.0, 1.0);
            double y = wave_center - upper * wave_h / 2.0;
            if (index == 0) cairo_move_to(cr, x, y); else cairo_line_to(cr, x, y);
        }
        for (GList *node = g_queue_peek_tail_link(r->envelopes); node; node = node->prev) {
            Envelope *e = node->data;
            guint reverse_index = --index;
            guint slot = first_slot + reverse_index;
            double x = left + wave_w * slot / (WAVEFORM_HISTORY_FRAMES - 1);
            double lower = CLAMP(e->minv * 1.6, -1.0, 1.0);
            double y = wave_center - lower * wave_h / 2.0;
            cairo_line_to(cr, x, y);
        }
        cairo_close_path(cr);
        cairo_set_source_rgba(cr, accent.red, accent.green, accent.blue, 0.94);
        cairo_fill(cr);
    }
    cairo_set_line_width(cr, 1);
    cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, 0.38);
    cairo_move_to(cr, left, wave_center); cairo_line_to(cr, left + wave_w, wave_center); cairo_stroke(cr);

    double meter_db = r->peak_dbfs;
    int lit = CLAMP((int)((meter_db + 60.0) / 6.0), 0, 10);
    const double mx = wave_w + 4, my = top + wave_h - 3;
    const double meter_w = width - mx;
    for (int i = 0; i < 10; ++i) {
        double y = my - i * 5;
        if (i < lit && i >= 8) cairo_set_source_rgba(cr, 0.95, 0.20, 0.16, 0.95);
        else if (i < lit && i >= 6) cairo_set_source_rgba(cr, 0.98, 0.68, 0.12, 0.95);
        else if (i < lit) cairo_set_source_rgba(cr, 0.24, 0.76, 0.37, 0.95);
        else cairo_set_source_rgba(cr, foreground.red, foreground.green, foreground.blue, 0.14);
        cairo_rectangle(cr, mx, y, meter_w, 3); cairo_fill(cr);
    }
}

static gboolean redraw(gpointer data) {
    Recorder *r = data;
    if (r->peak_dbfs > -120.0) r->peak_dbfs = MAX(-120.0, r->peak_dbfs - 0.096);
    if (r->drawing) gtk_widget_queue_draw(r->drawing);
    return G_SOURCE_CONTINUE;
}

static void position_like_voxtype(GtkWidget *widget, gpointer data) {
    (void)data;
    GdkDisplay *display = gtk_widget_get_display(widget);
    GdkSurface *surface = gtk_native_get_surface(GTK_NATIVE(widget));
    GdkMonitor *monitor = surface ? gdk_display_get_monitor_at_surface(display, surface) : NULL;
    if (!monitor) return;

    GdkRectangle geometry;
    gdk_monitor_get_geometry(monitor, &geometry);
    const int waveform_top = (int)lround(geometry.height * 0.85);
    /* The F9 card centers its 48 px waveform in a 72 px surface. Our 48 px
     * waveform follows a 24 px title row, so raise the surface by 12 px to
     * align the waveform rectangle itself. */
    const int overlay_top = MAX(24, waveform_top - 12);
    gtk_layer_set_anchor(GTK_WINDOW(widget), GTK_LAYER_SHELL_EDGE_BOTTOM, FALSE);
    gtk_layer_set_anchor(GTK_WINDOW(widget), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(widget), GTK_LAYER_SHELL_EDGE_TOP, overlay_top);
}

void ui_start(Recorder *r) {
    r->window = gtk_application_window_new(r->gtk_app);
    gtk_window_set_title(GTK_WINDOW(r->window), "Desktop Audio Recorder");
    gtk_window_set_default_size(GTK_WINDOW(r->window), 400, 72);
    gtk_window_set_resizable(GTK_WINDOW(r->window), FALSE);
    gtk_widget_set_can_target(r->window, FALSE);
    gtk_layer_init_for_window(GTK_WINDOW(r->window));
    gtk_layer_set_namespace(GTK_WINDOW(r->window), "omarchy-desktop-audio-recorder");
    gtk_layer_set_layer(GTK_WINDOW(r->window), GTK_LAYER_SHELL_LAYER_OVERLAY);
    gtk_layer_set_keyboard_mode(GTK_WINDOW(r->window), GTK_LAYER_SHELL_KEYBOARD_MODE_NONE);
    gtk_layer_set_exclusive_zone(GTK_WINDOW(r->window), 0);
    g_signal_connect(r->window, "map", G_CALLBACK(position_like_voxtype), NULL);
    gtk_layer_set_anchor(GTK_WINDOW(r->window), GTK_LAYER_SHELL_EDGE_TOP, TRUE);
    gtk_layer_set_margin(GTK_WINDOW(r->window), GTK_LAYER_SHELL_EDGE_TOP, 24);
    r->drawing = gtk_drawing_area_new();
    gtk_drawing_area_set_draw_func(GTK_DRAWING_AREA(r->drawing), draw, r, NULL);
    gtk_widget_set_can_target(r->drawing, FALSE);
    gtk_window_set_child(GTK_WINDOW(r->window), r->drawing);
    gtk_window_present(GTK_WINDOW(r->window));
    g_timeout_add(16, redraw, r);
}

void ui_stop(Recorder *r) {
    if (r->window) gtk_window_destroy(GTK_WINDOW(r->window));
    r->window = NULL; r->drawing = NULL;
}

void ui_push_frame(Recorder *r, double min_level, double max_level, double peak_dbfs) {
    Envelope *e = g_new0(Envelope, 1);
    e->minv = min_level; e->maxv = max_level; e->db = peak_dbfs;
    g_queue_push_tail(r->envelopes, e);
    while (g_queue_get_length(r->envelopes) > WAVEFORM_HISTORY_FRAMES)
        g_free(g_queue_pop_head(r->envelopes));
    r->peak_dbfs = peak_dbfs;
}
