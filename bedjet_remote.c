#include <furi.h>
#include <furi_hal.h>
#include <gui/gui.h>
#include <input/input.h>
#include <furi_ble/central.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bedjet_protocol.h"

#define DEVICE_COUNT    6
#define BLE_TIMEOUT     4000
#define STATUS_FRESH_MS 15000

typedef enum {
    Home,
    Scanning,
    Devices,
    Connecting,
    Control,
    Unsupported
} Screen;
typedef enum {
    KeyEvent,
    Advertisement,
    Notification,
    Quit
} EventType;
typedef struct {
    EventType type;
    InputKey key;
    BleCentralAdvReport report;
    uint16_t handle;
    uint8_t length;
    uint8_t data[32];
    uint32_t tick;
} Event;
typedef struct {
    Screen screen;
    char message[64];
    BleCentralAdvReport devices[DEVICE_COUNT];
    uint8_t count;
    uint8_t selected;
    uint8_t row;
    uint8_t mode_choice;
    uint16_t timer_choice;
    bool connected;
    bool has_status;
    BedjetStatus status;
    uint32_t status_tick;
} Model;
typedef struct {
    FuriMutex* mutex;
    FuriMessageQueue* events;
    FuriMessageQueue* input;
    Model model;
    uint16_t status_handle;
    uint16_t command_handle;
    bool initialized;
    bool was_advertising;
} App;

/* App logic is confined to the worker; BLE callbacks only copy to a bounded
 * queue. The GUI reads a locked snapshot and never calls the BLE API. */
static void publish(App* app, const Model* model) {
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    app->model = *model;
    furi_mutex_release(app->mutex);
}

static bool fresh(const Model* m) {
    return m->has_status && furi_get_tick() - m->status_tick < furi_ms_to_ticks(STATUS_FRESH_MS);
}

static bool matches(const BleCentralAdvReport* r) {
    const uint8_t* value;
    uint8_t length;
    if(ble_central_adv_find(r, 0x09, &value, &length) ||
       ble_central_adv_find(r, 0x08, &value, &length)) {
        const char* name = "BEDJET";
        bool match = length >= 6;
        for(uint8_t i = 0; match && i < 6; i++) {
            uint8_t c = value[i];
            if(c >= 'a' && c <= 'z') c -= 'a' - 'A';
            match = c == (uint8_t)name[i];
        }
        if(match) return true;
    }
    for(uint8_t type = 0x06; type <= 0x07; type++) {
        if(ble_central_adv_find(r, type, &value, &length)) {
            for(unsigned i = 0; i + 16 <= length; i += 16)
                if(memcmp(value + i, bedjet_service_uuid, 16) == 0) return true;
        }
    }
    return false;
}

static void scan_callback(const BleCentralAdvReport* report, void* context) {
    App* app = context;
    if(!matches(report)) return;
    Event e = {.type = Advertisement, .report = *report};
    furi_message_queue_put(app->events, &e, 0);
}

static void notify_callback(uint16_t handle, const uint8_t* data, uint16_t len, void* context) {
    App* app = context;
    if(len < 18 || len > 32) return;
    Event e = {
        .type = Notification, .handle = handle, .length = (uint8_t)len, .tick = furi_get_tick()};
    memcpy(e.data, data, len);
    furi_message_queue_put(app->events, &e, 0);
}

static void input_callback(InputEvent* event, void* context) {
    App* app = context;
    if(event->type == InputTypeShort || event->type == InputTypeRepeat)
        furi_message_queue_put(app->input, event, 0);
}

static void disconnect_device(App* app, Model* m) {
    ble_central_set_notify_callback(NULL, NULL);
    if(ble_central_is_scanning() && !ble_central_scan_stop()) {
        snprintf(m->message, sizeof(m->message), "Scan stop failed; retry Back");
        return;
    }
    if(ble_central_is_connected() && !ble_central_disconnect()) {
        snprintf(m->message, sizeof(m->message), "Disconnect pending; retry Back");
        return;
    }
    app->status_handle = 0;
    app->command_handle = 0;
    m->connected = false;
    m->has_status = false;
    m->screen = Home;
    snprintf(m->message, sizeof(m->message), "OK: scan for your BedJet");
}

