#ifndef DAWNCORD_B64_H
#define DAWNCORD_B64_H

#include <stddef.h>
#include <stdint.h>

/* Decode a NUL-terminated base64 string into out (standard alphabet,
   '=' padding, whitespace ignored). Returns the decoded length, or -1 on
   invalid input / insufficient space. */
int b64_decode(const char *in, uint8_t *out, size_t out_max);

#endif
