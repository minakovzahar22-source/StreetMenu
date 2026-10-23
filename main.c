/*
 * StreetMenu - animated game carousel launcher for PSP (tested target: PSP Street E1008, CFW 6.61)
 * Build: see Makefile (PSPSDK + psp-libpng required)
 */
#include <pspkernel.h>
#include <pspdisplay.h>
#include <pspdebug.h>
#include <pspctrl.h>
#include <pspgu.h>
#include <psppower.h>
#include <pspiofilemgr.h>
#include <png.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <strings.h>

PSP_MODULE_INFO("StreetMenu", 0, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER | THREAD_ATTR_VFPU);
int sce_newlib_heap_kb_size = -1024;

#define BUF_W      512
#define SCR_W      480
#define SCR_H      272
#define FRAME_SIZE (BUF_W * SCR_H * 4)
#define TEX_W      256
#define TEX_H      128
#define MAXG       200
#define PI2        6.2831853f

typedef struct { u32 c; float x, y, z; } CV;
typedef struct { float u, v; u32 c; float x, y, z; } TV;

typedef struct {
    char path[300];
    char title[64];
    u32 *tex;
    int  w, h;
    int  tried;
} Game;

static Game games[MAXG];
static int  ngames = 0;

static unsigned int __attribute__((aligned(16))) list[262144];
static void *draw_buf = 0;

/* ---------------- exit callback ---------------- */
static int exit_cb(int a, int b, void *c) { sceKernelExitGame(); return 0; }
static int cb_thread(SceSize args, void *argp) {
    int id = sceKernelCreateCallback("exit_cb", exit_cb, NULL);
    sceKernelRegisterExitCallback(id);
    sceKernelSleepThreadCB();
    return 0;
}
static void setup_callbacks(void) {
    int t = sceKernelCreateThread("cb", cb_thread, 0x11, 0xFA0, 0, 0);
    if (t >= 0) sceKernelStartThread(t, 0, 0);
}

/* ---------------- helpers ---------------- */
static float clampf(float v, float a, float b) { return v < a ? a : (v > b ? b : v); }
static float smooth(float t) { t = clampf(t, 0, 1); return t * t * (3 - 2 * t); }
static u32 col(int r, int g, int b, float a) {
    int ai = (int)(clampf(a, 0, 1) * 255.0f);
    return ((u32)ai << 24) | ((u32)b << 16) | ((u32)g << 8) | (u32)r;
}
static u32 rd32(const u8 *p) { u32 v; memcpy(&v, p, 4); return v; }
static u16 rd16(const u8 *p) { u16 v; memcpy(&v, p, 2); return v; }

/* ---------------- PARAM.SFO / PBP ---------------- */
static void parse_title(const u8 *s, size_t n, char *out, size_t os) {
    if (n < 20 || memcmp(s, "\0PSF", 4)) return;
    u32 kt = rd32(s + 8), dt = rd32(s + 12), cnt = rd32(s + 16);
    for (u32 i = 0; i < cnt && 20 + (i + 1) * 16 <= n; i++) {
        const u8 *e = s + 20 + i * 16;
        u32 ko = rd16(e), len = rd32(e + 4), doff = rd32(e + 12);
        if (kt + ko + 6 > n || dt + doff + 1 > n) continue;
        if (strncmp((const char *)s + kt + ko, "TITLE", 6) == 0) {
            u32 m = len < os - 1 ? len : (u32)(os - 1);
            if (dt + doff + m > n) m = n - dt - doff;
            memcpy(out, s + dt + doff, m);
            out[m] = 0;
            for (size_t k = 0; out[k]; k++)
                if ((u8)out[k] > 126 || (u8)out[k] < 32) out[k] = '?';
            return;
        }
    }
}

