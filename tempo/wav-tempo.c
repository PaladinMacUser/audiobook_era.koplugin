/* Pitch-preserving tempo conversion for sanoTTS PCM output.
 * The model always renders at its native duration scale. Sonic changes the
 * finished waveform without lowering its pitch. Built as a static ARM tool.
 */
#define _POSIX_C_SOURCE 200809L
#include "sonic.h"
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Static_assert(sizeof(short) == 2, "Sonic requires 16-bit samples");

static uint32_t le32(const unsigned char *p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static void put32(unsigned char *p, uint32_t n) {
    for (int i = 0; i < 4; ++i) p[i] = (unsigned char)(n >> (8 * i));
}

static int drain(sonicStream stream, FILE *out, uint32_t *bytes) {
    short samples[4096];
    int n;
    while ((n = sonicReadShortFromStream(stream, samples, 4096)) > 0) {
        if (*bytes > UINT32_MAX - 36u - (unsigned)n * 2u ||
            fwrite(samples, 2, (size_t)n, out) != (size_t)n) return 0;
        *bytes += (unsigned)n * 2u;
    }
    return 1;
}

static int convert(const char *path, float speed) {
    FILE *in = NULL, *out = NULL;
    sonicStream stream = NULL;
    char *temporary = NULL, *timing = NULL;
    int rc = 1, fd = -1;
    unsigned char header[44];
    uint32_t bytes = 0;
    in = fopen(path, "rb");
    if (!in || fread(header, 1, sizeof header, in) != sizeof header) goto done;
    /* snt_server writes this canonical PCM layout. Refuse other formats
     * rather than guessing offsets or changing a file we cannot interpret. */
    if (memcmp(header, "RIFF", 4) || memcmp(header + 8, "WAVEfmt ", 8) ||
        le32(header + 16) != 16 || le16(header + 20) != 1 ||
        le16(header + 22) != 1 || le16(header + 34) != 16 ||
        le16(header + 32) != 2 || memcmp(header + 36, "data", 4)) goto done;
    uint32_t rate = le32(header + 24), remaining = le32(header + 40);
    if (rate < 8000 || rate > 48000 || remaining == 0 || remaining % 2 ||
        le32(header + 28) != rate * 2u || remaining > UINT32_MAX - 36u ||
        le32(header + 4) != remaining + 36u) goto done;
    long position = ftell(in);
    if (fseek(in, 0, SEEK_END) || ftell(in) != (long)remaining + 44 ||
        fseek(in, position, SEEK_SET)) goto done;
    if (fabsf(speed - 1.0f) < 0.001f) { rc = 0; goto done; }
    stream = sonicCreateStream((int)rate, 1);
    if (!stream) goto done;
    sonicSetSpeed(stream, speed);
    sonicSetQuality(stream, 1);
    size_t length = strlen(path);
    temporary = malloc(length + 24);
    timing = malloc(length + 8);
    if (!temporary || !timing) goto done;
    snprintf(temporary, length + 24, "%s.tempo.XXXXXX", path);
    snprintf(timing, length + 8, "%s.timing", path);
    fd = mkstemp(temporary);
    if (fd < 0) goto done;
    out = fdopen(fd, "w+b");
    if (!out) goto done;
    fd = -1;
    if (fwrite(header, 1, sizeof header, out) != sizeof header) goto done;
    short samples[4096];
    while (remaining) {
        size_t n = remaining / 2u;
        if (n > 4096) n = 4096;
        if (fread(samples, 2, n, in) != n ||
            !sonicWriteShortToStream(stream, samples, (int)n) ||
            !drain(stream, out, &bytes)) goto done;
        remaining -= (uint32_t)n * 2u;
    }
    if (!sonicFlushStream(stream) || !drain(stream, out, &bytes) || !bytes) goto done;
    put32(header + 4, bytes + 36u);
    put32(header + 40, bytes);
    if (fseek(out, 0, SEEK_SET) || fwrite(header, 1, 44, out) != 44) goto done;
    int closed = fclose(out);
    out = NULL;
    if (closed || rename(temporary, path)) goto done;
    /* Sonic moves pitch periods locally. Native phoneme timestamps no
     * longer describe the waveform; let KOReader estimate from its duration. */
    unlink(timing);
    fprintf(stderr, "wav-tempo: speed=%.3f, pitch preserved, %u Hz, %u samples\n",
            speed, rate, bytes / 2u);
    rc = 0;
done:
    if (in) fclose(in);
    if (out) fclose(out);
    if (fd >= 0) close(fd);
    if (temporary) { unlink(temporary); free(temporary); }
    free(timing);
    if (stream) sonicDestroyStream(stream);
    if (rc) fprintf(stderr, "wav-tempo: conversion failed for %s\n", path);
    return rc;
}

int main(int argc, char **argv) {
    if (argc != 4 || strcmp(argv[1], "--speed")) {
        fprintf(stderr, "Usage: wav-tempo --speed 0.25..2.0 audio.wav\n");
        return 2;
    }
    char *end;
    errno = 0;
    float speed = strtof(argv[2], &end);
    if (errno || *end || !isfinite(speed) || speed < 0.25f || speed > 2.0f) return 2;
    return convert(argv[3], speed);
}
