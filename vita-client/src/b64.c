#include "b64.h"

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

int b64_decode(const char *in, uint8_t *out, size_t out_max)
{
    size_t len = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (; *in; in++) {
        char c = *in;
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t')
            continue;
        if (c == '=')
            break;  /* padding: done */

        int v = b64_val(c);
        if (v < 0)
            return -1;

        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (len >= out_max)
                return -1;
            out[len++] = (uint8_t)(acc >> bits);
        }
    }
    return (int)len;
}