static void connect_device(App* app, Model* m) {
    if(m->selected >= m->count) return;
    if(!ble_central_scan_stop()) {
        snprintf(m->message, sizeof(m->message), "Cannot stop scan; retry");
        return;
    }
    m->screen = Connecting;
    snprintf(m->message, sizeof(m->message), "Connecting; press unit pair");
    publish(app, m);
    const BleCentralAdvReport* target = &m->devices[m->selected];
    bool ok = ble_central_connect(target->addr, target->addr_type, 8000);
    uint16_t cccd = 0;
    if(ok) {
        snprintf(m->message, sizeof(m->message), "Discovering BedJet service");
        publish(app, m);
        ok = ble_central_find_characteristic_128(
            bedjet_service_uuid, bedjet_command_uuid, &app->command_handle, NULL, BLE_TIMEOUT);
    }
    if(ok)
        ok = ble_central_find_characteristic_128(
            bedjet_service_uuid, bedjet_status_uuid, &app->status_handle, &cccd, BLE_TIMEOUT);
    if(ok && cccd) {
        ble_central_set_notify_callback(notify_callback, app);
        ok = ble_central_subscribe(cccd, BLE_TIMEOUT);
    } else {
        ok = false;
    }
    if(!ok) {
        uint8_t code = ble_central_last_status();
        disconnect_device(app, m);
        snprintf(m->message, sizeof(m->message), "Link/setup failed: 0x%02X", code);
        return;
    }
    m->connected = true;
    m->has_status = false;
    m->screen = Control;
    m->row = 0;
    m->mode_choice = 0;
    m->timer_choice = 30;
    snprintf(m->message, sizeof(m->message), "Waiting for live status");
    /* Read may return a shorter device-status format while off. The decoder
     * ignores it. Mode and Off remain available without live telemetry. */
    uint8_t bytes[32], length = 0;
    if(ble_central_read(app->status_handle, bytes, sizeof(bytes), &length, BLE_TIMEOUT) &&
       bedjet_status_decode(bytes, length, &m->status)) {
        m->has_status = true;
        m->status_tick = furi_get_tick();
    }
}

static void send_command(App* app, Model* m, const uint8_t* bytes, size_t length) {
    if(!m->connected || !ble_central_is_connected() || !app->command_handle) {
        snprintf(m->message, sizeof(m->message), "Disconnected; reconnect first");
        return;
    }
    if(length == 0) {
        snprintf(m->message, sizeof(m->message), "Outside device limits");
        return;
    }
    /* Match ESPHome's Write Without Response transport. Controller acceptance
     * is not proof that BedJet applied a command. Actual settings are updated
     * exclusively by valid device status packets. */
    bool ok = ble_central_write(app->command_handle, bytes, (uint8_t)length, false, BLE_TIMEOUT);
    if(ok)
        snprintf(m->message, sizeof(m->message), "Queued; check device status");
    else
        snprintf(
            m->message, sizeof(m->message), "Write failed: 0x%02X", ble_central_last_status());
}

static const uint8_t mode_buttons[] = {2, 3, 5, 6}; /* Cool, Heat, Dry, Extended Heat */
static const char* const mode_labels[] = {"Cool", "Heat", "Dry", "Ext heat"};

