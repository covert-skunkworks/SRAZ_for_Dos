/*
 * binpatch.c - apply a patch made by bindiff, after verifying the target's CRC-32.
 *
 *   binpatch <target_file> <patch_file> [output_file]
 *
 * Reads the patch, then refuses to proceed unless the target file's size and CRC-32
 * match the original recorded in the patch (so it won't patch the wrong file or one
 * that's already patched). Applies the byte changes, verifies the result's CRC-32
 * against the patch, and writes the output (default: <target_file>.patched).
 * The target file is never modified in place unless you pass it as the output.
 *
 *   build:  gcc -O2 -Wall -o binpatch binpatch.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


/* CRC-32 (zlib / PKZIP polynomial 0xEDB88320) */
static unsigned long crc32_buf(const unsigned char *p, long n)
{
    static unsigned long tab[256];
    static int init = 0;

    if (!init) {
        for (unsigned long i = 0; i < 256; i++) {
            unsigned long c = i;
            for (int k = 0; k < 8; k++)
                c = (c & 1) ? (0xEDB88320UL ^ (c >> 1)) : (c >> 1);
            tab[i] = c;
        }
        init = 1;
    }

    unsigned long c = 0xFFFFFFFFUL;
    for (long i = 0; i < n; i++)
        c = tab[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static unsigned char *read_file(const char *path, long *n)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "error: cannot open '%s'\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    rewind(f);

    unsigned char *b = (unsigned char *)malloc(*n > 0 ? *n : 1);
    if (b && *n > 0 && fread(b, 1, *n, f) != (size_t)*n) {
        fprintf(stderr, "error: short read on '%s'\n", path);
        free(b);
        fclose(f);
        return NULL;
    }

    fclose(f);
    return b;
}

static unsigned long get_u32(const unsigned char *p)
{
    return  (unsigned long)p[0]
         | ((unsigned long)p[1] << 8)
         | ((unsigned long)p[2] << 16)
         | ((unsigned long)p[3] << 24);
}


int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: %s <target_file> <patch_file> [output_file]\n", argv[0]);
        return 1;
    }

    const char *target = argv[1];
    const char *patchf = argv[2];

    char outbuf[1024];
    const char *out;
    if (argc > 3) {
        out = argv[3];
    } else {
        snprintf(outbuf, sizeof outbuf, "%s.patched", target);
        out = outbuf;
    }

    long np, nt;
    unsigned char *p = read_file(patchf, &np);
    unsigned char *t = read_file(target, &nt);
    if (!p || !t)
        return 2;

    /* parse the patch header */
    if (np < 28 || memcmp(p, "BDIF", 4) != 0) {
        fprintf(stderr, "error: '%s' is not a BDIF patch file\n", patchf);
        return 3;
    }

    unsigned long ver      = get_u32(p + 4);
    unsigned long src_crc  = get_u32(p + 8);
    unsigned long src_size = get_u32(p + 12);
    unsigned long dst_crc  = get_u32(p + 16);
    unsigned long dst_size = get_u32(p + 20);
    unsigned long count    = get_u32(p + 24);

    if (ver != 1) {
        fprintf(stderr, "error: unsupported patch version %lu\n", ver);
        return 3;
    }
    if (np < 28 + (long)count * 6) {
        fprintf(stderr, "error: patch file truncated\n");
        return 3;
    }

    /* verify the target is the expected original */
    unsigned long crc_t = crc32_buf(t, nt);
    printf("target: %s  (%ld bytes, crc %08lX)\n", target, nt, crc_t);
    printf("patch expects original  %lu bytes, crc %08lX\n", src_size, src_crc);
    if ((unsigned long)nt != src_size || crc_t != src_crc) {
        fprintf(stderr, "ABORT: target does not match the patch's source "
                        "(wrong file, or already patched). Nothing written.\n");
        return 4;
    }

    /* build the patched output (handles a size change) */
    unsigned char *o = (unsigned char *)calloc(dst_size > 0 ? dst_size : 1, 1);
    if (!o) {
        fprintf(stderr, "error: out of memory\n");
        return 5;
    }

    long copy = (long)(nt < (long)dst_size ? nt : (long)dst_size);
    memcpy(o, t, copy);

    /* apply each record: check the old byte, set the new byte */
    const unsigned char *rec = p + 28;
    long mismatches = 0;
    for (unsigned long i = 0; i < count; i++) {
        unsigned long off = get_u32(rec);
        unsigned char old = rec[4];
        unsigned char neu = rec[5];
        rec += 6;

        if (off >= dst_size)
            continue;                       /* defensive */
        if (off < (unsigned long)nt && t[off] != old)
            mismatches++;
        o[off] = neu;
    }

    if (mismatches)
        fprintf(stderr, "warning: %ld record(s) had an unexpected old byte\n", mismatches);

    /* verify the result before writing anything */
    unsigned long crc_o = crc32_buf(o, dst_size);
    if (crc_o != dst_crc) {
        fprintf(stderr, "ABORT: patched result crc %08lX != expected %08lX. Nothing written.\n",
                crc_o, dst_crc);
        return 6;
    }

    FILE *f = fopen(out, "wb");
    if (!f) {
        fprintf(stderr, "error: cannot write '%s'\n", out);
        return 7;
    }
    fwrite(o, 1, dst_size, f);
    fclose(f);

    printf("applied %lu change(s); result %lu bytes, crc %08lX (verified)\n", count, dst_size, crc_o);
    printf("wrote: %s\n", out);

    free(p);
    free(t);
    free(o);
    return 0;
}
