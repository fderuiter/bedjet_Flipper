#include "bedjet_protocol.h"

#define UUID_BYTES(a, b) \
    0x74, 0x65, 0x4a, 0x64, 0x65, 0x42, 0x55, 0xaa, 0x80, 0x00, 0xd0, 0xbe, a, b, 0x00, 0x00
const uint8_t bedjet_service_uuid[16] = {UUID_BYTES(0x00, 0x10)};
const uint8_t bedjet_status_uuid[16] = {UUID_BYTES(0x00, 0x20)};
const uint8_t bedjet_command_uuid[16] = {UUID_BYTES(0x04, 0x20)};

BedjetStatusResult bedjet_status_parse(const uint8_t* d, size_t n, BedjetStatus* out) {
    if(!d || !out || n < 18 || n > 32) return BedjetStatusLength;
    if(d[0] > 1 || d[1] != 0x56 || d[3] != 1) return BedjetStatusHeader;
    /* A complete frame (flag 0) must be exactly the declared length; a partial
     * notification (flag 1) may stop early. */
    if(d[2] < 18 || d[2] > 32 || n > d[2] || (d[0] == 0 && n < d[2])) return BedjetStatusDeclared;
    if(d[4] > 24 || d[5] > 59 || d[6] > 59 || d[11] > 24 || d[12] > 59) return BedjetStatusRuntime;
    if(d[9] > 6) return BedjetStatusMode;
    if(d[10] > 19) return BedjetStatusFan;
    if(d[7] < 2 || d[7] > 100 || d[17] < 2 || d[17] > 100) return BedjetStatusTemperature;
    if(d[13] < 38 || d[14] > 86 || d[13] > d[14] || d[8] < d[13] || d[8] > d[14])
        return BedjetStatusLimits;
    BedjetStatus s = {
        .mode = d[9],
        .target_step = d[8],
        .outlet_step = d[7],
        .ambient_step = d[17],
        .fan_step = d[10],
        .min_step = d[13],
        .max_step = d[14],
        .max_minutes = (uint16_t)(d[11] * 60u + d[12]),
        .remaining_seconds = d[4] * 3600u + d[5] * 60u + d[6],
    };
    *out = s;
    return BedjetStatusOk;
}

bool bedjet_status_decode(const uint8_t* d, size_t n, BedjetStatus* out) {
    return bedjet_status_parse(d, n, out) == BedjetStatusOk;
}

const char* bedjet_status_result_name(BedjetStatusResult result) {
    static const char* const names[] = {
        "ok", "length", "header", "declared", "runtime", "mode", "fan", "temp", "limits"};
    return (unsigned)result < BedjetStatusResultCount ? names[result] : "unknown";
}

size_t bedjet_button(uint8_t b, uint8_t out[3]) {
    if(!out || !((b >= 1 && b <= 6) || (b >= 0x20 && b <= 0x22))) return 0;
    out[0] = 0x01;
    out[1] = b;
    return 2;
}

size_t bedjet_temperature(uint8_t step, uint8_t min, uint8_t max, uint8_t out[3]) {
    if(!out || min < 38 || max > 86 || min > max || step < min || step > max) return 0;
    out[0] = 0x03;
    out[1] = step;
    return 2;
}

size_t bedjet_fan(uint8_t step, uint8_t out[3]) {
    if(!out || step > 19) return 0;
    out[0] = 0x07;
    out[1] = step;
    return 2;
}

size_t bedjet_timer(uint16_t minutes, uint16_t max_minutes, uint8_t out[3]) {
    if(!out || minutes == 0 || minutes > max_minutes || minutes > 600) return 0;
    out[0] = 0x02;
    out[1] = (uint8_t)(minutes / 60);
    out[2] = (uint8_t)(minutes % 60);
    return 3;
}

uint16_t bedjet_fahrenheit_tenths(uint8_t step) {
    return (uint16_t)(9u * step + 320u);
}

const char* bedjet_mode_name(uint8_t mode) {
    static const char* const names[] = {"Off", "Heat", "Turbo", "Ext heat", "Cool", "Dry", "Wait"};
    return mode <= 6 ? names[mode] : "Unknown";
}
