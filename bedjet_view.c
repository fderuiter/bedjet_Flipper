// SPDX-License-Identifier: GPL-3.0-or-later
#include "bedjet_view.h"
#include <stdio.h>
#include <string.h>

static const uint8_t mode_values[] = {4, 1, 5, 3}; /* Matches the controller's draft order. */

BedjetBadge bedjet_view_badge(const BedjetModel* m, uint32_t now_ms) {
    if(m->screen != Control) return BedjetBadgeNone;
    /* Several consecutive invalid frames mean the unit is talking but its
     * frames are not understood; showing old values as merely stale would hide
     * that. */
    if(m->diagnostics.rejected_since_ok >= 3) return BedjetBadgeBadData;
    if(!m->has_status) return BedjetBadgeNoData;
    return bedjet_controller_fresh(m, now_ms) ? BedjetBadgeLive : BedjetBadgeStale;
}

const char* bedjet_view_badge_text(BedjetBadge badge) {
    static const char* const text[] = {"", "LIVE", "STALE", "NO DATA", "BAD DATA"};
    return (unsigned)badge <= BedjetBadgeBadData ? text[badge] : "";
}

void bedjet_view_temperature(const BedjetModel* m, uint8_t step, char* out, size_t size) {
    /* One step is half a degree Celsius; the protocol never changes. */
    unsigned tenths = m->celsius ? step * 5u : bedjet_fahrenheit_tenths(step);
    snprintf(out, size, "%u.%u%c", tenths / 10, tenths % 10, m->celsius ? 'C' : 'F');
}

void bedjet_view_actual(const BedjetModel* m, uint32_t now_ms, char* out, size_t size) {
    BedjetBadge badge = bedjet_view_badge(m, now_ms);
    if(!m->has_status) {
        snprintf(
            out,
            size,
            "%s",
            badge == BedjetBadgeBadData ? "Status not understood" : "No status yet");
        return;
    }
    char temperature[12];
    bedjet_view_temperature(m, m->status.target_step, temperature, sizeof(temperature));
    int n = snprintf(
        out,
        size,
        "%s %s %u%%",
        bedjet_mode_name(m->status.mode),
        temperature,
        (m->status.fan_step + 1u) * 5u);
    uint32_t left = m->status.remaining_seconds;
    if(left && n > 0 && (size_t)n < size)
        snprintf(
            out + n,
            size - n,
            " %lu:%02lu left",
            (unsigned long)(left / 3600),
            (unsigned long)(left / 60 % 60));
}

static bool pending_kind(const BedjetModel* m, BedjetCommandKind kind) {
    return m->command.kind == kind &&
           (m->command.state == BedjetCommandPending || m->command.state == BedjetCommandQueued);
}

void bedjet_view_row(const BedjetModel* m, unsigned row, char* out, size_t size) {
    char value[12];
    switch(row) {
    case BedjetRowMode: {
        uint8_t draft = mode_values[m->mode_choice % sizeof(mode_values)];
        bool current = m->has_status && m->status.mode == draft;
        snprintf(out, size, "Mode: %s %s", bedjet_mode_name(draft), current ? "(now)" : "[OK]");
        break;
    }
    case BedjetRowTemperature:
        if(pending_kind(m, BedjetCommandTemperature)) {
            bedjet_view_temperature(m, (uint8_t)m->command.desired, value, sizeof(value));
            snprintf(out, size, "Temp want %s", value);
        } else if(!m->limits_ready)
            snprintf(out, size, "Temp locked: wait status");
        else
            snprintf(out, size, "Temperature  < >");
        break;
    case BedjetRowFan:
        if(pending_kind(m, BedjetCommandFan))
            snprintf(out, size, "Fan want %u%%", (m->command.desired + 1u) * 5u);
        else
            snprintf(out, size, "Fan speed  < >");
        break;
    case BedjetRowTimer:
        if(!m->limits_ready)
            snprintf(out, size, "Timer locked: wait status");
        else
            snprintf(out, size, "Set timer %um [OK]", m->timer_choice);
        break;
    case BedjetRowM1:
    case BedjetRowM2:
    case BedjetRowM3:
        snprintf(out, size, "Run preset M%u [OK]", row - BedjetRowM1 + 1);
        break;
    case BedjetRowOff:
        snprintf(out, size, "Off: hold OK 1 s");
        break;
    case BedjetRowUnits:
        snprintf(out, size, "Units: %s [OK]", m->celsius ? "Celsius" : "Fahrenheit");
        break;
    case BedjetRowInfo:
        snprintf(out, size, "Info %u/%u  OK: refresh", m->info_page + 1u, BEDJET_INFO_PAGES);
        break;
    default:
        snprintf(out, size, "%s", "");
        break;
    }
}

