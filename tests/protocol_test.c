#include "../bedjet_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Synthetic vector based on documented offsets, NOT a hardware capture:
 * complete frame, Heat, target 60 (30 C) within 38..86, fan 9, 2:30:15 left,
 * 4:00 maximum runtime, outlet 50 and ambient 42. */
static const uint8_t full[32] = {0, 0x56, 32, 1, 2, 30, 15, 50, 60, 1, 9, 4, 0, 38, 86, 0, 0, 42};

/* Documented acceptance per byte for single-byte changes to `full`, written
 * from the layout table in docs/architecture.md rather than from the decoder.
 * Target 60 constrains the minimum to 38..60 and the maximum to 60..86. */
static BedjetStatusResult expected(size_t index, unsigned v) {
    switch(index) {
    case 0:
        return v <= 1 ? BedjetStatusOk : BedjetStatusHeader;
    case 1:
        return v == 0x56 ? BedjetStatusOk : BedjetStatusHeader;
    case 2:
        return v == 32 ? BedjetStatusOk : BedjetStatusDeclared;
    case 3:
        return v == 1 ? BedjetStatusOk : BedjetStatusHeader;
    case 4:
    case 11:
        return v <= 24 ? BedjetStatusOk : BedjetStatusRuntime;
    case 5:
    case 6:
    case 12:
        return v <= 59 ? BedjetStatusOk : BedjetStatusRuntime;
    case 7:
    case 17:
        return v >= 2 && v <= 100 ? BedjetStatusOk : BedjetStatusTemperature;
    case 8:
        return v >= 38 && v <= 86 ? BedjetStatusOk : BedjetStatusLimits;
    case 9:
        return v <= 6 ? BedjetStatusOk : BedjetStatusMode;
    case 10:
        return v <= 19 ? BedjetStatusOk : BedjetStatusFan;
    case 13:
        return v >= 38 && v <= 60 ? BedjetStatusOk : BedjetStatusLimits;
    case 14:
        return v >= 60 && v <= 86 ? BedjetStatusOk : BedjetStatusLimits;
    default:
        return BedjetStatusOk; /* Turbo time, shutdown reason and tail are not interpreted. */
    }
}

static void test_valid_frames(void) {
    BedjetStatus s = {0};
    assert(bedjet_status_decode(full, 32, &s));
    assert(s.mode == 1 && s.target_step == 60 && s.fan_step == 9);
    assert(s.remaining_seconds == 9015 && s.max_minutes == 240 && s.ambient_step == 42);
    assert(s.outlet_step == 50 && s.min_step == 38 && s.max_step == 86);
    assert(bedjet_fahrenheit_tenths(38) == 662 && bedjet_fahrenheit_tenths(86) == 1094);
    assert(bedjet_fahrenheit_tenths(60) == 860);
    uint8_t partial[20];
    memcpy(partial, full, 20);
    partial[0] = 1;
    assert(bedjet_status_parse(partial, 20, &s) == BedjetStatusOk);
}

static void test_each_rule_has_a_rejecting_vector(void) {
    static const struct {
        size_t index;
        uint8_t value;
        BedjetStatusResult result;
    } cases[] = {
        {0, 2, BedjetStatusHeader},         {1, 0x05, BedjetStatusHeader},
        {3, 2, BedjetStatusHeader},         {2, 17, BedjetStatusDeclared},
        {2, 33, BedjetStatusDeclared},      {2, 31, BedjetStatusDeclared},
        {4, 25, BedjetStatusRuntime},       {5, 60, BedjetStatusRuntime},
        {6, 60, BedjetStatusRuntime},       {11, 25, BedjetStatusRuntime},
        {12, 60, BedjetStatusRuntime},      {9, 7, BedjetStatusMode},
        {10, 20, BedjetStatusFan},          {7, 1, BedjetStatusTemperature},
        {7, 101, BedjetStatusTemperature},  {17, 1, BedjetStatusTemperature},
        {17, 101, BedjetStatusTemperature}, {13, 37, BedjetStatusLimits},
        {14, 87, BedjetStatusLimits},       {13, 61, BedjetStatusLimits},
        {14, 59, BedjetStatusLimits},       {8, 37, BedjetStatusLimits},
        {8, 87, BedjetStatusLimits},
    };
    for(size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t bad[32];
        memcpy(bad, full, 32);
        bad[cases[i].index] = cases[i].value;
        BedjetStatus s = {.mode = 99};
        assert(bedjet_status_parse(bad, 32, &s) == cases[i].result);
        assert(s.mode == 99); /* Output is untouched on rejection. */
    }
    BedjetStatus s;
    /* A partial notification may stop early, but cannot declare >32 bytes. */
    uint8_t partial[20];
    memcpy(partial, full, 20);
    partial[0] = 1;
    partial[2] = 33;
    assert(bedjet_status_parse(partial, 20, &s) == BedjetStatusDeclared);
    uint8_t oversized[40] = {0};
    memcpy(oversized, full, 32);
    assert(bedjet_status_parse(oversized, 33, &s) == BedjetStatusLength);
    assert(bedjet_status_parse(NULL, 32, &s) == BedjetStatusLength);
    assert(bedjet_status_parse(full, 32, NULL) == BedjetStatusLength);
    assert(!strcmp(bedjet_status_result_name(BedjetStatusLimits), "limits"));
    assert(!strcmp(bedjet_status_result_name(BedjetStatusResultCount), "unknown"));
}

