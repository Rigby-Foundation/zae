/* SPDX-License-Identifier: GPL-2.0-only */
#include "doomgeneric.h"
#include "doomkeys.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/fb.h>

#define KEY_QUEUE_SIZE 256
static struct {
    int pressed;
    unsigned char key;
} key_queue[KEY_QUEUE_SIZE];
static int kq_head = 0, kq_tail = 0;

static void queue_key(int pressed, unsigned char key)
{
    int next = (kq_head + 1) % KEY_QUEUE_SIZE;
    if (next != kq_tail) {
        key_queue[kq_head].pressed = pressed;
        key_queue[kq_head].key = key;
        kq_head = next;
    }
}

/* Keyboard: /dev/console in K_RAW mode hands us PS/2 set 1 scancodes, so
 * every key has a real press and release (0x80 set on the break code, 0xE0
 * before the extended keys). If raw mode isn't available (an old kernel, or
 * input from the serial line) fall back to cooked characters with a timed
 * "hold", which is playable but mushy. */
static int raw_mode;

static unsigned char scancode_to_doom(unsigned char sc, int ext)
{
    if (ext) {
        switch (sc) {
        case 0x48: return KEY_UPARROW;
        case 0x50: return KEY_DOWNARROW;
        case 0x4B: return KEY_LEFTARROW;
        case 0x4D: return KEY_RIGHTARROW;
        case 0x1D: return KEY_FIRE;         /* right ctrl */
        case 0x38: return KEY_RALT;         /* right alt: strafe */
        case 0x1C: return KEY_ENTER;        /* keypad enter */
        }
        return 0;
    }
    static const unsigned char row1[] = "1234567890-=";
    if (sc >= 0x02 && sc <= 0x0D) return row1[sc - 0x02];
    static const unsigned char row2[] = "qwertyuiop[]";
    if (sc >= 0x10 && sc <= 0x1B) return row2[sc - 0x10];
    static const unsigned char row3[] = "asdfghjkl;'`";
    if (sc >= 0x1E && sc <= 0x29) return row3[sc - 0x1E];
    static const unsigned char row4[] = "\\zxcvbnm,./";
    if (sc >= 0x2B && sc <= 0x35) return row4[sc - 0x2B];
    switch (sc) {
    case 0x01: return KEY_ESCAPE;
    case 0x0E: return KEY_BACKSPACE;
    case 0x0F: return KEY_TAB;
    case 0x1C: return KEY_ENTER;
    case 0x1D: return KEY_FIRE;             /* left ctrl */
    case 0x2A: return KEY_RSHIFT;           /* left shift: run */
    case 0x36: return KEY_RSHIFT;
    case 0x38: return KEY_LALT;             /* left alt: strafe */
    case 0x39: return KEY_USE;              /* space */
    case 0x3A: return KEY_CAPSLOCK;
    case 0x3B: return KEY_F1; case 0x3C: return KEY_F2; case 0x3D: return KEY_F3;
    case 0x3E: return KEY_F4; case 0x3F: return KEY_F5; case 0x40: return KEY_F6;
    case 0x41: return KEY_F7; case 0x42: return KEY_F8; case 0x43: return KEY_F9;
    case 0x44: return KEY_F10; case 0x57: return KEY_F11; case 0x58: return KEY_F12;
    case 0x0C: return KEY_MINUS;
    case 0x0D: return KEY_EQUALS;
    case 0x45: return KEY_PAUSE;
    }
    return 0;
}

static void poll_raw(void)
{
    static int ext;
    unsigned char buf[64];
    long n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned char b = buf[i];
            if (b == 0xE0) { ext = 1; continue; }
            if (b == 0xE1) { continue; }            /* pause sequence: ignore */
            int released = b & 0x80;
            unsigned char dk = scancode_to_doom(b & 0x7F, ext);
            ext = 0;
            if (!dk) continue;
            /* WASD as movement/strafe keys, like most source ports */
            if (dk == 'w') dk = KEY_UPARROW;
            else if (dk == 's') dk = KEY_DOWNARROW;
            else if (dk == 'a') dk = KEY_STRAFE_L;
            else if (dk == 'd') dk = KEY_STRAFE_R;
            else if (dk == 'e') dk = KEY_USE;
            queue_key(!released, dk);
        }
    }
}

#define MAX_KEYS 256
static uint32_t key_held_until[MAX_KEYS];
static uint8_t  key_is_down[MAX_KEYS];