void bedjet_view_devices_title(const BedjetModel* m, char* out, size_t size) {
    snprintf(
        out,
        size,
        "%u unit%s%s%s",
        m->count,
        m->count == 1 ? "" : "s",
        m->more_devices ? " (more nearby)" : "",
        m->screen == Scanning ? " scanning..." : "");
}

void bedjet_view_device(const BedjetModel* m, unsigned index, char* out, size_t size) {
    if(index >= m->count) {
        snprintf(out, size, "%s", "");
        return;
    }
    const BedjetDevice* d = &m->devices[index];
    /* The address suffix separates two units with the same name. */
    snprintf(
        out,
        size,
        "%.11s ..%02X%02X %ddB",
        d->name[0] ? d->name : "BedJet",
        d->addr[1],
        d->addr[0],
        d->rssi);
}

static void hex_line(char* out, const uint8_t* bytes, unsigned from, unsigned length) {
    /* Ten bytes fit the 128 px canvas in the secondary font. */
    size_t n = 0;
    out[0] = '\0';
    for(unsigned i = from; i < length && i < from + 10; i++)
        n += snprintf(out + n, BEDJET_VIEW_LINE - n, i == from ? "%02X" : " %02X", bytes[i]);
}

static unsigned hex_page(
    char lines[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE],
    const char* label,
    const uint8_t* bytes,
    unsigned length) {
    if(!length) {
        snprintf(lines[0], BEDJET_VIEW_LINE, "%s: none yet", label);
        return 1;
    }
    unsigned count = 0;
    for(unsigned from = 0; from < length && count < BEDJET_VIEW_INFO_LINES; from += 10)
        hex_line(lines[count++], bytes, from, length);
    return count;
}

unsigned
    bedjet_view_info(const BedjetModel* m, char lines[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE]) {
    const BedjetDiagnostics* d = &m->diagnostics;
    switch(m->info_page) {
    case 0:
        snprintf(
            lines[0],
            BEDJET_VIEW_LINE,
            "ok %lu bad %lu late %lu",
            (unsigned long)d->frames_ok,
            (unsigned long)d->frames_rejected,
            (unsigned long)d->frames_late);
        if(d->frames_rejected)
            snprintf(
                lines[1],
                BEDJET_VIEW_LINE,
                "last bad: %s, %u bytes",
                bedjet_status_result_name((BedjetStatusResult)d->last_reason),
                d->rejected_len);
        else
            snprintf(lines[1], BEDJET_VIEW_LINE, "no rejected frames");
        snprintf(
            lines[2],
            BEDJET_VIEW_LINE,
            "drop %lu big %lu BLE %02X",
            (unsigned long)m->dropped_events,
            (unsigned long)d->frames_oversize,
            d->ble_status);
        return 3;
    case 1:
        return hex_page(lines, "Bad frame", d->rejected, d->rejected_len);
    case 2:
        return hex_page(lines, "Good frame", d->accepted, d->accepted_len);
    default:
        if(!d->reads_ok && !d->reads_failed) {
            snprintf(lines[0], BEDJET_VIEW_LINE, "No status reads yet");
            return 1;
        }
        snprintf(
            lines[0],
            BEDJET_VIEW_LINE,
            "read %u B %s, fail %lu",
            d->read_len,
            bedjet_status_result_name((BedjetStatusResult)d->read_result),
            (unsigned long)d->reads_failed);
        hex_line(lines[1], d->read, 0, d->read_len);
        hex_line(lines[2], d->read, 10, d->read_len);
        return 3;
    }
}