static unsigned test_lengths(void) {
    unsigned cases = 0;
    uint8_t partial[32];
    memcpy(partial, full, 32);
    partial[0] = 1;
    for(size_t n = 0; n <= 32; n++, cases += 2) {
        BedjetStatus s;
        BedjetStatusResult complete = bedjet_status_parse(full, n, &s);
        BedjetStatusResult flagged = bedjet_status_parse(partial, n, &s);
        if(n < 18) {
            assert(complete == BedjetStatusLength && flagged == BedjetStatusLength);
        } else {
            /* A complete frame must match its declared 32 bytes exactly. */
            assert(complete == (n == 32 ? BedjetStatusOk : BedjetStatusDeclared));
            assert(flagged == BedjetStatusOk);
        }
    }
    return cases;
}

static unsigned test_single_byte_mutations(void) {
    unsigned cases = 0;
    for(size_t index = 0; index < 32; index++)
        for(unsigned value = 0; value < 256; value++, cases++) {
            uint8_t bad[32];
            memcpy(bad, full, 32);
            bad[index] = (uint8_t)value;
            BedjetStatus s = {.mode = 99};
            BedjetStatusResult result = bedjet_status_parse(bad, 32, &s);
            assert(result == expected(index, value));
            if(result != BedjetStatusOk) assert(s.mode == 99);
            if(result == BedjetStatusOk && index == 9) assert(s.mode == value);
            if(result == BedjetStatusOk && index == 8) assert(s.target_step == value);
            /* Every truncation must also be memory safe under the sanitizers. */
            for(size_t len = 0; len < 32; len++)
                (void)bedjet_status_parse(bad, len, &s);
        }
    return cases;
}

static void test_command_vectors(void) {
    uint8_t cmd[3];
    assert(bedjet_button(2, cmd) == 2 && cmd[0] == 1 && cmd[1] == 2);
    assert(bedjet_button(0x22, cmd) == 2 && cmd[1] == 0x22);
    assert(bedjet_button(0x43, cmd) == 0); /* No firmware/debug commands. */
    assert(bedjet_temperature(50, 38, 86, cmd) == 2 && cmd[0] == 3 && cmd[1] == 50);
    assert(bedjet_temperature(75, 38, 72, cmd) == 0);
    assert(bedjet_fan(9, cmd) == 2 && cmd[0] == 7 && cmd[1] == 9);
    assert(bedjet_fan(20, cmd) == 0);
    assert(bedjet_timer(150, 240, cmd) == 3 && cmd[0] == 2 && cmd[1] == 2 && cmd[2] == 30);
    assert(bedjet_timer(241, 240, cmd) == 0 && bedjet_timer(0, 600, cmd) == 0);
    assert(bedjet_timer(601, 1440, cmd) == 0);
}

int main(void) {
    test_valid_frames();
    test_each_rule_has_a_rejecting_vector();
    unsigned lengths = test_lengths();
    unsigned mutations = test_single_byte_mutations();
    test_command_vectors();
    printf(
        "PASS: command vectors, per-rule rejections, %u length cases and %u single-byte "
        "mutations checked against the documented layout (plus every truncation)\n",
        lengths,
        mutations);
    return 0;
}
