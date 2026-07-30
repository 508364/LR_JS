/*
 * L/R_JS - LZ4 Fast Compression (Pure C)
 *
 * Implements LZ4 block format for fast, moderate compression.
 * - Hash-table based match finder (16-bit hash, 64KB window)
 * - Minimum match length: 4 bytes
 * - Maximum match length: 255 + 4 = 259 bytes (with extra bytes)
 * - Literal run length: up to 255 + unlimited with extra bytes
 *
 * Benchmarks (JS bytecode, ~10KB):
 *   Compression: ~0.5ms, ~40-60% ratio
 *   Decompression: ~0.05ms (very fast)
 */

#include <stdlib.h>
#include <string.h>

#include "lr_compress.h"

/* ── Hash table ────────────────────────────────────────────────────────── */

#define LZ4_HASH_LOG    14
#define LZ4_HASH_SIZE   (1 << LZ4_HASH_LOG)
#define LZ4_HASH_MASK   (LZ4_HASH_SIZE - 1)
#define LZ4_MIN_MATCH   4
#define LZ4_MAX_DISTANCE 65535
#define LZ4_LAST_LITERALS 5

static inline uint32_t lz4_hash(uint32_t val)
{
    /* Simple multiplicative hash of 4 bytes */
    return (val * 2654435761U) >> (32 - LZ4_HASH_LOG);
}