static void control_key(App* app, Model* m, InputKey key) {
    if(key == InputKeyUp) {
        m->row = (m->row + 7) % 8;
        return;
    }
    if(key == InputKeyDown) {
        m->row = (m->row + 1) % 8;
        return;
    }
    int delta = key == InputKeyRight ? 1 : key == InputKeyLeft ? -1 : 0;
    uint8_t bytes[3];
    size_t length = 0;
    if(m->row == 0) {
        if(delta) m->mode_choice = (m->mode_choice + 4 + delta) % 4;
        if(key == InputKeyOk) length = bedjet_button(mode_buttons[m->mode_choice], bytes);
    } else if(m->row == 7 && key == InputKeyOk) {
        length = bedjet_button(1, bytes);
    } else if(m->row >= 4 && m->row <= 6 && key == InputKeyOk) {
        length = bedjet_button(0x20 + m->row - 4, bytes);
    } else if(m->row >= 1 && m->row <= 3) {
        if(!fresh(m)) {
            snprintf(m->message, sizeof(m->message), "Need fresh device status");
            return;
        }
        if(m->row == 1 && delta) {
            int step = m->status.target_step + delta;
            if(step < m->status.min_step || step > m->status.max_step) return;
            length = bedjet_temperature(step, m->status.min_step, m->status.max_step, bytes);
        } else if(m->row == 2 && delta) {
            int step = m->status.fan_step + delta;
            if(step < 0 || step > 19) return;
            length = bedjet_fan(step, bytes);
        } else if(m->row == 3) {
            int max = m->status.max_minutes;
            if(max > 600) max = 600;
            int choice = m->timer_choice + delta * 5;
            if(choice > max) choice = max;
            if(choice < 1) choice = 1;
            m->timer_choice = choice;
            if(key == InputKeyOk) length = bedjet_timer(m->timer_choice, max, bytes);
        }
    }
    if(length) send_command(app, m, bytes, length);
}

static int32_t worker(void* context) {
    App* app = context;
    Model m = {.screen = Home, .timer_choice = 30};
    if(!furi_hal_bt_is_central_supported()) {
        m.screen = Unsupported;
        snprintf(m.message, sizeof(m.message), "Requires Full BLE radio stack");
    } else {
        ble_central_init();
        app->initialized = true;
        snprintf(m.message, sizeof(m.message), "OK: scan for your BedJet");
    }
    publish(app, &m);
    uint32_t scan_tick = 0;
    bool running = true;
    while(running) {
        Event e;
        if(furi_message_queue_get(app->events, &e, furi_ms_to_ticks(100)) == FuriStatusOk) {
            if(e.type == Quit) break;
            if(m.screen == Unsupported) continue;
            if(e.type == Advertisement && m.screen == Scanning) {
                unsigned i;
                for(i = 0; i < m.count; i++)
                    if(m.devices[i].addr_type == e.report.addr_type &&
                       memcmp(m.devices[i].addr, e.report.addr, 6) == 0)
                        break;
                if(i < DEVICE_COUNT) {
                    m.devices[i] = e.report;
                    if(i == m.count) m.count++;
                }
            } else if(e.type == Notification && m.connected && e.handle == app->status_handle) {
                if(bedjet_status_decode(e.data, e.length, &m.status)) {
                    m.has_status = true;
                    m.status_tick = e.tick;
                    snprintf(m.message, sizeof(m.message), "Live device status");
                }
            } else if(e.type == KeyEvent) {
                if(e.key == InputKeyBack) {
                    disconnect_device(app, &m);
                } else if(m.screen == Home && e.key == InputKeyOk) {
                    app->was_advertising |= furi_hal_bt_is_advertising();
                    if(furi_hal_bt_is_advertising()) furi_hal_bt_stop_advertising();
                    /* GAP stop is asynchronous; let its thread finish before scanning. */
                    for(unsigned i = 0; i < 25 && furi_hal_bt_is_advertising(); i++)
                        furi_delay_ms(20);
                    m.count = 0;
                    m.selected = 0;
                    m.has_status = false;
                    if(ble_central_scan_start(scan_callback, app)) {
                        m.screen = Scanning;
                        scan_tick = furi_get_tick();
                        snprintf(m.message, sizeof(m.message), "Scanning for 8 seconds...");
                    } else
                        snprintf(
                            m.message,
                            sizeof(m.message),
                            "Scan failed: 0x%02X",
                            ble_central_last_status());
                } else if(m.screen == Devices && m.count) {
                    if(e.key == InputKeyUp) m.selected = (m.selected + m.count - 1) % m.count;
                    if(e.key == InputKeyDown) m.selected = (m.selected + 1) % m.count;
                    if(e.key == InputKeyOk) connect_device(app, &m);
                } else if(m.screen == Control)
                    control_key(app, &m, e.key);
            }
        }
        if(m.screen == Scanning && furi_get_tick() - scan_tick > furi_ms_to_ticks(8000)) {
            if(ble_central_scan_stop()) {
                m.screen = Devices;
                snprintf(
                    m.message,
                    sizeof(m.message),
                    m.count ? "Select unit; OK to connect" : "No units. Back to try again");
            } else {
                snprintf(m.message, sizeof(m.message), "Scan stop failed; retrying");
            }
        }
        if(m.connected && !ble_central_is_connected()) {
            disconnect_device(app, &m);
            snprintf(m.message, sizeof(m.message), "Connection lost. OK: rescan");
        }
        publish(app, &m);
    }
    if(app->initialized) {
        ble_central_set_notify_callback(NULL, NULL);
        /* deinit detaches callback contexts even when controller teardown is
         * deferred. Retry briefly; no application pointer survives this. */
        for(unsigned attempt = 0; attempt < 3; attempt++) {
            if(ble_central_deinit()) break;
            furi_delay_ms(100);
        }
        if(app->was_advertising) furi_hal_bt_start_advertising();
    }
    return 0;
}

