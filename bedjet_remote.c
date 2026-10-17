// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_controller.h"
#include "bedjet_log.h"
#include "bedjet_flipper.h"
#include "bedjet_view.h"
#include <furi.h>
#include <gui/elements.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    FuriMutex* mutex;
    BedjetModel model;
    BedjetController controller;
    BedjetFlipper driver;
    unsigned logged_rejections;
    BedjetPhase logged_phase;
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
    BJ_LOG_I("%s %s len=%u: %s", label, detail, n, hex);
}

static const char* phase_name(BedjetPhase phase) {
    static const char* const names[] = {
        "boot",
        "idle",
        "pause advertising",
        "scan",
        "connect",
        "find command",
        "find status",
        "subscribe",
        "initial read",
        "ready",
        "disconnecting",
        "stopping",
        "done"};
    return (unsigned)phase < COUNT_OF(names) ? names[phase] : "?";
}
static const char* screen_name(BedjetScreen screen) {
    static const char* const names[] = {
        "Home", "Scanning", "Devices", "Connecting", "Control", "Unsupported", "Closing", "Stopped"};
    return (unsigned)screen < COUNT_OF(names) ? names[screen] : "?";
}
static const char* command_kind_name(BedjetCommandKind kind) {
    static const char* const names[] = {
        "none", "mode", "temperature", "fan", "timer", "memory", "off"};
    return (unsigned)kind < COUNT_OF(names) ? names[kind] : "?";
}
static const char* command_state_name(BedjetCommandState state) {
    static const char* const names[] = {
        "idle",
        "queued",
        "pending",
        "applied",
        "rejected",
        "timed out",
        "conflict",
        "cancelled",
        "unverified"};
    return (unsigned)state < COUNT_OF(names) ? names[state] : "?";
}

/* Bring-up evidence on the debug log (CLI "log"): messages, state changes,
 * units found, commands, status reads, the first and mode-changing valid
 * frames, up to 32 rejected frames, and every counter that moves. */
static void log_changes(App* app, const BedjetModel* before, const BedjetModel* after) {
    const BedjetDiagnostics* b = &before->diagnostics;
    const BedjetDiagnostics* a = &after->diagnostics;
    BedjetController* c = &app->controller;
    if(c->phase != app->logged_phase) {
        BJ_LOG_I("Phase %s -> %s", phase_name(app->logged_phase), phase_name(c->phase));
        if(app->logged_phase == BedjetScan)
            BJ_LOG_I(
                "Scan reports: %u matched a BedJet, %u filtered, %u refused; %u units listed%s",
                atomic_load(&c->scan_matched),
                atomic_load(&c->scan_filtered),
                atomic_load(&c->scan_refused),
                after->count,
                after->more_devices ? " (more seen)" : "");
        if(app->logged_phase == BedjetScan) {
            for(unsigned i = 0; i < after->count; i++) {
                const BedjetDevice* d = &after->devices[i];
                BJ_LOG_I(
                    "Unit %u: \"%s\" ..%02X:%02X type %u, RSSI %d,%s%s %s",
                    i + 1,
                    d->name,
                    d->addr[1],
                    d->addr[0],
                    d->addr_type,
                    d->rssi,
                    d->flags & BedjetDeviceService ? " service UUID," : "",
                    d->flags & BedjetDeviceNamed ? " BEDJET name," : "",
                    d->flags & BedjetDeviceConnectable ? "connectable" : "not seen connectable");
            }
        }
        app->logged_phase = c->phase;
    }
    if(before->screen != after->screen)
        BJ_LOG_I("Screen %s -> %s", screen_name(before->screen), screen_name(after->screen));
    if(strcmp(before->message, after->message)) BJ_LOG_I("%s", after->message);
    if(before->connected != after->connected)
        BJ_LOG_I("Model %s", after->connected ? "connected" : "disconnected");
    if(before->has_status != after->has_status)
        BJ_LOG_I("Status %s", after->has_status ? "received" : "cleared");
    if(before->limits_ready != after->limits_ready && after->limits_ready)
        BJ_LOG_I("Device limits ready");
    if(before->command.kind != after->command.kind ||
       before->command.state != after->command.state ||
       strcmp(before->command.result, after->command.result))
        BJ_LOG_I(
            "Command %s %s: desired %u, sent %u%s%s",
            command_kind_name(after->command.kind),
            command_state_name(after->command.state),
            after->command.desired,
            after->command.sent,
            after->command.result[0] ? ", " : "",
            after->command.result);
    if(after->dropped_events != before->dropped_events)
        BJ_LOG_W("Inbox dropped events: %lu", (unsigned long)after->dropped_events);
    if(a->frames_late != b->frames_late)
        BJ_LOG_W("Late frames: %lu", (unsigned long)a->frames_late);
    if(a->frames_oversize != b->frames_oversize)
        BJ_LOG_W("Oversized frames: %lu", (unsigned long)a->frames_oversize);
    if(a->reads_failed != b->reads_failed)
        BJ_LOG_W(
            "Status read failed (%lu so far): %s",
            (unsigned long)a->reads_failed,
            bedjet_status_result_name((BedjetStatusResult)a->read_result));
    if(after->cleanup_deferred && !before->cleanup_deferred)
        BJ_LOG_W("Bluetooth cleanup deferred");
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
    if(a->ble_status != b->ble_status) BJ_LOG_I("BLE status 0x%02X", a->ble_status);
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
        BJ_LOG_W("Controller cleanup deferred; application callbacks detached");
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

/* Layout: a 12 px header (title, link badge, rule), content from y 14 to
 * 51, and the bottom row for button hints or the status message. */
static void draw_header(Canvas* canvas, const BedjetModel* m, uint32_t time, const char* title) {
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, title);
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
    canvas_draw_line(canvas, 0, 12, 127, 12);
}