static void scan_games(void) {
    SceUID d = sceIoDopen("ms0:/PSP/GAME");
    if (d < 0) return;
    SceIoDirent e;
    memset(&e, 0, sizeof(e));
    while (sceIoDread(d, &e) > 0 && ngames < MAXG) {
        if (FIO_S_ISDIR(e.d_stat.st_mode) && e.d_name[0] != '.' && strcmp(e.d_name, "StreetMenu") != 0) {
            Game *g = &games[ngames];
            memset(g, 0, sizeof(*g));
            snprintf(g->path, sizeof(g->path), "ms0:/PSP/GAME/%s/EBOOT.PBP", e.d_name);
            SceUID fd = sceIoOpen(g->path, PSP_O_RDONLY, 0);
            if (fd >= 0) {
                u32 hdr[10];
                strncpy(g->title, e.d_name, sizeof(g->title) - 1);
                if (sceIoRead(fd, hdr, 40) == 40 && hdr[0] == 0x50425000) {
                    u32 sz = hdr[3] - hdr[2];
                    if (sz > 0 && sz < 32768) {
                        u8 *buf = (u8 *)malloc(sz);
                        if (buf) {
                            sceIoLseek32(fd, hdr[2], PSP_SEEK_SET);
                            if (sceIoRead(fd, buf, sz) == (int)sz)
                                parse_title(buf, sz, g->title, sizeof(g->title));
                            free(buf);
                        }
                    }
                    ngames++;
                }
                sceIoClose(fd);
            }
        }
        memset(&e, 0, sizeof(e));
    }
    sceIoDclose(d);
}

static int cmp_games(const void *a, const void *b) {
    return strcasecmp(((const Game *)a)->title, ((const Game *)b)->title);
}

/* ---------------- PNG from memory ---------------- */
typedef struct { const u8 *d; size_t pos, len; } Mem;
static void png_mem_read(png_structp p, png_bytep out, png_size_t n) {
    Mem *m = (Mem *)png_get_io_ptr(p);
    if (m->pos + n > m->len) { png_error(p, "eof"); return; }
    memcpy(out, m->d + m->pos, n);
    m->pos += n;
}

static u32 *decode_png(const u8 *data, size_t len, int *ow, int *oh) {
    if (len < 8 || png_sig_cmp((png_bytep)data, 0, 8)) return NULL;
    png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING, NULL, NULL, NULL);
    if (!png) return NULL;
    png_infop info = png_create_info_struct(png);
    u32 *volatile pix = NULL;
    if (!info || setjmp(png_jmpbuf(png))) {
        png_destroy_read_struct(&png, &info, NULL);
        if (pix) free(pix);
        return NULL;
    }
    Mem m = { data, 0, len };
    png_set_read_fn(png, &m, png_mem_read);
    png_read_info(png, info);
    png_uint_32 w = png_get_image_width(png, info), h = png_get_image_height(png, info);
    int bd = png_get_bit_depth(png, info), ct = png_get_color_type(png, info);
    if (w > TEX_W || h > TEX_H) png_error(png, "too big");
    if (bd == 16) png_set_strip_16(png);
    if (ct == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
    if (ct == PNG_COLOR_TYPE_GRAY && bd < 8) png_set_expand_gray_1_2_4_to_8(png);
    if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
    if (ct == PNG_COLOR_TYPE_GRAY || ct == PNG_COLOR_TYPE_GRAY_ALPHA) png_set_gray_to_rgb(png);
    png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
    png_read_update_info(png, info);

    pix = (u32 *)memalign(16, TEX_W * TEX_H * 4);
    if (!pix) png_error(png, "oom");
    memset(pix, 0, TEX_W * TEX_H * 4);
    png_bytep rows[TEX_H];
    for (png_uint_32 y = 0; y < h; y++) rows[y] = (png_bytep)(pix + y * TEX_W);
    png_read_image(png, rows);
    png_destroy_read_struct(&png, &info, NULL);
    *ow = (int)w; *oh = (int)h;
    return pix;
}

static void load_icon(Game *g) {
    g->tried = 1;
    SceUID fd = sceIoOpen(g->path, PSP_O_RDONLY, 0);
    if (fd < 0) return;
    u32 hdr[10];
    if (sceIoRead(fd, hdr, 40) == 40 && hdr[0] == 0x50425000) {
        u32 sz = hdr[4] - hdr[3];
        if (sz > 8 && sz < 262144) {
            u8 *buf = (u8 *)malloc(sz);
            if (buf) {
                sceIoLseek32(fd, hdr[3], PSP_SEEK_SET);
                if (sceIoRead(fd, buf, sz) == (int)sz)
                    g->tex = decode_png(buf, sz, &g->w, &g->h);
                free(buf);
                if (g->tex) sceKernelDcacheWritebackAll();
            }
        }
    }
    sceIoClose(fd);
}

static void unload_icon(Game *g) {
    if (g->tex) { free(g->tex); g->tex = NULL; }
    g->tried = 0;
}

