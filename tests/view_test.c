#include "../bedjet_view.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static BedjetModel connected(void) {
    BedjetModel m = {
        .screen = Control, .has_status = true, .limits_ready = true, .status_ms = 1000};
    m.status = (BedjetStatus){
        .mode = BedjetModeHeat,
        .target_step = 60,
        .fan_step = 9,
        .min_step = 38,
        .max_step = 86,
        .max_minutes = 240,
        .remaining_seconds = 9015};
    m.mode_choice = 1; /* Heat */
    m.timer_choice = 30;
    return m;
}

static void test_badges_distinguish_link_states(void) {
    BedjetModel m = connected();
    assert(bedjet_view_badge(&m, 1000) == BedjetBadgeLive);
    assert(bedjet_view_badge(&m, 1000 + BEDJET_STATUS_FRESH_MS) == BedjetBadgeStale);
    m.has_status = false;
    assert(bedjet_view_badge(&m, 1000) == BedjetBadgeNoData);
    m.diagnostics.rejected_since_ok = 3;
    assert(bedjet_view_badge(&m, 1000) == BedjetBadgeBadData);
    m.screen = Devices;
    assert(bedjet_view_badge(&m, 1000) == BedjetBadgeNone);
    assert(!strcmp(bedjet_view_badge_text(BedjetBadgeBadData), "BAD DATA"));
}

static void test_actual_line_units_and_remaining_time(void) {
    BedjetModel m = connected();
    char out[BEDJET_VIEW_LINE];
    bedjet_view_actual(&m, 1000, out, sizeof(out));
    assert(!strcmp(out, "Heat 86.0F 50% 2:30 left"));
    m.celsius = true;
    m.status.target_step = 61;
    bedjet_view_actual(&m, 1000, out, sizeof(out));
    assert(!strcmp(out, "Heat 30.5C 50% 2:30 left"));
    m.status.remaining_seconds = 0;
    bedjet_view_actual(&m, 1000, out, sizeof(out));
    assert(!strcmp(out, "Heat 30.5C 50%"));
    m.has_status = false;
    bedjet_view_actual(&m, 1000, out, sizeof(out));
    assert(!strcmp(out, "No status yet"));
}

static void test_rows_label_drafts_wants_and_locks(void) {
    BedjetModel m = connected();
    char out[BEDJET_VIEW_LINE];
    bedjet_view_row(&m, BedjetRowMode, out, sizeof(out));
    assert(!strcmp(out, "Mode: Heat (now)"));
    m.mode_choice = 0;
    bedjet_view_row(&m, BedjetRowMode, out, sizeof(out));
    assert(!strcmp(out, "Mode: Cool [OK]"));
    m.command = (BedjetCommand){
        .kind = BedjetCommandTemperature, .state = BedjetCommandQueued, .desired = 63};
    bedjet_view_row(&m, BedjetRowTemperature, out, sizeof(out));
    assert(!strcmp(out, "Temp want 88.7F"));
    m.command.state = BedjetCommandApplied;
    m.limits_ready = false;
    bedjet_view_row(&m, BedjetRowTemperature, out, sizeof(out));
    assert(strstr(out, "locked"));
    bedjet_view_row(&m, BedjetRowTimer, out, sizeof(out));
    assert(strstr(out, "locked"));
    bedjet_view_row(&m, BedjetRowOff, out, sizeof(out));
    assert(!strcmp(out, "Off: hold OK 1 s"));
    bedjet_view_row(&m, BedjetRowM3, out, sizeof(out));
    assert(!strcmp(out, "Run preset M3 [OK]"));
}

static void test_devices_and_info_pages(void) {
    BedjetModel m = connected();
    m.screen = Devices;
    m.count = 1;
    m.devices[0] = (BedjetDevice){
        .addr = {0xB2, 0xA1},
        .rssi = -61,
        .flags = BedjetDeviceService,
        .name = "BEDJET_V3_BEDROOM"};
    char out[BEDJET_VIEW_LINE];
    bedjet_view_device(&m, 0, out, sizeof(out));
    assert(!strcmp(out, "BEDJET_V3_B ..A1B2 -61dB"));
    m.devices[0].name[0] = '\0';
    bedjet_view_device(&m, 0, out, sizeof(out));
    assert(!strcmp(out, "BedJet ..A1B2 -61dB"));
    m.more_devices = true;
    bedjet_view_devices_title(&m, out, sizeof(out));
    assert(!strcmp(out, "1 unit (more nearby)"));
    /* Advanced scan: unidentified devices without a name read "unknown". */
    m.scan_all = true;
    m.devices[0].flags = 0;
    bedjet_view_device(&m, 0, out, sizeof(out));
    assert(!strcmp(out, "unknown ..A1B2 -61dB"));
    bedjet_view_devices_title(&m, out, sizeof(out));
    assert(!strcmp(out, "1 device (more nearby)"));
    m.scan_all = false;
    char lines[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE];
    m.diagnostics.frames_rejected = 2;
    m.diagnostics.last_reason = BedjetStatusMode;
    m.diagnostics.rejected_len = 12;
    for(unsigned i = 0; i < 12; i++)
        m.diagnostics.rejected[i] = (uint8_t)(0xF0 + i);
    assert(bedjet_view_info(&m, lines) == 3 && !strcmp(lines[1], "last bad: mode, 12 bytes"));
    m.info_page = 1;
    assert(bedjet_view_info(&m, lines) == 2);
    assert(!strcmp(lines[0], "F0 F1 F2 F3 F4 F5 F6 F7 F8 F9") && !strcmp(lines[1], "FA FB"));
    m.info_page = 2;
    assert(bedjet_view_info(&m, lines) == 1 && strstr(lines[0], "none yet"));
    m.info_page = 3;
    m.diagnostics.reads_ok = 1;
    m.diagnostics.read_len = 11;
    m.diagnostics.read_result = BedjetStatusLength;
    assert(bedjet_view_info(&m, lines) == 3 && !strcmp(lines[0], "read 11 B length, fail 0"));
}

int main(void) {
    test_badges_distinguish_link_states();
    test_actual_line_units_and_remaining_time();
    test_rows_label_drafts_wants_and_locks();
    test_devices_and_info_pages();
    puts("PASS: view labels, badges, units, devices and Info pages");
    return 0;
}
