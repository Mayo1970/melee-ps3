// Host test of ps3/sdk/thp.c (TOOLCHAIN=linux): frames of an MTH movie, or a THP still, as PPM files. Each frame is
// also written as a standard JPEG (0xFF stuffed again, EOI added), to compare with another decoder.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <dolphin/os.h>
#include <dolphin/thp/thp.h>

void OSReport(char* fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void gp_post_mem_changed(const void* p, u32 size) {}

u32 gp_post_copy(void* dst, const void* src, u32 size)
{
    memcpy(dst, src, size);
    return 0;
}

void gp_wait_copy(u32 n) {}

static u32 be32(const u8* p)
{
    return (u32) p[0] << 24 | (u32) p[1] << 16 | (u32) p[2] << 8 | p[3];
}

static u8 texel(const u8* tiles, u32 w, u32 x, u32 y)
{
    return tiles[((y / 4) * (w / 8) + x / 8) * 32 + (y % 4) * 8 + x % 8];
}

static u8 clamp(double v)
{
    return v < 0 ? 0 : v > 255 ? 255 : (u8) (v + 0.5);
}

static void write_ppm(const char* path, const u8* ty, const u8* tu, const u8* tv, u32 w, u32 h)
{
    FILE* f = fopen(path, "wb");
    u32 x, y;

    fprintf(f, "P6\n%u %u\n255\n", w, h);
    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            double Y = 1.164 * (texel(ty, w, x, y) - 16), U = texel(tu, w / 2, x / 2, y / 2) - 128.0;
            double V = texel(tv, w / 2, x / 2, y / 2) - 128.0;
            u8 rgb[3] = { clamp(Y + 1.596 * V), clamp(Y - 0.392 * U - 0.813 * V), clamp(Y + 2.017 * U) };

            fwrite(rgb, 1, 3, f);
        }
    }
    fclose(f);
}

// The frame with 0x00 after each 0xFF of the scan, as a JPEG decoder expects.
static void write_jpeg(const char* path, const u8* jpg, u32 size)
{
    FILE* f = fopen(path, "wb");
    u32 i = 2;

    while (i < size) {
        u32 len = (u32) jpg[i + 2] << 8 | jpg[i + 3];

        if (jpg[i + 1] == 0xDA) {
            break;
        }
        i += 2 + len;
    }
    i += 2 + ((u32) jpg[i + 2] << 8 | jpg[i + 3]);
    fwrite(jpg, 1, i, f);
    for (; i < size; i++) {
        fputc(jpg[i], f);
        if (jpg[i] == 0xFF) {
            fputc(0, f);
        }
    }
    fputc(0xFF, f);
    fputc(0xD9, f);
    fclose(f);
}

static int decode_one(const char* prefix, u32 n, u8* jpg, u32 size, u16 w, u16 h)
{
    THPDec_8032FD40_Data desc = { 0, w, h, 4 };
    u16 hdr[2] = { w, h };
    u8* work = malloc(THPDec_8032FD40(&desc, h));
    u8* ty = calloc(1, (size_t) w * h);
    u8* tu = calloc(1, (size_t) w * h / 4);
    u8* tv = calloc(1, (size_t) w * h / 4);
    u8 status = 0xEE;
    THPFileInfo* info;
    char path[256];

    info = THPVideoDecode(hdr, &status, (THPFileInfo*) work, jpg, &desc);
    if (info == NULL) {
        printf("frame %u: status %u\n", n, status);
        return 1;
    }
    if (w == 640) {
        THPDec_80331340(info, ty, tu, tv);
    } else {
        THPDec_803313D0(info, ty, tu, tv, w);
    }
    snprintf(path, sizeof(path), "%s_%04u.ppm", prefix, n);
    write_ppm(path, ty, tu, tv, w, h);
    snprintf(path, sizeof(path), "%s_%04u.jpg", prefix, n);
    write_jpeg(path, jpg, size);
    printf("frame %u: %ux%u, %u bytes, status %u\n", n, w, h, size, status);
    free(work);
    free(ty);
    free(tu);
    free(tv);
    return 0;
}

// thp_test file first count prefix: frames first to first + count - 1 of an MTH file, or a JPEG still.
int main(int argc, char** argv)
{
    FILE* f;
    u8 head[64];
    u32 first, count, size, off, i;
    int fails = 0;

    if (argc != 5 || (f = fopen(argv[1], "rb")) == NULL) {
        printf("usage: thp_test file first count prefix\n");
        return 2;
    }
    first = (u32) atoi(argv[2]);
    count = (u32) atoi(argv[3]);
    fread(head, 1, sizeof(head), f);
    if (head[0] == 0xFF && head[1] == 0xD8) {
        THPDec_8032FD40_Data desc;
        u8* jpg;

        fseek(f, 0, SEEK_END);
        size = (u32) ftell(f);
        jpg = malloc(size + 64);
        fseek(f, 0, SEEK_SET);
        fread(jpg, 1, size, f);
        if (!THPDec_8032F8D4(jpg, &desc)) {
            printf("not a THP still\n");
            return 1;
        }
        return decode_one(argv[4], 0, jpg, size, desc.val1, desc._pad);
    }
    // lbmthp.c's header: size at 0x10, first frame at 0x20, its size at 0x28.
    off = be32(head + 0x20);
    size = be32(head + 0x28);
    for (i = 0; i < first + count; i++) {
        u8* frame = malloc(size + 64);
        u32 next;

        fseek(f, (long) off, SEEK_SET);
        if (fread(frame, 1, size, f) != size) {
            printf("frame %u: short read\n", i);
            return 1;
        }
        next = be32(frame);
        if (i >= first) {
            fails += decode_one(argv[4], i, frame + 4, size - 4, (u16) be32(head + 0x10),
                                (u16) be32(head + 0x14));
        }
        free(frame);
        off += size;
        size = next;
    }
    printf("%s\n", fails ? "FAIL" : "done");
    return fails != 0;
}
