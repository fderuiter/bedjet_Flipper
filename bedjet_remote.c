// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_controller.h"
#include "bedjet_flipper.h"
#include <furi.h>
#include <gui/gui.h>
#include <input/input.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    FuriMutex* mutex;
    BedjetModel model;
    BedjetController controller;
    BedjetFlipper driver;
} App;

_Static_assert(sizeof(App) <= 4096, "Application state exceeds its fixed heap budget");

static const char* const mode_labels[] = {"Cool", "Heat", "Dry", "Ext heat"};
static bool fresh(const BedjetModel* model) {
    BedjetTransport transport = bedjet_flipper_transport();
    return bedjet_controller_fresh(model, transport.now_ms(NULL));
}

static void input_callback(InputEvent* event, void* context) {
    App* app = context;
    uint32_t session = bedjet_controller_session(&app->controller);
    /* Press, rather than Short, also handles a held Back. Neither priority
     * action passes through the lossy ordinary inbox or waits on the GUI lock. */
    if(event->key == InputKeyBack && event->type == InputTypePress) {
        bedjet_controller_post_key(&app->controller, session, BedjetBack);
        return;
    }
    if(event->key == InputKeyOk && event->type == InputTypeLong) {
        bedjet_controller_post_key(&app->controller, session, BedjetOff);
        return;
    }
    if(event->type != InputTypeShort && event->type != InputTypeRepeat) return;
    if(event->type == InputTypeRepeat && event->key != InputKeyLeft && event->key != InputKeyRight)
        return;
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
        return;
    default:
        return;
    }
    bedjet_controller_post_key(&app->controller, session, key);
}

static int32_t worker(void* context) {
    App* app = context;
    for(;;) {
        bedjet_controller_step(&app->controller);
        furi_mutex_acquire(app->mutex, FuriWaitForever);
        app->model = app->controller.model;
        furi_mutex_release(app->mutex);
        if(app->controller.phase == BedjetDone) break;
        furi_delay_ms(20);
    }
    if(app->controller.model.cleanup_deferred)
        FURI_LOG_W("BedJet", "Controller cleanup deferred; application callbacks detached");
    return 0;
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    BedjetModel m;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    m = app->model;
    furi_mutex_release(app->mutex);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BedJet 3 Remote");
    canvas_set_font(canvas, FontSecondary);
    char line[64];
    if(m.dropped_events) {
        snprintf(
            line,
            sizeof(line),
            "!%lu",
            (unsigned long)(m.dropped_events > 999 ? 999 : m.dropped_events));
        canvas_draw_str(canvas, 103, 10, line);
    }
    if(m.screen == Control) {
        if(m.has_status) {
            unsigned f = bedjet_fahrenheit_tenths(m.status.target_step);
            snprintf(
                line,
                sizeof(line),
                "%s  %u.%uF  %u%% %s",
                bedjet_mode_name(m.status.mode),
                f / 10,
                f % 10,
                (m.status.fan_step + 1) * 5,
                fresh(&m) ? "" : "STALE");
        } else
            snprintf(line, sizeof(line), "Connected; no live status");
        canvas_draw_str(canvas, 2, 21, line);
        const unsigned first = (m.row / 3) * 3;
        for(unsigned i = first; i < first + 3 && i < 8; i++) {
            char value[40];
            if(i == 0)
                snprintf(value, sizeof(value), "Mode: %s [OK]", mode_labels[m.mode_choice]);
            else if(i == 1) {
                if(m.command.kind == BedjetCommandTemperature &&
                   (m.command.state == BedjetCommandPending ||
                    m.command.state == BedjetCommandQueued)) {
                    unsigned f = bedjet_fahrenheit_tenths(m.command.desired);
                    snprintf(value, sizeof(value), "Temp want: %u.%uF", f / 10, f % 10);
                } else
                    snprintf(
                        value,
                        sizeof(value),
                        "%s",
                        m.limits_ready ? "Temperature < / >" : "Temp locked: mode status");
            } else if(i == 2) {
                if(m.command.kind == BedjetCommandFan &&
                   (m.command.state == BedjetCommandPending ||
                    m.command.state == BedjetCommandQueued))
                    snprintf(value, sizeof(value), "Fan want: %u%%", (m.command.desired + 1) * 5);
                else
                    snprintf(value, sizeof(value), "Fan speed    < / >");
            } else if(i == 3)
                snprintf(
                    value,
                    sizeof(value),
                    "Timer: %um %s",
                    m.timer_choice,
                    m.limits_ready ? "[OK]" : "locked");
            else if(i < 7)
                snprintf(value, sizeof(value), "Run M%u [OK]", i - 3);
            else
                snprintf(value, sizeof(value), "TURN OFF: hold OK");
            snprintf(line, sizeof(line), "%c %s", i == m.row ? '>' : ' ', value);
            canvas_draw_str(canvas, 2, 32 + (i - first) * 10, line);
        }
    } else if(m.screen == Devices || m.screen == Scanning) {
        snprintf(
            line,
            sizeof(line),
            "%u unit(s)%s",
            m.count,
            m.screen == Scanning ? " scanning..." : "");
        canvas_draw_str(canvas, 2, 22, line);
        for(unsigned i = 0; i < m.count && i < 3; i++) {
            unsigned index = m.selected >= 3 ? i + 3 : i;
            if(index >= m.count) break;
            snprintf(
                line,
                sizeof(line),
                "%c BedJet ..%02X%02X %ddB",
                index == m.selected ? '>' : ' ',
                m.devices[index].addr[1],
                m.devices[index].addr[0],
                m.devices[index].rssi);
            canvas_draw_str(canvas, 2, 33 + i * 10, line);
        }
    } else {
        canvas_draw_str(canvas, 2, 25, "Experimental BLE remote");
        canvas_draw_str(
            canvas,
            2,
            37,
            m.screen == Unsupported ? "PR #4436 + Full stack" : "Close the phone BedJet app");
        canvas_draw_str(
            canvas,
            2,
            49,
            m.screen == Closing    ? "Closing Bluetooth..." :
            m.screen == Connecting ? "Setting up Bluetooth..." :
                                     "Back: exit / disconnect");
    }
    /* Small font: display a bounded prefix; full controller errors are also
     * visible as short messages in this footer. */
    m.message[30] = '\0';
    canvas_draw_str(canvas, 2, 63, m.message);
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
        furi_mutex_release(app->mutex);
        if(stopped) break;
        view_port_update(view);
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
