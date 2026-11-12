// QEMU-only replacement for the P4 ROM strtod routine.
// Espressif's current P4 QEMU image traps in the ROM implementation while
// cJSON parses cloud responses. The activation path only needs the numeric
// token consumed; preserve the token boundary and return its integer prefix.
#include <stdint.h>
#include <stddef.h>

static int is_digit(char c) {
    return c >= '0' && c <= '9';
}

double __wrap_strtod(const char* text, char** endptr) {
    const char* p = text;
    int negative = 0;
    uint64_t value = 0;
    if (p != NULL && *p == '-') {
        negative = 1;
        ++p;
    } else if (p != NULL && *p == '+') {
        ++p;
    }
    while (p != NULL && is_digit(*p)) {
        value = value * 10u + (uint64_t)(*p - '0');
        ++p;
    }
    if (p != NULL && *p == '.') {
        ++p;
        while (is_digit(*p)) ++p;
    }
    if (p != NULL && (*p == 'e' || *p == 'E')) {
        ++p;
        if (*p == '+' || *p == '-') ++p;
        while (is_digit(*p)) ++p;
    }
    if (endptr != NULL) *endptr = (char*)p;
    if (negative) return -(double)value;
    return (double)value;
}