/* ---------------- GU ---------------- */
static void gu_init(void) {
    sceGuInit();
    sceGuStart(GU_DIRECT, list);
    sceGuDrawBuffer(GU_PSM_8888, (void *)0, BUF_W);
    sceGuDispBuffer(SCR_W, SCR_H, (void *)FRAME_SIZE, BUF_W);
    sceGuDepthBuffer((void *)(2 * FRAME_SIZE), BUF_W);
    sceGuOffset(2048 - SCR_W / 2, 2048 - SCR_H / 2);
    sceGuViewport(2048, 2048, SCR_W, SCR_H);
    sceGuScissor(0, 0, SCR_W, SCR_H);
    sceGuEnable(GU_SCISSOR_TEST);
    sceGuShadeModel(GU_SMOOTH);
    sceGuEnable(GU_BLEND);
    sceGuBlendFunc(GU_ADD, GU_SRC_ALPHA, GU_ONE_MINUS_SRC_ALPHA, 0, 0);
    sceGuFinish();
    sceGuSync(0, 0);
    sceDisplayWaitVblankStart();
    sceGuDisplay(GU_TRUE);
    draw_buf = (void *)0;
}

static void rect_col(float x, float y, float w, float h, u32 c1, u32 c2) {
    CV *v = (CV *)sceGuGetMemory(4 * sizeof(CV));
    v[0] = (CV){ c1, x, y, 0 };     v[1] = (CV){ c1, x + w, y, 0 };
    v[2] = (CV){ c2, x, y + h, 0 }; v[3] = (CV){ c2, x + w, y + h, 0 };
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
}

static void glow(float cx, float cy, float r, float a) {
    CV *v = (CV *)sceGuGetMemory(10 * sizeof(CV));
    v[0] = (CV){ col(200, 230, 255, a), cx, cy, 0 };
    for (int i = 0; i < 9; i++) {
        float an = i * (PI2 / 8.0f);
        v[i + 1] = (CV){ col(200, 230, 255, 0), cx + cosf(an) * r, cy + sinf(an) * r, 0 };
    }
    sceGuDisable(GU_TEXTURE_2D);
    sceGuDrawArray(GU_TRIANGLE_FAN, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 10, 0, v);
}

static void quad_tex(u32 *tex, int iw, int ih, float x, float y, float w, float h,
                     u32 ctop, u32 cbot, int flip) {
    TV *v = (TV *)sceGuGetMemory(4 * sizeof(TV));
    float v0 = flip ? (float)ih : 0.0f, v1 = flip ? 0.0f : (float)ih;
    v[0] = (TV){ 0,         v0, ctop, x,     y,     0 };
    v[1] = (TV){ (float)iw, v0, ctop, x + w, y,     0 };
    v[2] = (TV){ 0,         v1, cbot, x,     y + h, 0 };
    v[3] = (TV){ (float)iw, v1, cbot, x + w, y + h, 0 };
    sceGuEnable(GU_TEXTURE_2D);
    sceGuTexMode(GU_PSM_8888, 0, 0, 0);
    sceGuTexImage(0, TEX_W, TEX_H, TEX_W, tex);
    sceGuTexFunc(GU_TFX_MODULATE, GU_TCC_RGBA);
    sceGuTexFilter(GU_LINEAR, GU_LINEAR);
    sceGuTexWrap(GU_CLAMP, GU_CLAMP);
    sceGuDrawArray(GU_TRIANGLE_STRIP, GU_TEXTURE_32BITF | GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, 4, 0, v);
}

/* ---------------- scene ---------------- */
static void draw_background(float t) {
    int r1 = 20 + (int)(15 * sinf(t * 0.15f)), g1 = 50 + (int)(25 * sinf(t * 0.11f + 1)), b1 = 130 + (int)(40 * sinf(t * 0.09f + 2));
    int r2 = 4 + (int)(4 * sinf(t * 0.13f)), g2 = 10 + (int)(8 * sinf(t * 0.1f)), b2 = 40 + (int)(14 * sinf(t * 0.07f));
    rect_col(0, 0, SCR_W, SCR_H, col(r1, g1, b1, 1), col(r2, g2, b2, 1));

    sceGuDisable(GU_TEXTURE_2D);
    for (int w = 0; w < 3; w++) {
        const int N = 24;
        CV *v = (CV *)sceGuGetMemory(sizeof(CV) * (N + 1) * 2);
        float base = 150 + w * 34, amp = 14 + w * 5, f = 0.012f + w * 0.004f, sp = 0.6f + w * 0.25f;
        for (int i = 0; i <= N; i++) {
            float x = i * (float)SCR_W / N;
            float y = base + amp * sinf(x * f * 2.0f + t * sp + w * 2.0f);
            v[i * 2]     = (CV){ col(255, 255, 255, 0.11f - 0.025f * w), x, y, 0 };
            v[i * 2 + 1] = (CV){ col(255, 255, 255, 0.0f), x, y + 70, 0 };
        }
        sceGuDrawArray(GU_TRIANGLE_STRIP, GU_COLOR_8888 | GU_VERTEX_32BITF | GU_TRANSFORM_2D, (N + 1) * 2, 0, v);
    }
    for (int i = 0; i < 14; i++) {
        float ph = fmodf(t * (0.03f + 0.01f * (i % 5)) + i * 0.37f, 1.0f);
        float x = fmodf(i * 97.0f, (float)SCR_W) + 12 * sinf(t * 0.8f + i);
        float y = 285 - ph * 310;
        float r = 6 + (i % 4) * 5;
        glow(x, y, r, sinf(ph * 3.14159f) * 0.28f);
    }
}