static void draw_centered(Canvas* canvas, int32_t y, const char* text) {
    char line[BEDJET_VIEW_LINE + 4];
    snprintf(line, sizeof(line), "%s", text);
    size_t n = strlen(line);
    while(n && canvas_string_width(canvas, line) > 124)
        line[--n] = '\0';
    canvas_draw_str_aligned(canvas, 64, y, AlignCenter, AlignBottom, line);
}

static void draw_lines(Canvas* canvas, const char* a, const char* b, const char* c) {
    draw_centered(canvas, 26, a);
    draw_centered(canvas, 37, b);
    draw_centered(canvas, 48, c);
}

/* Four rising bars, filled up to the signal level. */
static void draw_signal(Canvas* canvas, int32_t x, int32_t y, int8_t rssi) {
    unsigned bars = bedjet_view_signal_bars(rssi);
    for(unsigned i = 0; i < 4; i++) {
        int32_t height = 2 + (int32_t)i * 2;
        if(i < bars)
            canvas_draw_box(canvas, x + (int32_t)i * 3, y - height, 2, height);
        else
            canvas_draw_dot(canvas, x + (int32_t)i * 3, y - 1);
    }
}

/* One selectable row: inverted when selected, text clipped to `width`. */
static void draw_row(Canvas* canvas, int32_t y, int32_t width, bool selected, const char* text) {
    if(selected) {
        canvas_draw_rbox(canvas, 0, y - 9, width, 11, 2);
        canvas_set_color(canvas, ColorWhite);
    }
    char line[BEDJET_VIEW_LINE + 4];
    snprintf(line, sizeof(line), "%s", text);
    size_t n = strlen(line);
    while(n && canvas_string_width(canvas, line) > (uint16_t)(width - 5))
        line[--n] = '\0';
    canvas_draw_str(canvas, 3, y, line);
    canvas_set_color(canvas, ColorBlack);
}

static void draw_home(Canvas* canvas, const BedjetModel* m) {
    draw_centered(canvas, 25, "Hold the Flipper close");
    draw_centered(canvas, 35, "to the BedJet; close its app");
    if(m->message[0]) draw_centered(canvas, 47, m->message);
    elements_button_left(canvas, "Exit");
    elements_button_center(canvas, "Scan");
    elements_button_right(canvas, "All");
}

static void draw_devices(Canvas* canvas, const BedjetModel* m, uint32_t time) {
    char text[BEDJET_VIEW_LINE];
    if(m->screen == Scanning) {
        unsigned percent = bedjet_view_percent(m->stage_ms, time, BEDJET_SCAN_MS);
        snprintf(
            text, sizeof(text), "%s %u found", m->scan_all ? "All devices:" : "BedJets:", m->count);
        elements_progress_bar(canvas, 2, 15, 124, percent / 100.0f);
        draw_fit(canvas, 2, 31, text);
        for(unsigned i = 0; i < m->count && i < 2; i++) {
            snprintf(text, sizeof(text), "%s", bedjet_view_device_name(&m->devices[i]));
            draw_fit(canvas, 8, 42 + i * 10, text);
            draw_signal(canvas, 112, 42 + i * 10, m->devices[i].rssi);
        }
        return;
    }
    if(!m->count) {
        draw_lines(
            canvas,
            m->scan_all ? "Nothing heard" : "No BedJet found",
            "Move the Flipper closer",
            "Back, then scan again");
        return;
    }
    const unsigned first = (m->selected / 3) * 3;
    for(unsigned i = first; i < first + 3 && i < m->count; i++) {
        const BedjetDevice* d = &m->devices[i];
        int32_t y = 23 + (int32_t)(i - first) * 11;
        snprintf(
            text,
            sizeof(text),
            "%s%.12s ..%02X%02X",
            m->scan_all && (d->flags & (BedjetDeviceService | BedjetDeviceNamed)) ? "* " : "",
            bedjet_view_device_name(d),
            d->addr[1],
            d->addr[0]);
        draw_row(canvas, y, 108, i == m->selected, text);
        draw_signal(canvas, 111, y + 1, d->rssi);
    }
    if(m->count > 3) elements_scrollbar(canvas, m->selected, m->count);
    elements_button_center(canvas, "Connect");
}