static void draw_callback(Canvas* canvas, void* context) {
    App* app = context;
    Model m;
    furi_mutex_acquire(app->mutex, FuriWaitForever);
    m = app->model;
    furi_mutex_release(app->mutex);
    canvas_clear(canvas);
    canvas_set_font(canvas, FontPrimary);
    canvas_draw_str(canvas, 2, 10, "BedJet 3 Remote");
    canvas_set_font(canvas, FontSecondary);
    char line[64];
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
            else if(i == 1)
                snprintf(value, sizeof(value), "Temperature  < / >");
            else if(i == 2)
                snprintf(value, sizeof(value), "Fan speed    < / >");
            else if(i == 3)
                snprintf(value, sizeof(value), "Timer: %um [OK]", m.timer_choice);
            else if(i < 7)
                snprintf(value, sizeof(value), "Run M%u [OK]", i - 3);
            else
                snprintf(value, sizeof(value), "TURN OFF [OK]");
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
            m.screen == Connecting ? "Setting up Bluetooth..." : "Back: exit / disconnect");
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
    app->events = furi_message_queue_alloc(24, sizeof(Event));
    app->input = furi_message_queue_alloc(8, sizeof(InputEvent));
    ViewPort* view = view_port_alloc();
    view_port_draw_callback_set(view, draw_callback, app);
    view_port_input_callback_set(view, input_callback, app);
    Gui* gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(gui, view, GuiLayerFullscreen);
    FuriThread* thread = furi_thread_alloc_ex("BedJetBLE", 4096, worker, app);
    furi_thread_start(thread);
    bool running = true;
    while(running) {
        InputEvent key;
        if(furi_message_queue_get(app->input, &key, furi_ms_to_ticks(100)) == FuriStatusOk) {
            furi_mutex_acquire(app->mutex, FuriWaitForever);
            Screen screen = app->model.screen;
            furi_mutex_release(app->mutex);
            if(key.key == InputKeyBack && (screen == Home || screen == Unsupported))
                running = false;
            else if(key.type == InputTypeShort || key.key == InputKeyLeft || key.key == InputKeyRight) {
                Event e = {.type = KeyEvent, .key = key.key};
                furi_message_queue_put(app->events, &e, 0);
            }
        }
        view_port_update(view);
    }
    Event quit = {.type = Quit};
    furi_message_queue_put(app->events, &quit, FuriWaitForever);
    furi_thread_join(thread);
    view_port_enabled_set(view, false);
    gui_remove_view_port(gui, view);
    view_port_free(view);
    furi_record_close(RECORD_GUI);
    furi_thread_free(thread);
    furi_message_queue_free(app->input);
    furi_message_queue_free(app->events);
    furi_mutex_free(app->mutex);
    free(app);
    return 0;
}