static void draw_item(Game *g, float d, float t, float intro, float launch) {
    float ad = fabsf(d), sgn = d < 0 ? -1.0f : 1.0f;
    float k = smooth(fminf(ad, 1.0f));
    float spread = 0.6f + 0.4f * intro;
    float x = 240 + sgn * ((ad < 1 ? ad * 160.0f : 160.0f + (ad - 1) * 85.0f)) * spread;
    float scale = 1.3f - 0.58f * k - (ad > 1 ? 0.04f * (ad - 1) : 0);
    if (scale < 0.4f) scale = 0.4f;
    float alpha = (1.0f - 0.25f * k) * intro;
    if (ad > 2.5f) alpha *= clampf(1.0f - (ad - 2.5f) / 1.2f, 0, 1);
    if (alpha <= 0.01f) return;

    float cy = 108 + (ad < 0.5f ? sinf(t * 2.0f) * 3.0f * (1 - ad * 2) : 0);
    if (ad < 0.5f && launch > 0) scale *= 1.0f + 0.7f * smooth(launch);

    float ih = 80.0f * scale;
    float iw = (g->tex && g->h > 0) ? ih * (float)g->w / (float)g->h : 144.0f * scale;
    float px = x - iw / 2, py = cy - ih / 2;

    /* soft shadow + selection glow */
    rect_col(px - 4, py + ih + 2, iw + 8, 10, col(0, 0, 0, 0.25f * alpha), col(0, 0, 0, 0));
    float gl = (1.0f - k) * (0.22f + 0.08f * sinf(t * 3.0f)) * alpha;
    if (gl > 0.01f) rect_col(px - 7, py - 7, iw + 14, ih + 14, col(255, 255, 255, gl), col(160, 200, 255, gl));

    if (g->tex) {
        quad_tex(g->tex, g->w, g->h, px, py, iw, ih, col(255, 255, 255, alpha), col(255, 255, 255, alpha), 0);
        /* reflection */
        float rh = ih * 0.55f;
        quad_tex(g->tex, g->w, g->h, px, py + ih + 3, iw, rh,
                 col(255, 255, 255, 0.32f * alpha), col(255, 255, 255, 0), 1);
    } else {
        rect_col(px, py, iw, ih, col(70, 90, 130, alpha), col(30, 40, 70, alpha));
    }
}

static void draw_text_centered(const char *s, int row) {
    int len = (int)strlen(s);
    int cx = (68 - len) / 2;
    if (cx < 0) cx = 0;
    pspDebugScreenSetXY(cx, row);
    pspDebugScreenPrintf("%s", s);
}

static void launch(const char *path) {
    struct SceKernelLoadExecParam p;
    memset(&p, 0, sizeof(p));
    p.size = sizeof(p);
    p.args = strlen(path) + 1;
    p.argp = (void *)path;
    p.key = "game";
    sceKernelLoadExec(path, &p);
}