static void draw_connecting(Canvas* canvas, const BedjetModel* m, uint32_t time) {
    char text[BEDJET_VIEW_LINE];
    const BedjetDevice* d = &m->devices[m->selected];
    snprintf(
        text, sizeof(text), "%s ..%02X%02X", bedjet_view_device_name(d), d->addr[1], d->addr[0]);
    draw_centered(canvas, 22, text);
    snprintf(
        text,
        sizeof(text),
        "Attempt %u of %u",
        m->attempt ? m->attempt : 1u,
        (unsigned)BEDJET_LINK_ATTEMPTS);
    draw_centered(canvas, 31, text);
    unsigned percent = bedjet_view_percent(m->stage_ms, time, BEDJET_CONNECT_TIMEOUT_MS);
    elements_progress_bar(canvas, 14, 33, 100, percent / 100.0f);
    elements_button_left(canvas, "Cancel");
}

static void draw_control(Canvas* canvas, const BedjetModel* m, uint32_t time) {
    char text[BEDJET_VIEW_LINE];
    if(m->row == BedjetRowInfo) {
        bedjet_view_row(m, BedjetRowInfo, text, sizeof(text));
        draw_row(canvas, 23, 124, true, text);
        char info[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE];
        unsigned count = bedjet_view_info(m, info);
        for(unsigned i = 0; i < count; i++)
            draw_fit(canvas, 2, 34 + i * 10, info[i]);
        return;
    }
    bedjet_view_actual(m, time, text, sizeof(text));
    draw_centered(canvas, 22, text);
    const unsigned first = (m->row / 3) * 3;
    for(unsigned i = first; i < first + 3 && i < BedjetRowCount; i++) {
        bedjet_view_row(m, i, text, sizeof(text));
        draw_row(canvas, 33 + (int32_t)(i - first) * 10, 122, i == m->row, text);
    }
    elements_scrollbar(canvas, m->row, BedjetRowCount);
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    /* Draw from the shared snapshot under its lock instead of copying the
     * model onto the GUI thread's 2 KiB stack. */
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    const BedjetModel* m = &app->model;
    uint32_t time = now_ms();
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    const char* title = m->screen == Control    ? "BedJet" :
                        m->screen == Scanning   ? "Scanning" :
                        m->screen == Devices    ? (m->scan_all ? "Devices" : "BedJets") :
                        m->screen == Connecting ? "Connecting" :
                                                  "FredJet";
    draw_header(canvas, m, time, title);
    bool message_line = true;
    switch(m->screen) {
    case Control:
        draw_control(canvas, m, time);
        break;
    case Scanning:
    case Devices:
        draw_devices(canvas, m, time);
        message_line = m->screen == Scanning;
        break;
    case Connecting:
        draw_connecting(canvas, m, time);
        message_line = false;
        draw_centered(canvas, 51, m->message);
        break;
    case Unsupported:
        draw_lines(
            canvas,
            "Needs BLE central firmware",
            "and the Full BLE radio stack",
            "See docs/setup.md. Back: exit");
        break;
    case Closing:
        draw_lines(canvas, "", "Closing Bluetooth...", "");
        break;
    case Stopped:
        draw_lines(
            canvas,
            m->cleanup_deferred ? "BT cleanup deferred" : "",
            m->cleanup_deferred ? "Reboot Flipper if BT" : "Stopped",
            m->cleanup_deferred ? "misbehaves" : "");
        break;
    default:
        draw_home(canvas, m);
        message_line = false;
        break;
    }
    /* Baseline 62 keeps descenders on the 64-row display. */
    if(message_line) draw_fit(canvas, 2, 62, m->message);
    furi_mutex_release(app->mutex);
}

int32_t fredjet_app(void* context) {
    UNUSED(context);
    App* app = calloc(1, sizeof(App));
    bedjet_log_open();
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
    bedjet_log_close();
    return 0;
}
