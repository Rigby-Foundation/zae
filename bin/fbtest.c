/* SPDX-License-Identifier: GPL-2.0-only */
/* Framebuffer test utility for sic OS. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

static inline void put_pixel(uint32_t *fb, uint32_t pitch_pixels, uint32_t x, uint32_t y, uint32_t color)
{
    fb[y * pitch_pixels + x] = color;
}

static void fill_rect(uint32_t *fb, uint32_t pitch_pixels, uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t color)
{
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            put_pixel(fb, pitch_pixels, x0 + x, y0 + y, color);
        }
    }
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("fbtest: opening /dev/fb0...\n");
    int fd = open("/dev/fb0", O_RDWR);
    if (fd < 0) {
        perror("open /dev/fb0");
        return 1;
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("ioctl FBIOGET_VSCREENINFO");
        close(fd);
        return 1;
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("ioctl FBIOGET_FSCREENINFO");
        close(fd);
        return 1;
    }

    printf("fbtest: resolution %ux%u, %u bpp, pitch %u bytes, smem_len %u bytes\n",
           vinfo.xres, vinfo.yres, vinfo.bits_per_pixel, finfo.line_length, finfo.smem_len);

    size_t screensize = (size_t)finfo.line_length * vinfo.yres;
    uint32_t *fb = mmap(NULL, screensize, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (fb == MAP_FAILED) {
        perror("mmap /dev/fb0");
        close(fd);
        return 1;
    }

    printf("fbtest: mmap succeeded at %p, switching to KD_GRAPHICS...\n", (void *)fb);
    ioctl(fd, KDSETMODE, KD_GRAPHICS);

    uint32_t pitch_pixels = finfo.line_length / 4;

    /* Draw full screen dark slate blue background */
    fill_rect(fb, pitch_pixels, 0, 0, vinfo.xres, vinfo.yres, 0x181824);

    /* Draw 7 standard color bars */
    uint32_t colors[] = {
        0xFF0000, /* Red */
        0x00FF00, /* Green */
        0x0000FF, /* Blue */
        0xFFFF00, /* Yellow */
        0x00FFFF, /* Cyan */
        0xFF00FF, /* Magenta */
        0xFFFFFF  /* White */
    };
    uint32_t bar_w = vinfo.xres / 7;
    uint32_t bar_h = vinfo.yres / 3;
    for (int i = 0; i < 7; i++) {
        fill_rect(fb, pitch_pixels, i * bar_w, 40, bar_w, bar_h, colors[i]);
    }

    /* Draw gradient */
    uint32_t grad_y = 60 + bar_h;
    uint32_t grad_h = 80;
    for (uint32_t x = 0; x < vinfo.xres; x++) {
        uint32_t c = (x * 255) / vinfo.xres;
        uint32_t rgb = (c << 16) | (c << 8) | c;
        for (uint32_t y = 0; y < grad_h; y++) {
            put_pixel(fb, pitch_pixels, x, grad_y + y, rgb);
        }
    }

    /* Animate a bouncing square for ~60 frames */
    int bx = 50, by = grad_y + grad_h + 30;
    int dx = 4, dy = 3;
    int bsize = 40;
    int box_min_y = grad_y + grad_h + 10;
    int box_max_y = (int)vinfo.yres - bsize - 10;
    int box_max_x = (int)vinfo.xres - bsize - 10;

    for (int frame = 0; frame < 90; frame++) {
        /* Erase old box */
        fill_rect(fb, pitch_pixels, bx, by, bsize, bsize, 0x181824);

        bx += dx;
        by += dy;
        if (bx <= 10 || bx >= box_max_x) dx = -dx;
        if (by <= box_min_y || by >= box_max_y) dy = -dy;

        /* Draw new box */
        fill_rect(fb, pitch_pixels, bx, by, bsize, bsize, 0x00E5FF);

        usleep(16000); /* ~60 fps */
    }

    /* Restore text mode */
    ioctl(fd, KDSETMODE, KD_TEXT);
    munmap(fb, screensize);
    close(fd);

    printf("fbtest: finished successfully!\n");
    return 0;
}
