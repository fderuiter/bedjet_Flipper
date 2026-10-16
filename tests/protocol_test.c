#include "../bedjet_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void) {
    /* Synthetic vector based on documented offsets, NOT a hardware capture. */
    uint8_t full[32] = {0, 0x56, 32, 1, 2, 30, 15, 50, 60, 1, 9, 4, 0, 38, 86, 0, 0, 42};
    BedjetStatus s = {0};
    assert(bedjet_status_decode(full, 32, &s));
    assert(s.mode == 1 && s.target_step == 60 && s.fan_step == 9);
    assert(s.remaining_seconds == 9015 && s.max_minutes == 240 && s.ambient_step == 42);
    assert(bedjet_fahrenheit_tenths(38) == 662 && bedjet_fahrenheit_tenths(86) == 1094);
    assert(bedjet_fahrenheit_tenths(60) == 860);
    uint8_t partial[20];
    memcpy(partial, full, 20);
    partial[0] = 1;
    assert(bedjet_status_decode(partial, 20, &s));
    for(size_t n = 0; n < 18; n++)
        assert(!bedjet_status_decode(full, n, &s));
    assert(!bedjet_status_decode(full, 20, &s));
    assert(!bedjet_status_decode(NULL, 32, &s));
    assert(!bedjet_status_decode(full, 32, NULL));
    uint8_t bad[32];
    memcpy(bad, full, 32);
    bad[10] = 20;
    BedjetStatus before = s;
    assert(!bedjet_status_decode(bad, 32, &s));
    assert(memcmp(&s, &before, sizeof s) == 0);
    memcpy(bad, full, 32);
    bad[9] = 7;
    assert(!bedjet_status_decode(bad, 32, &s));
    memcpy(bad, full, 32);
    bad[5] = 60;
    assert(!bedjet_status_decode(bad, 32, &s));
    memcpy(bad, full, 32);
    bad[13] = 70;
    assert(!bedjet_status_decode(bad, 32, &s));
    memcpy(bad, full, 32);
    bad[1] = 5;
    assert(!bedjet_status_decode(bad, 32, &s));
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
    /* Exhaust each single-byte corruption and all truncated lengths under ASan. */
    for(size_t index = 0; index < 32; index++)
        for(unsigned value = 0; value < 256; value++) {
            memcpy(bad, full, 32);
            bad[index] = (uint8_t)value;
            for(size_t len = 0; len <= 32; len++)
                (void)bedjet_status_decode(bad, len, &s);
        }
    puts("PASS: command vectors, status validation, truncation and 270336 malformed-input cases");
    return 0;
}
