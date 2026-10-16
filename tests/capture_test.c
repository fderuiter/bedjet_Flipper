#include "../bedjet_protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Replays sanitized hardware captures (tests/captures, written by
 * scripts/captures.py) through the parser. Each frame line records what the
 * app concluded on the device: "status | <mode> | <len> | <hex>" for accepted
 * frames, "read | <result> | ..." for status reads and "rejected | <reason> |
 * ..." for rejected notifications. A parser change that classifies a real
 * frame differently fails here until the capture is deliberately re-reviewed. */

static int failures;

static char* trim(char* s) {
    while(*s == ' ')
        s++;
    char* end = s + strlen(s);
    while(end > s && (end[-1] == ' ' || end[-1] == '\n' || end[-1] == '\r'))
        *--end = '\0';
    return s;
}

static void fail(const char* file, unsigned line, const char* why, const char* detail) {
    fprintf(stderr, "%s:%u: %s (%s)\n", file, line, why, detail);
    failures++;
}

static void check_frame(const char* file, unsigned line, char* text) {
    char* fields[4];
    for(unsigned i = 0; i < 4; i++) {
        char* bar = i < 3 ? strchr(text, '|') : NULL;
        if(i < 3 && !bar) {
            fail(file, line, "expected 4 fields", text);
            return;
        }
        if(bar) *bar = '\0';
        fields[i] = trim(text);
        text = bar ? bar + 1 : text;
    }
    const char* kind = fields[0];
    const char* detail = fields[1];
    char* end;
    unsigned long length = strtoul(fields[2], &end, 10);
    if(*end || end == fields[2]) {
        fail(file, line, "bad length", fields[2]);
        return;
    }
    uint8_t bytes[32];
    size_t count = 0;
    for(char* hex = fields[3]; *hex;) {
        unsigned long value = strtoul(hex, &end, 16);
        if(end == hex || value > 0xFF || count == sizeof(bytes)) {
            fail(file, line, "bad hex", fields[3]);
            return;
        }
        bytes[count++] = (uint8_t)value;
        hex = trim(end);
    }
    /* The device log prints at most 32 bytes; longer frames are oversize. */
    if(length > 32) {
        if(count != 32 || strcmp(detail, "length"))
            fail(
                file, line, "oversize frame must be a 32-byte prefix rejected for length", detail);
        return;
    }
    if(count != length) {
        fail(file, line, "hex count differs from length", fields[2]);
        return;
    }
    BedjetStatus status = {0};
    const char* result = bedjet_status_result_name(bedjet_status_parse(bytes, count, &status));
    if(!strcmp(kind, "status")) {
        if(strcmp(result, "ok"))
            fail(file, line, "accepted on device, now rejected", result);
        else if(strcmp(bedjet_mode_name(status.mode), detail))
            fail(file, line, "mode differs", bedjet_mode_name(status.mode));
    } else if(!strcmp(kind, "read") || !strcmp(kind, "rejected")) {
        if(strcmp(result, detail)) fail(file, line, "result differs", result);
        if(!strcmp(kind, "rejected") && !strcmp(result, "ok"))
            fail(file, line, "rejected frame cannot parse ok", detail);
    } else {
        fail(file, line, "unknown kind", kind);
    }
}

int main(int argc, char** argv) {
    unsigned frames = 0;
    for(int i = 1; i < argc; i++) {
        FILE* f = fopen(argv[i], "r");
        if(!f) {
            perror(argv[i]);
            return 1;
        }
        char text[512];
        unsigned line = 0;
        while(fgets(text, sizeof(text), f)) {
            line++;
            char* s = trim(text);
            if(!*s || *s == '#') continue;
            check_frame(argv[i], line, s);
            frames++;
        }
        fclose(f);
    }
    if(failures) return 1;
    printf("PASS: %u captured frames from %d files\n", frames, argc - 1);
    return 0;
}
