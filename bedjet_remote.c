// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_controller.h"
#include "bedjet_flipper.h"
#include "bedjet_view.h"
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TAG "BedJet"

typedef struct {
    FuriMutex* mutex;
    BedjetModel model;
    BedjetController controller;
    BedjetFlipper driver;
    unsigned logged_rejections;
} App;

_Static_assert(sizeof(App) <= 4096, "Application state exceeds its fixed heap budget");

static uint32_t now_ms(void) {
    BedjetTransport transport = bedjet_flipper_transport();
    return transport.now_ms(NULL);
}

static void input_callback(InputEvent* event, void* context) {
    App* app = context;
    /* Interpretation (hold-to-Off, Long/Repeat steps, Back on Press) lives in
     * the portable controller; this only maps types and never takes the GUI
     * lock, so Back and Off never wait on drawing. */
    BedjetKey key;
    switch(event->key) {
    case InputKeyUp:
        key = BedjetUp;
        break;
    case InputKeyDown:
        key = BedjetDown;
        break;
    case InputKeyLeft:
        key = BedjetLeft;
        break;
    case InputKeyRight:
        key = BedjetRight;
        break;
    case InputKeyOk:
        key = BedjetOk;
        break;
    case InputKeyBack:
        key = BedjetBack;
        break;
    default:
        return;
    }
    BedjetInputType type;
    switch(event->type) {
    case InputTypePress:
        type = BedjetInputPress;
        break;
    case InputTypeRelease:
        type = BedjetInputRelease;
        break;
    case InputTypeShort:
        type = BedjetInputShort;
        break;
    case InputTypeLong:
        type = BedjetInputLong;
        break;
    case InputTypeRepeat:
        type = BedjetInputRepeat;
        break;
    default:
        return;
    }
    bedjet_controller_input(&app->controller, key, type, now_ms());
}

static void log_frame(const char* label, const char* detail, const uint8_t* bytes, unsigned n) {
    char hex[3 * 32 + 1];
    size_t used = 0;
    hex[0] = '\0';
    for(unsigned i = 0; i < n && i < 32; i++)
        used += snprintf(hex + used, sizeof(hex) - used, "%02X ", bytes[i]);
    FURI_LOG_I(TAG, "%s %s len=%u: %s", label, detail, n, hex);
}

/* Bring-up evidence on the debug log (CLI "log"): messages, status reads, the
 * first and mode-changing valid frames, and up to 32 rejected frames. */
static void log_changes(App* app, const BedjetModel* before, const BedjetModel* after) {
    const BedjetDiagnostics* b = &before->diagnostics;
    const BedjetDiagnostics* a = &after->diagnostics;
    if(strcmp(before->message, after->message)) FURI_LOG_I(TAG, "%s", after->message);
    if(a->frames_rejected < b->frames_rejected) app->logged_rejections = 0;
    if(a->frames_rejected > b->frames_rejected && app->logged_rejections < 32) {
        app->logged_rejections++;
        log_frame(
            "rejected",
            bedjet_status_result_name((BedjetStatusResult)a->last_reason),
            a->rejected,
            a->rejected_len);
    }
    if(a->reads_ok != b->reads_ok)
        log_frame(
            "read",
            bedjet_status_result_name((BedjetStatusResult)a->read_result),
            a->read,
            a->read_len);
    if(a->frames_ok > b->frames_ok && (!b->frames_ok || before->status.mode != after->status.mode))
        log_frame("status", bedjet_mode_name(after->status.mode), a->accepted, a->accepted_len);
    if(a->ble_status != b->ble_status) FURI_LOG_I(TAG, "BLE status 0x%02X", a->ble_status);
}

static int32_t worker(void* context) {
    App* app = context;
    for(;;) {
        bedjet_controller_step(&app->controller);
        /* Only this thread writes app->model, so reading it here is safe. */
        log_changes(app, &app->model, &app->controller.model);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model = app->controller.model;
        furi_mutex_release(app->mutex);
        if(app->controller.phase == BedjetDone) break;
        furi_delay_ms(20);
    }
    if(app->controller.model.cleanup_deferred)
        FURI_LOG_W(TAG, "Controller cleanup deferred; application callbacks detached");
    return 0;
}

/* Draw text clipped to the canvas width using the firmware's font metrics. */
static void draw_fit(Canvas* canvas, int32_t x, int32_t y, const char* text) {
    char line[BEDJET_VIEW_LINE + 4];
    snprintf(line, sizeof(line), "%s", text);
    size_t n = strlen(line);
    while(n && canvas_string_width(canvas, line) > (uint16_t)(128 - x))
        line[--n] = '\0';
    canvas_draw_str(canvas, x, y, line);
}

static void draw_lines(Canvas* canvas, const char* a, const char* b, const char* c) {
    draw_fit(canvas, 2, 24, a);
    draw_fit(canvas, 2, 36, b);
    draw_fit(canvas, 2, 48, c);
}