int main(void) {
    setup_callbacks();
    scePowerSetClockFrequency(333, 333, 166);
    sceCtrlSetSamplingCycle(0);
    sceCtrlSetSamplingMode(PSP_CTRL_MODE_ANALOG);

    pspDebugScreenInit();
    pspDebugScreenEnableBackColor(0);

    scan_games();
    if (ngames > 1) qsort(games, ngames, sizeof(Game), cmp_games);

    gu_init();

    int target = 0;
    float scroll = 0, hold = 0;
    int prev_dir = 0, launching = 0;
    float ltime = 0, t = 0;
    u32 old = 0;
    u64 last = sceKernelGetSystemTimeWide();

    while (1) {
        u64 now = sceKernelGetSystemTimeWide();
        float dt = (float)(now - last) / 1000000.0f;
        last = now;
        if (dt > 0.05f) dt = 0.05f;
        t += dt;
        float intro = smooth(t / 0.9f);

        /* ---- input ---- */
        SceCtrlData pad;
        sceCtrlReadBufferPositive(&pad, 1);
        u32 pressed = pad.Buttons & ~old;
        old = pad.Buttons;

        if (!launching && ngames > 0) {
            int dir = 0;
            if ((pad.Buttons & PSP_CTRL_LEFT) || pad.Lx < 64) dir = -1;
            else if ((pad.Buttons & PSP_CTRL_RIGHT) || pad.Lx > 192) dir = 1;
            int fire = 0;
            if (dir != prev_dir) { fire = (dir != 0); hold = 0; }
            else if (dir) {
                hold += dt;
                if (hold > 0.3f) { hold = 0.3f - 0.11f; fire = 1; }
            }
            prev_dir = dir;
            if (fire) target += dir;
            if (pressed & PSP_CTRL_LTRIGGER) target -= 5;
            if (pressed & PSP_CTRL_RTRIGGER) target += 5;
            if (target < 0) target = 0;
            if (target > ngames - 1) target = ngames - 1;
            if ((pressed & PSP_CTRL_CROSS) && fabsf(target - scroll) < 0.6f) { launching = 1; ltime = 0; }
        }
        if (pressed & PSP_CTRL_TRIANGLE) sceKernelExitGame();

        /* ---- animation ---- */
        scroll += (target - scroll) * (1.0f - expf(-dt * 10.0f));
        if (fabsf(target - scroll) < 0.002f) scroll = (float)target;
        if (launching) {
            ltime += dt;
            if (ltime > 0.55f) {
                launch(games[target].path);
                launching = 0; /* only reached if launch failed */
            }
        }

        /* ---- icon cache: one load per frame, window +-4 ---- */
        int c = (int)floorf(scroll + 0.5f);
        for (int i = 0; i < ngames; i++)
            if (abs(i - c) > 6 && games[i].tex) unload_icon(&games[i]);
        for (int dd = 0; dd <= 4; dd++) {
            int done = 0;
            for (int s = -1; s <= 1 && !done; s += 2) {
                int i = c + s * dd;
                if (i >= 0 && i < ngames && !games[i].tried) { load_icon(&games[i]); done = 1; }
                if (dd == 0) break;
            }
            if (done) break;
        }

        /* ---- draw ---- */
        sceGuStart(GU_DIRECT, list);
        draw_background(t);
        if (ngames > 0) {
            float lp = launching ? ltime / 0.5f : 0;
            for (int i = c - 4; i < c; i++)
                if (i >= 0 && i < ngames) draw_item(&games[i], i - scroll, t, intro, 0);
            for (int i = c + 4; i > c; i--)
                if (i >= 0 && i < ngames) draw_item(&games[i], i - scroll, t, intro, 0);
            if (c >= 0 && c < ngames) draw_item(&games[c], c - scroll, t, intro, lp);
        }
        if (launching) {
            float a = smooth(ltime / 0.5f);
            rect_col(0, 0, SCR_W, SCR_H, col(255, 255, 255, a), col(255, 255, 255, a));
        }
        sceGuFinish();
        sceGuSync(0, 0);

        /* ---- text overlay (debug font on current draw buffer) ---- */
        pspDebugScreenSetOffset((int)draw_buf);
        pspDebugScreenSetTextColor(0xFFFFFFFF);
        if (ngames == 0) {
            draw_text_centered("No games found in ms0:/PSP/GAME", 15);
        } else if (!launching && fabsf(target - scroll) < 0.08f && intro > 0.95f) {
            draw_text_centered(games[target].title, 26);
            char cnt[32];
            snprintf(cnt, sizeof(cnt), "%d / %d", target + 1, ngames);
            pspDebugScreenSetTextColor(0xFFC8C8C8);
            draw_text_centered(cnt, 28);
        }
        if (!launching && intro > 0.95f) {
            pspDebugScreenSetTextColor(0xFF9A9A9A);
            draw_text_centered("< > Select    X Launch    L/R Jump 5    /\\ Exit", 33);
        }

        sceDisplayWaitVblankStart();
        sceGuSwapBuffers();
        draw_buf = (draw_buf == (void *)0) ? (void *)FRAME_SIZE : (void *)0;
    }
    return 0;
}