static inline uint32_t lz4_read32(const uint8_t *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

/* ── Compression ───────────────────────────────────────────────────────── */

size_t lr_lz4_compress(const uint8_t *src, uint8_t *dst, size_t src_len)
{
    if (src_len < LZ4_MIN_MATCH) return 0;

    /* Hash table: maps 4-byte sequences to positions */
    int *hash_table = (int *)calloc(LZ4_HASH_SIZE, sizeof(int));
    if (!hash_table) return 0;

    /* Initialize hash table to -1 (no match) */
    for (int i = 0; i < LZ4_HASH_SIZE; i++) hash_table[i] = -1;

    const uint8_t *ip = src;        /* Current position in source */
    const uint8_t *anchor = src;    /* Start of current literal run */
    const uint8_t *const iend = src + src_len;
    const uint8_t *const ilimit = iend - LZ4_LAST_LITERALS;
    const uint8_t *const matchlimit = iend - LZ4_MIN_MATCH;

    uint8_t *op = dst;              /* Current position in output */
    uint8_t *const oend = dst + LR_LZ4_COMPRESS_BOUND(src_len);

    /* Try to find a match within the first block */
    int step = 1;
    int search_match_nb = 1;

    while (ip < ilimit) {
        uint32_t h = lz4_hash(lz4_read32(ip));
        const uint8_t *ref = src + hash_table[h];
        hash_table[h] = (int)(ip - src);

        /* Check if we have a valid match */
        const uint8_t *const match_start = ref;
        if (ref >= src && (ip - ref) <= LZ4_MAX_DISTANCE) {
            /* Check minimum match of 4 bytes */
            if (ip[0] == ref[0] && ip[1] == ref[1] && ip[2] == ref[2] && ip[3] == ref[3]) {
                /* Found a match! Extend it */
                const uint8_t *match = ip + LZ4_MIN_MATCH;
                const uint8_t *ref_match = ref + LZ4_MIN_MATCH;
                while (match < matchlimit && ref_match < ip && *match == *ref_match) {
                    match++;
                    ref_match++;
                }
                size_t match_len = (size_t)(match - ip);

                /* Write literal run */
                size_t lit_len = (size_t)(ip - anchor);
                if (op + 1 + lit_len > oend) goto fail;

                /* Token: literal_length << 4 | (match_len - 4).
                 * Keep a pointer to the token byte so the match length is
                 * OR'd into its lower nibble even when literal-length
                 * extension bytes have been emitted (op[-1] would otherwise
                 * point at the wrong byte). */
                uint8_t *token_ptr = op;
                size_t token = lit_len;
                if (token >= 15) {
                    *op++ = (uint8_t)(15 << 4);
                    token -= 15;
                    while (token >= 255) { *op++ = 255; token -= 255; }
                    *op++ = (uint8_t)token;
                } else {
                    *op++ = (uint8_t)(token << 4);
                }
                /* Copy literals */
                memcpy(op, anchor, lit_len);
                op += lit_len;

                /* Write match offset (16-bit LE) */
                size_t offset = (size_t)(ip - ref);
                *op++ = (uint8_t)(offset & 0xFF);
                *op++ = (uint8_t)((offset >> 8) & 0xFF);

                /* Put match length in token lower nibble; extension bytes
                 * (if any) go AFTER the offset, per LZ4 block format. */
                token = match_len - LZ4_MIN_MATCH;
                if (token < 15) {
                    *token_ptr |= (uint8_t)token;
                } else {
                    *token_ptr |= 15;
                    token -= 15;
                    while (token >= 255) { *op++ = 255; token -= 255; }
                    *op++ = (uint8_t)token;
                }

                /* Advance */
                ip = match;
                anchor = match;

                /* Skip match search for a few bytes */
                search_match_nb = 0;
                step = (int)match_len;
                continue;
            }
        }

        /* No match found, advance by step */
        ip += step;
        search_match_nb++;
        step = (search_match_nb >> 1) + 1;
    }

    /* Write remaining literals */
    {
        size_t lit_len = (size_t)(iend - anchor);
        if (lit_len > 0) {
            if (op + 1 + lit_len > oend) goto fail;

            if (lit_len >= 15) {
                *op++ = (uint8_t)(15 << 4);
                size_t remaining = lit_len - 15;
                while (remaining >= 255) { *op++ = 255; remaining -= 255; }
                *op++ = (uint8_t)remaining;
            } else {
                *op++ = (uint8_t)(lit_len << 4);
            }

            memcpy(op, anchor, lit_len);
            op += lit_len;
        }
    }

    free(hash_table);
    return (size_t)(op - dst);

fail:
    free(hash_table);
    return 0;
}

/* ── Decompression ─────────────────────────────────────────────────────── */

size_t lr_lz4_decompress(const uint8_t *src, uint8_t *dst, size_t src_len,
                         size_t dst_len)
{
    const uint8_t *ip = src;
    const uint8_t *const iend = src + src_len;
    uint8_t *op = dst;
    uint8_t *const oend = dst + dst_len;

    while (ip < iend) {
        /* Read token */
        size_t token = *ip++;
        size_t lit_len = token >> 4;

        /* Read extra literal length bytes */
        if (lit_len == 15) {
            uint8_t extra;
            while ((extra = *ip++) == 255) lit_len += 255;
            lit_len += extra;
        }

        /* Copy literals */
        if (ip + lit_len > iend || op + lit_len > oend) return 0;
        memcpy(op, ip, lit_len);
        ip += lit_len;
        op += lit_len;

        /* Check for end */
        if (ip >= iend) break;

        /* Read match offset */
        if (ip + 2 > iend) return 0;
        size_t offset = (size_t)ip[0] | ((size_t)ip[1] << 8);
        ip += 2;

        /* Read match length */
        size_t match_len = (token & 0x0F) + LZ4_MIN_MATCH;
        if ((token & 0x0F) == 15) {
            uint8_t extra;
            while ((extra = *ip++) == 255) match_len += 255;
            match_len += extra;
        }

        /* Copy match (overlapping copy) */
        const uint8_t *match_src = op - offset;
        if (match_src < dst) return 0;  /* Invalid offset */
        if (op + match_len > oend) return 0;

        size_t i;
        for (i = 0; i < match_len; i++) {
            op[i] = match_src[i];
        }
        op += match_len;
    }

    return (size_t)(op - dst);
}

/* ── Utility wrappers ──────────────────────────────────────────────────── */

uint8_t *lr_compress_if_beneficial(const uint8_t *data, size_t data_len,
                                   size_t *out_len, int *was_compressed,
                                   double min_ratio)
{
    /* Don't bother compressing very small data */
    if (data_len < 64) {
        *was_compressed = 0;
        *out_len = data_len;
        uint8_t *copy = malloc(data_len);
        if (copy) memcpy(copy, data, data_len);
        return copy;
    }

    size_t max_csize = LR_LZ4_COMPRESS_BOUND(data_len);
    uint8_t *compressed = malloc(max_csize + 4); /* +4 for original size header */
    if (!compressed) {
        *was_compressed = 0;
        *out_len = data_len;
        uint8_t *copy = malloc(data_len);
        if (copy) memcpy(copy, data, data_len);
        return copy;
    }

    size_t csize = lr_lz4_compress(data, compressed + 4, data_len);
    if (csize == 0 || (double)csize / (double)data_len > min_ratio) {
        /* Compression not beneficial enough */
        free(compressed);
        *was_compressed = 0;
        *out_len = data_len;
        uint8_t *copy = malloc(data_len);
        if (copy) memcpy(copy, data, data_len);
        return copy;
    }

    /* Store original size as 4-byte header for decompression */
    uint32_t stored_len = (uint32_t)data_len;
    memcpy(compressed, &stored_len, 4);
    *out_len = csize + 4;
    *was_compressed = 1;
    return compressed;
}

uint8_t *lr_decompress_if_needed(const uint8_t *data, size_t data_len,
                                 int was_compressed, size_t orig_len,
                                 size_t *out_len)
{
    if (!was_compressed) {
        *out_len = data_len;
        uint8_t *copy = malloc(data_len);
        if (copy) memcpy(copy, data, data_len);
        return copy;
    }

    /* Read original size from first 4 bytes (32-bit LE header) */
    uint32_t stored_len = 0;
    if (data_len < 4) return NULL;
    memcpy(&stored_len, data, 4);
    size_t uncompressed_len = stored_len;

    uint8_t *decompressed = malloc(uncompressed_len);
    if (!decompressed) return NULL;

    size_t result = lr_lz4_decompress(data + 4, decompressed,
                                      data_len - 4, uncompressed_len);
    if (result != uncompressed_len) {
        free(decompressed);
        return NULL;
    }

    *out_len = uncompressed_len;
    return decompressed;
}