static void draw_control(Canvas* canvas, const BedjetModel* m, uint32_t time) {
    char line[BEDJET_VIEW_LINE + 4], text[BEDJET_VIEW_LINE];
    if(m->row == BedjetRowInfo) {
        bedjet_view_row(m, BedjetRowInfo, text, sizeof(text));
        snprintf(line, sizeof(line), "> %s", text);
        draw_fit(canvas, 2, 21, line);
        char info[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE];
        unsigned count = bedjet_view_info(m, info);
        for(unsigned i = 0; i < count; i++)
            draw_fit(canvas, 2, 32 + i * 10, info[i]);
        return;
    }
    bedjet_view_actual(m, time, text, sizeof(text));
    draw_fit(canvas, 2, 21, text);
    const unsigned first = (m->row / 3) * 3;
    for(unsigned i = first; i < first + 3 && i < BedjetRowCount; i++) {
        bedjet_view_row(m, i, text, sizeof(text));
        snprintf(line, sizeof(line), "%c %s", i == m->row ? '>' : ' ', text);
        draw_fit(canvas, 2, 32 + (i - first) * 10, line);
    }
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    /* Draw from the shared snapshot under its lock instead of copying the
     * model onto the GUI thread's 2 KiB stack. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const BedjetModel* m = &app->model;
    uint32_t time = now_ms();
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, m->screen == Control ? "BedJet" : "BedJet 3 Remote");
    canvas_set_font(canvas, FontSecondary);
    char corner[24];
    const char* badge = bedjet_view_badge_text(bedjet_view_badge(m, time));
    if(m->dropped_events)
        snprintf(
            corner,
            sizeof(corner),
            "!%lu %s",
            (unsigned long)(m->dropped_events > 999 ? 999 : m->dropped_events),
            badge);
    else
        snprintf(corner, sizeof(corner), "%s", badge);
    canvas_draw_str(canvas, 126 - canvas_string_width(canvas, corner), 10, corner);
    char line[BEDJET_VIEW_LINE + 4], text[BEDJET_VIEW_LINE];
    switch(m->screen) {
    case Control:
        draw_control(canvas, m, time);
        break;
    case Devices:
    case Scanning: {
        bedjet_view_devices_title(m, text, sizeof(text));
        draw_fit(canvas, 2, 22, text);
        const unsigned first = (m->selected / 3) * 3;
        for(unsigned i = first; i < first + 3 && i < m->count; i++) {
            bedjet_view_device(m, i, text, sizeof(text));
            snprintf(line, sizeof(line), "%c %s", i == m->selected ? '>' : ' ', text);
            draw_fit(canvas, 2, 33 + (i - first) * 10, line);
        }
        if(m->screen == Devices && !m->count) draw_fit(canvas, 2, 33, "Back, then OK to rescan");
        break;
    }
    case Unsupported:
        draw_lines(
            canvas,
            "Needs BLE central firmware",
            "and the Full BLE radio stack",
            "See docs/setup.md. Back: exit");
        break;
    case Connecting:
        bedjet_view_device(m, m->selected, text, sizeof(text));
        draw_lines(canvas, "Connecting to", text, "Back: cancel");
        break;
    case Closing:
        draw_lines(canvas, "Closing Bluetooth...", "", "");
        break;
    case Stopped:
        draw_lines(
            canvas,
            m->cleanup_deferred ? "BT cleanup deferred" : "Stopped",
            m->cleanup_deferred ? "Reboot Flipper if BT" : "",
            m->cleanup_deferred ? "misbehaves" : "");
        break;
    default:
        draw_lines(canvas, "1. Close BedJet phone app", "2. OK: scan for 8 seconds", "Back: exit");
        break;
    }
    /* Baseline 62 keeps descenders on the 64-row display. */
    draw_fit(canvas, 2, 62, m->message);
    furi_mutex_release(app->mutex);
}

int32_t bedjet_remote_app(void* context) {
    UNUSED(context);
    App* app = calloc(1, sizeof(App));
    app->mutex = furi_mutex_alloc(FuriMutexTypeNormal);
    BedjetTransport transport = bedjet_flipper_transport();
    furi_check(bedjet_controller_init(&app->controller, &transport, &app->driver));
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, draw_callback, app);
    view_port_input_callback_set(view, input_callback, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view, GuiLayerFullscreen);
    FuriThread* thread = furi_thread_alloc_ex("BedJetBLE", 4096, worker, app);
    furi_thread_start(thread);
    for(;;) {
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        bool stopped = app->model.screen == Stopped;
        bool deferred = app->model.cleanup_deferred;
        furi_mutex_release(app->mutex);
        view_port_update(view);
        if(stopped) {
            /* Leave a deferred-cleanup notice readable before exiting. */
            if(deferred) furi_delay_ms(2500);
            break;
        }
        furi_delay_ms(100);
    }
    furi_thread_join(thread);
    view_port_enabled_set(view, false);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    furi_thread_free(thread);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
