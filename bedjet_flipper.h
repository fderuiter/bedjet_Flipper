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
} BedjetFlipper;

BedjetTransport bedjet_flipper_transport(void);