/* Cooked fallback: a character means "held for a moment". */
static void press_key(unsigned char dk, uint32_t hold_ms)
{
    if (!dk) return;
    uint32_t now = DG_GetTicksMs();
    static const unsigned char opposite[][2] = {
        { KEY_UPARROW, KEY_DOWNARROW }, { KEY_DOWNARROW, KEY_UPARROW },
        { KEY_LEFTARROW, KEY_RIGHTARROW }, { KEY_RIGHTARROW, KEY_LEFTARROW },
        { KEY_STRAFE_L, KEY_STRAFE_R }, { KEY_STRAFE_R, KEY_STRAFE_L },
    };
    for (size_t i = 0; i < sizeof opposite / sizeof opposite[0]; i++)
        if (dk == opposite[i][0] && key_is_down[opposite[i][1]]) {
            key_is_down[opposite[i][1]] = 0;
            key_held_until[opposite[i][1]] = 0;
            queue_key(0, opposite[i][1]);
        }
    if (!key_is_down[dk]) {
        key_is_down[dk] = 1;
        queue_key(1, dk);
    }
    key_held_until[dk] = now + hold_ms;
}

static void expire_keys(void)
{
    uint32_t now = DG_GetTicksMs();
    for (int k = 0; k < MAX_KEYS; k++)
        if (key_is_down[k] && (int32_t)(now - key_held_until[k]) >= 0) {
            key_is_down[k] = 0;
            key_held_until[k] = 0;
            queue_key(0, (unsigned char)k);
        }
}

static int esc_state;
static uint32_t esc_time;

static void poll_keys(void)
{
    char buf[64];
    long n;
    while ((n = read(0, buf, sizeof buf)) > 0) {
        for (long i = 0; i < n; i++) {
            unsigned char c = (unsigned char)buf[i];
            if (esc_state == 0 && c == 27) { esc_state = 1; esc_time = DG_GetTicksMs(); continue; }
            if (esc_state == 1) {
                if (c == '[' || c == 'O') { esc_state = 2; continue; }
                press_key(KEY_ESCAPE, 120);
                esc_state = 0;
            } else if (esc_state == 2) {
                esc_state = 0;
                unsigned char dk = c == 'A' ? KEY_UPARROW : c == 'B' ? KEY_DOWNARROW : c == 'C' ? KEY_RIGHTARROW : c == 'D' ? KEY_LEFTARROW : 0;
                if (dk) press_key(dk, 250);
                continue;
            }
            switch (c) {
            case 'w': press_key(KEY_UPARROW, 250); break;
            case 'W': press_key(KEY_UPARROW, 250); press_key(KEY_RSHIFT, 250); break;
            case 's': press_key(KEY_DOWNARROW, 250); break;
            case 'S': press_key(KEY_DOWNARROW, 250); press_key(KEY_RSHIFT, 250); break;
            case 'a': case ',': case '<': press_key(KEY_STRAFE_L, 250); break;
            case 'A': press_key(KEY_STRAFE_L, 250); press_key(KEY_RSHIFT, 250); break;
            case 'd': case '.': case '>': press_key(KEY_STRAFE_R, 250); break;
            case 'D': press_key(KEY_STRAFE_R, 250); press_key(KEY_RSHIFT, 250); break;
            case 'q': case 'Q': press_key(KEY_LEFTARROW, 250); break;
            case 'r': case 'R': press_key(KEY_RIGHTARROW, 250); break;
            case ' ': case 'x': case 'X': case 'z': case 'Z': press_key(KEY_FIRE, 120); break;
            case 'e': case 'E': case 'f': case 'F': press_key(KEY_USE, 120); break;
            case '\n': case '\r': press_key(KEY_ENTER, 120); press_key(KEY_USE, 120); break;
            case '\t': press_key(KEY_TAB, 120); break;
            case 27: press_key(KEY_ESCAPE, 120); break;
            case 'y': case 'Y': press_key('y', 120); break;
            case 'n': case 'N': press_key('n', 120); break;
            default:
                if (c >= '1' && c <= '7') press_key(c, 120);
                else if (c >= 'a' && c <= 'z') press_key(c, 200);
                break;
            }
        }
    }
    if (esc_state == 1 && (int32_t)(DG_GetTicksMs() - esc_time) > 60) {
        press_key(KEY_ESCAPE, 120);
        esc_state = 0;
    }
    expire_keys();
}

int DG_GetKey(int *pressed, unsigned char *key)
{
    if (kq_head == kq_tail) {
        if (raw_mode) poll_raw(); else poll_keys();
    }
    if (kq_head == kq_tail)
        return 0;
    *pressed = key_queue[kq_tail].pressed;
    *key = key_queue[kq_tail].key;
    kq_tail = (kq_tail + 1) % KEY_QUEUE_SIZE;
    return 1;
}

