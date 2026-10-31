/*
 * Raw DEFLATE (RFC 1951) decoder for compressed boot payloads (loader.c,
 * tools/mkpayload.py --compress): ABL only loads a ramdisk of about 20 MB
 * (an image with a 41 MB payload stayed on its splash), so payloads travel
 * deflated and are unpacked here before loading. Canonical Huffman decoding
 * one bit at a time, as RFC 1951 3.2.2 describes it; no tables beyond the
 * code-length counts. Runs with the MMU off, so input is read in aligned
 * 8-byte words (Device memory) and every output byte is a plain store.
 */
#include "inflate.h"

#define MAXBITS		15
#define MAXLCODES	286
#define MAXDCODES	30

struct in {
	const uint8_t *src;
	uint64_t len, pos;		/* in bytes */
	uint64_t word;			/* bits not yet used */
	int nbits;
	int err;
};

struct huff {
	uint16_t count[MAXBITS + 1];	/* codes of each length */
	uint16_t symbol[MAXLCODES];	/* symbols ordered by code */
};

static uint64_t load64(const uint8_t *p)
{
	return *(volatile const uint64_t *)p;
}

static void refill(struct in *s)
{
	while (s->nbits <= 56) {
		if (s->pos >= s->len) {
			if (s->nbits == 0)
				s->err = 1;
			return;
		}
		uint64_t b;
		if ((s->pos & 7) == 0 && s->pos + 8 <= s->len && s->nbits == 0) {
			s->word = load64(s->src + s->pos);
			s->pos += 8;
			s->nbits = 64;
			return;
		}
		b = s->src[s->pos++];
		s->word |= b << s->nbits;
		s->nbits += 8;
	}
}

static uint32_t bits(struct in *s, int n)
{
	if (n == 0)
		return 0;
	if (s->nbits < n)
		refill(s);
	if (s->nbits < n) {
		s->err = 1;
		return 0;
	}
	uint32_t v = (uint32_t)(s->word & ((1ULL << n) - 1));
	s->word = n == 64 ? 0 : s->word >> n;
	s->nbits -= n;
	return v;
}

/* Canonical Huffman: walk the code one bit at a time against the per-length counts. */
static int decode(struct in *s, const struct huff *h)
{
	int code = 0, first = 0, index = 0;
	for (int len = 1; len <= MAXBITS; len++) {
		code |= (int)bits(s, 1);
		int count = h->count[len];
		if (code - count < first)
			return h->symbol[index + (code - first)];
		index += count;
		first += count;
		first <<= 1;
		code <<= 1;
	}
	return -1;
}

/* Build h from code lengths; 0 if complete or incomplete-but-usable, < 0 if over-subscribed. */
static int build(struct huff *h, const uint8_t *length, int n)
{
	uint16_t offs[MAXBITS + 1];
	for (int len = 0; len <= MAXBITS; len++)
		h->count[len] = 0;
	for (int sym = 0; sym < n; sym++)
		h->count[length[sym]]++;
	if (h->count[0] == n)
		return 0;
	int left = 1;
	for (int len = 1; len <= MAXBITS; len++) {
		left <<= 1;
		left -= h->count[len];
		if (left < 0)
			return -1;
	}
	offs[1] = 0;
	for (int len = 1; len < MAXBITS; len++)
		offs[len + 1] = offs[len] + h->count[len];
	for (int sym = 0; sym < n; sym++)
		if (length[sym] != 0)
			h->symbol[offs[length[sym]]++] = (uint16_t)sym;
	return left;
}

