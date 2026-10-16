// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include "bedjet_controller.h"

/* Portable display text derived only from the model, so labels can be tested
 * on a host. The GUI fits each line to the 128x64 canvas with the firmware's
 * own font metrics. Observed values stay authoritative: "want" values and
 * drafts are always labelled as such. */

#define BEDJET_VIEW_LINE       64u
#define BEDJET_VIEW_INFO_LINES 3u

typedef enum {
    BedjetBadgeNone,
    BedjetBadgeLive, /* valid status within the freshness window */
    BedjetBadgeStale, /* last valid status is too old to act on */
    BedjetBadgeNoData, /* connected, no valid status yet (a unit in standby may not notify) */
    BedjetBadgeBadData /* frames arrive but none validate: capture them (Info page) */
} BedjetBadge;

BedjetBadge bedjet_view_badge(const BedjetModel* model, uint32_t now_ms);
const char* bedjet_view_badge_text(BedjetBadge badge);
/* Target-style temperature in the selected display unit, e.g. "86.0F" or "30.0C". */
void bedjet_view_temperature(const BedjetModel* model, uint8_t step, char* out, size_t size);
/* Actual device state, e.g. "Heat 86.0F 50% 2:30 left". */
void bedjet_view_actual(const BedjetModel* model, uint32_t now_ms, char* out, size_t size);
void bedjet_view_row(const BedjetModel* model, unsigned row, char* out, size_t size);
void bedjet_view_device(const BedjetModel* model, unsigned index, char* out, size_t size);
void bedjet_view_devices_title(const BedjetModel* model, char* out, size_t size);
/* Lines for the selected Info page; returns how many were written. */
unsigned bedjet_view_info(
    const BedjetModel* model,
    char lines[BEDJET_VIEW_INFO_LINES][BEDJET_VIEW_LINE]);