static int fb_fd = -1;

static int stdin_flags = -1;

static void restore_console(void)
{
    ioctl(0, KDSKBMODE, K_XLATE);
    if (stdin_flags >= 0)
        fcntl(0, F_SETFL, stdin_flags);     /* the shell shares this file description */
    if (fb_fd >= 0)
        ioctl(fb_fd, KDSETMODE, KD_TEXT);
}
static uint32_t *fb_pixels = NULL;
static size_t fb_size = 0;
static uint32_t pitch_pixels = 0;
static int off_x = 0, off_y = 0;
static int render_w = DOOMGENERIC_RESX, render_h = DOOMGENERIC_RESY;
static int scale2x = 0;

void DG_Init(void)
{
    fb_fd = open("/dev/fb0", O_RDWR);
    if (fb_fd < 0) {
        perror("open /dev/fb0");
        exit(1);
    }

    struct fb_var_screeninfo vinfo;
    if (ioctl(fb_fd, FBIOGET_VSCREENINFO, &vinfo) != 0) {
        perror("ioctl FBIOGET_VSCREENINFO");
        exit(1);
    }

    struct fb_fix_screeninfo finfo;
    if (ioctl(fb_fd, FBIOGET_FSCREENINFO, &finfo) != 0) {
        perror("ioctl FBIOGET_FSCREENINFO");
        exit(1);
    }

    fb_size = (size_t)finfo.line_length * vinfo.yres;
    fb_pixels = mmap(NULL, fb_size, PROT_READ | PROT_WRITE, MAP_SHARED, fb_fd, 0);
    if (fb_pixels == MAP_FAILED) {
        perror("mmap /dev/fb0");
        exit(1);
    }

    /* Switch console to graphics mode */
    ioctl(fb_fd, KDSETMODE, KD_GRAPHICS);

    pitch_pixels = finfo.line_length / 4;
    if (vinfo.xres >= DOOMGENERIC_RESX * 2 && vinfo.yres >= DOOMGENERIC_RESY * 2) {
        scale2x = 1;
        render_w = DOOMGENERIC_RESX * 2;
        render_h = DOOMGENERIC_RESY * 2;
        off_x = ((int)vinfo.xres - render_w) / 2;
        off_y = ((int)vinfo.yres - render_h) / 2;
    } else {
        scale2x = 0;
        render_w = DOOMGENERIC_RESX;
        render_h = DOOMGENERIC_RESY;
        if (render_w > (int)vinfo.xres) render_w = (int)vinfo.xres;
        if (render_h > (int)vinfo.yres) render_h = (int)vinfo.yres;
        off_x = ((int)vinfo.xres - render_w) / 2;
        off_y = ((int)vinfo.yres - render_h) / 2;
    }

    /* Black out the screen */
    memset(fb_pixels, 0, fb_size);

    /* Make stdin non-blocking, and raw if the console supports it */
    int flags = fcntl(0, F_GETFL, 0);
    stdin_flags = flags;
    if (flags >= 0)
        fcntl(0, F_SETFL, flags | O_NONBLOCK);
    raw_mode = ioctl(0, KDSKBMODE, K_RAW) == 0;
    atexit(restore_console);
}

void DG_DrawFrame(void)
{
    if (scale2x) {
        for (int y = 0; y < DOOMGENERIC_RESY; y++) {
            const uint32_t *src = DG_ScreenBuffer + y * DOOMGENERIC_RESX;
            uint32_t *dst0 = fb_pixels + (off_y + y * 2) * pitch_pixels + off_x;
            uint32_t *dst1 = dst0 + pitch_pixels;
            for (int x = 0; x < DOOMGENERIC_RESX; x++) {
                uint32_t p = src[x];
                dst0[x * 2] = p;
                dst0[x * 2 + 1] = p;
                dst1[x * 2] = p;
                dst1[x * 2 + 1] = p;
            }
        }
    } else {
        for (int y = 0; y < render_h; y++) {
            uint32_t *dst = fb_pixels + (off_y + y) * pitch_pixels + off_x;
            const uint32_t *src = DG_ScreenBuffer + y * DOOMGENERIC_RESX;
            memcpy(dst, src, render_w * sizeof(uint32_t));
        }
    }
}

void DG_SleepMs(uint32_t ms)
{
    if (ms > 0)
        usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}


void DG_SetWindowTitle(const char *title)
{
    (void)title;
}

int main(int argc, char **argv)
{
    doomgeneric_Create(argc, argv);
    while (1) {
        doomgeneric_Tick();
    }
    return 0;
}