static const uint16_t len_base[29] = {
	3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
	35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
	0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
	1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
	257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
	0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

struct out {
	uint8_t *dst;
	uint64_t len, pos;
};

static int codes(struct in *s, struct out *o, const struct huff *lit, const struct huff *dist)
{
	for (;;) {
		int sym = decode(s, lit);
		if (s->err || sym < 0)
			return -2;
		if (sym < 256) {
			if (o->pos >= o->len)
				return -3;
			o->dst[o->pos++] = (uint8_t)sym;
		} else if (sym == 256) {
			return 0;
		} else {
			sym -= 257;
			if (sym >= 29)
				return -4;
			uint32_t len = len_base[sym] + bits(s, len_extra[sym]);
			int d = decode(s, dist);
			if (s->err || d < 0 || d >= 30)
				return -5;
			uint32_t back = dist_base[d] + bits(s, dist_extra[d]);
			if (back > o->pos)
				return -6;
			if (o->pos + len > o->len)
				return -3;
			for (uint32_t i = 0; i < len; i++, o->pos++)
				o->dst[o->pos] = o->dst[o->pos - back];
		}
	}
}

static int block_stored(struct in *s, struct out *o)
{
	s->word >>= s->nbits & 7;		/* to a byte boundary */
	s->nbits -= s->nbits & 7;
	uint32_t len = bits(s, 16), nlen = bits(s, 16);
	if (s->err || (len ^ 0xffff) != nlen)
		return -7;
	if (o->pos + len > o->len)
		return -3;
	for (uint32_t i = 0; i < len; i++)
		o->dst[o->pos++] = (uint8_t)bits(s, 8);
	return s->err ? -2 : 0;
}

static struct huff lit_h, dist_h;	/* in .bss: kept off the small shim stack */

static int block_fixed(struct in *s, struct out *o)
{
	uint8_t length[MAXLCODES];
	int sym = 0;
	for (; sym < 144; sym++)
		length[sym] = 8;
	for (; sym < 256; sym++)
		length[sym] = 9;
	for (; sym < 280; sym++)
		length[sym] = 7;
	for (; sym < 288 && sym < MAXLCODES; sym++)
		length[sym] = 8;
	build(&lit_h, length, MAXLCODES);
	for (sym = 0; sym < MAXDCODES; sym++)
		length[sym] = 5;
	build(&dist_h, length, MAXDCODES);
	return codes(s, o, &lit_h, &dist_h);
}

static int block_dynamic(struct in *s, struct out *o)
{
	static const uint8_t order[19] = { 16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
	uint8_t length[MAXLCODES + MAXDCODES];
	int nlen = (int)bits(s, 5) + 257, ndist = (int)bits(s, 5) + 1, ncode = (int)bits(s, 4) + 4;
	if (s->err || nlen > MAXLCODES || ndist > MAXDCODES)
		return -8;
	int i = 0;
	for (; i < ncode; i++)
		length[order[i]] = (uint8_t)bits(s, 3);
	for (; i < 19; i++)
		length[order[i]] = 0;
	if (build(&lit_h, length, 19) != 0)
		return -9;
	for (i = 0; i < nlen + ndist;) {
		int sym = decode(s, &lit_h);
		if (s->err || sym < 0)
			return -10;
		if (sym < 16) {
			length[i++] = (uint8_t)sym;
			continue;
		}
		uint8_t val = 0;
		int rep;
		if (sym == 16) {
			if (i == 0)
				return -11;
			val = length[i - 1];
			rep = 3 + (int)bits(s, 2);
		} else if (sym == 17) {
			rep = 3 + (int)bits(s, 3);
		} else {
			rep = 11 + (int)bits(s, 7);
		}
		if (i + rep > nlen + ndist)
			return -12;
		while (rep--)
			length[i++] = val;
	}
	if (length[256] == 0)
		return -13;
	int e = build(&lit_h, length, nlen);
	if (e < 0 || (e > 0 && nlen - lit_h.count[0] != 1))
		return -14;
	e = build(&dist_h, length + nlen, ndist);
	if (e < 0 || (e > 0 && ndist - dist_h.count[0] != 1))
		return -15;
	return codes(s, o, &lit_h, &dist_h);
}

int64_t inflate_raw(uint8_t *dst, uint64_t dstlen, const uint8_t *src, uint64_t srclen)
{
	struct in s = { src, srclen, 0, 0, 0, 0 };
	struct out o = { dst, dstlen, 0 };
	int last, rc;
	do {
		last = (int)bits(&s, 1);
		int type = (int)bits(&s, 2);
		if (s.err)
			return -2;
		rc = type == 0 ? block_stored(&s, &o) : type == 1 ? block_fixed(&s, &o) :
		    type == 2 ? block_dynamic(&s, &o) : -16;
		if (rc)
			return rc;
	} while (!last);
	return (int64_t)o.pos;
}
