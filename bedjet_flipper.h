// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include "bedjet_controller.h"

/* Only the adapter includes Flipper/BLE headers. Callback fields are updated
 * only after the underlying API's synchronous callback-drain boundary. */
typedef struct {
    BedjetScanSink scan;
    void* scan_context;
    BedjetNotifySink notify;
    void* notify_context;
    /* Diagnostic counts of every report the controller delivered during the
     * current scan, before any filtering; written only by the scan callback
     * and read after scan_stop has drained it. */
    uint32_t scan_reports;
    uint32_t scan_responses;
    uint32_t scan_oversized;
    /* Reports whose raw bytes contain "BEDJET" or the service UUID, found
     * without the controller's AD parser, and the first one's bytes. */
    uint32_t scan_bedjet_raw;
    uint8_t scan_first_type;
    uint8_t scan_first_len;
    uint8_t scan_first[31];
    /* Written by the notification callback; read by the worker for logs. */
    uint32_t notifications;
    /* Worker-only: last polled states, so polls log only on change. */
    bool seen_advertising, seen_scanning, seen_connected;
    uint8_t seen_status;
    uint32_t connected_ms;
} BedjetFlipper;

BedjetTransport bedjet_flipper_transport(void);
