/* SPDX-License-Identifier: GPL-2.0-only */
/* Copyright (C) 2026 Rigby Foundation */
/* beep [hz] [seconds]: a sine wave through /dev/dsp. Also the sound self
 * test: reports the device's format and how long the write took. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <math.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/soundcard.h>

int main(int argc, char **argv)
{
    double hz = argc > 1 ? atof(argv[1]) : 440.0, secs = argc > 2 ? atof(argv[2]) : 1.0;
    int fd = open("/dev/dsp", O_WRONLY);
    if (fd < 0) { perror("/dev/dsp"); return 1; }
    int fmt = AFMT_S16_LE, ch = 2, rate = 48000;
    ioctl(fd, SNDCTL_DSP_SETFMT, &fmt);
    ioctl(fd, SNDCTL_DSP_CHANNELS, &ch);
    ioctl(fd, SNDCTL_DSP_SPEED, &rate);
    audio_buf_info bi = { 0 };
    ioctl(fd, SNDCTL_DSP_GETOSPACE, &bi);
    printf("beep: /dev/dsp %d Hz, %d channels, fmt %#x, %d fragments of %d bytes\n", rate, ch, fmt, bi.fragstotal, bi.fragsize);
    if (fmt != AFMT_S16_LE || ch != 2) { fprintf(stderr, "beep: unexpected format\n"); return 1; }
    int frames = (int)(rate * secs);
    short *buf = malloc((size_t)frames * 4);
    for (int i = 0; i < frames; i++) {
        short v = (short)(12000 * sin(2 * M_PI * hz * i / rate));
        buf[2 * i] = v; buf[2 * i + 1] = v;
    }
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    size_t off = 0, total = (size_t)frames * 4;
    while (off < total) {
        ssize_t n = write(fd, (char *)buf + off, total - off);
        if (n <= 0) { perror("write"); return 1; }
        off += (size_t)n;
    }
    ioctl(fd, SNDCTL_DSP_SYNC, 0);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    printf("beep: %.0f Hz for %.1f s played in %ld ms\n", hz, secs, (long)((t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000));
    close(fd);
    return 0;
}
