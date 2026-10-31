#pragma once
#include <stdint.h>

/* Raw DEFLATE (RFC 1951) from src[0..srclen) into dst[0..dstlen); returns the bytes written, or < 0 on error. */
int64_t inflate_raw(uint8_t *dst, uint64_t dstlen, const uint8_t *src, uint64_t srclen);
