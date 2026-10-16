#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* UUIDs in STM32WB's little-endian representation. */
extern const uint8_t bedjet_service_uuid[16];
extern const uint8_t bedjet_status_uuid[16];
extern const uint8_t bedjet_command_uuid[16];

/* Observed operating modes (status byte 9). */
enum {
    BedjetModeStandby,
    BedjetModeHeat,
    BedjetModeTurbo,
    BedjetModeExtHeat,
    BedjetModeCool,
    BedjetModeDry,
    BedjetModeWait
};

typedef struct {
    uint8_t mode;
    uint8_t target_step;
    uint8_t outlet_step;
    uint8_t ambient_step;
    uint8_t fan_step;
    uint8_t min_step;
    uint8_t max_step;
    uint16_t max_minutes;
    uint32_t remaining_seconds;
} BedjetStatus;

/* Only the first 18 status bytes are needed. Extended/unknown fields are not
 * interpreted; a 20-byte partial notification contains all core fields. */
bool bedjet_status_decode(const uint8_t* data, size_t length, BedjetStatus* out);
size_t bedjet_button(uint8_t button, uint8_t out[3]);
size_t bedjet_temperature(uint8_t step, uint8_t min, uint8_t max, uint8_t out[3]);
size_t bedjet_fan(uint8_t step, uint8_t out[3]);
size_t bedjet_timer(uint16_t minutes, uint16_t max_minutes, uint8_t out[3]);
uint16_t bedjet_fahrenheit_tenths(uint8_t step);
const char* bedjet_mode_name(uint8_t mode);
