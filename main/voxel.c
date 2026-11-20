/* voxel.c — voir voxel.h */
#include "voxel.h"
#include <math.h>
#include <string.h>
#include "esp_heap_caps.h"
#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

#define N    VOX_N
#define MASK (VOX_N - 1)
#define PI   3.14159265f
#define CLOUD_N 128

static uint8_t *hmap;                /* hauteurs (sous VOX_SEA : eau) */
static uint16_t *cmap;               /* couleurs éclairées (génération) */
/* carte de rendu : couleur (bits 0–15) et hauteur (16–23) dans le même mot,
 * une seule ligne de cache PSRAM lue par échantillon */
static uint32_t *vmap;
static uint8_t *cloud;               /* densité des nuages 128×128 */
static float *lat;                   /* treillis du bruit 64×64 */
static volatile int s_prog = -1;     /* -1 : rien ; 0..100 */
static volatile bool s_abort, s_task;
static uint32_t s_seed;

/* ------------------------------------------------------------------ bruit */
static uint32_t hash32(uint32_t x)
{
    x ^= x >> 16; x *= 0x7feb352d; x ^= x >> 15; x *= 0x846ca68b; x ^= x >> 16;
    return x;
}

static float vnoise(float x, float y, int per)       /* bruit de valeur périodique */
{
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    fx = fx * fx * (3 - 2 * fx); fy = fy * fy * (3 - 2 * fy);
    int x0 = ((xi % per) + per) % per, y0 = ((yi % per) + per) % per;
    int x1 = (x0 + 1) % per, y1 = (y0 + 1) % per;
    float a = lat[y0 * 64 + x0], b = lat[y0 * 64 + x1], c = lat[y1 * 64 + x0], d = lat[y1 * 64 + x1];
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

/* somme d'octaves, période de base `per` cellules sur toute la carte */
static float fbm(float x, float y, int per, int oct, float gain, float shift)
{
    float h = 0, amp = 1, tot = 0;
    for (int o = 0; o < oct && per <= 64; o++, per *= 2) {
        h += vnoise((x + shift) * per / N, (y + shift * 0.7f) * per / N, per) * amp;
        tot += amp;
        amp *= gain;
    }
    return h / tot;
}

static float smooth(float e0, float e1, float x)
{
    float t = (x - e0) / (e1 - e0);
    t = t < 0 ? 0 : t > 1 ? 1 : t;
    return t * t * (3 - 2 * t);
}

/* ------------------------------------------------------------------ génération */
/* prés : cellules de Voronoï sur une grille de 16, frontières = haies */
#define FC 16
static void field_at(int x, int y, uint32_t *id, float *edge, int *cx, int *cy)
{
    int gx = x / FC, gy = y / FC;
    float d1 = 1e9f, d2 = 1e9f;
    for (int j = -1; j <= 1; j++)
        for (int i = -1; i <= 1; i++) {
            int cxg = gx + i, cyg = gy + j;
            uint32_t h = hash32((uint32_t)(cxg & (N / FC - 1)) * 73856093u ^ (uint32_t)(cyg & (N / FC - 1)) * 19349663u ^ s_seed);
            float px = cxg * FC + 2 + (h & 0xFF) / 255.0f * (FC - 4);
            float py = cyg * FC + 2 + ((h >> 8) & 0xFF) / 255.0f * (FC - 4);
            float dx = px - x, dy = py - y, d = dx * dx + dy * dy;
            if (d < d1) { d2 = d1; d1 = d; *id = h; *cx = (int)px; *cy = (int)py; }
            else if (d < d2) d2 = d;
        }
    *edge = sqrtf(d2) - sqrtf(d1);
}

static void gen_height(void)
{
    for (int y = 0; y < N && !s_abort; y++) {
        for (int x = 0; x < N; x++) {
            float h = fbm(x, y, 4, 5, 0.48f, 0);
            float m = fbm(x, y, 2, 3, 0.5f, 900);                 /* massifs */
            float v = (h - 0.36f) / 0.42f;
            v = v < 0 ? 0 : v;
            float hh = 25 + powf(v, 1.4f) * (70 + 120 * smooth(0.45f, 0.7f, m));
            if (h < 0.33f) hh = 25 - (0.33f - h) * 150;            /* lacs */
            /* rivières : ligne de niveau d'un second bruit */
            float r = fabsf(fbm(x, y, 4, 3, 0.5f, 333) - 0.5f);
            float bed = smooth(0.012f, 0.035f, r);
            if (hh < 120) hh = (VOX_SEA - 4) + (hh - (VOX_SEA - 4)) * (0.15f + 0.85f * bed) * (bed < 1 ? 1 : 1);
            if (bed < 0.3f) hh = fminf(hh, VOX_SEA - 2);
            hmap[y * N + x] = (uint8_t)(hh < 0 ? 0 : hh > 250 ? 250 : hh);
        }
        s_prog = y * 30 / N;
    }
}

typedef struct { float r, g, b; } col_t;

static void gen_color(void)
{
    const float sx = cosf(VOX_SUN_A), sy = sinf(VOX_SUN_A), rise = 0.32f;   /* soleil bas */
    const float lz = 0.55f, ll = sqrtf(1 - lz * lz);
    for (int y = 0; y < N && !s_abort; y++) {
        for (int x = 0; x < N; x++) {
            int i = y * N + x;
            int h = hmap[i];
            float slope = fabsf((float)hmap[y * N + ((x + 1) & MASK)] - hmap[y * N + ((x - 1) & MASK)])
                        + fabsf((float)hmap[((y + 1) & MASK) * N + x] - hmap[((y - 1) & MASK) * N + x]);
            uint32_t rn = hash32((uint32_t)i * 2654435761u ^ s_seed);
            float n = ((rn & 0xFF) / 255.0f - 0.5f);
            float forest = fbm(x, y, 8, 3, 0.55f, 555);
            col_t c;
            int bump = 0;
            if (h < VOX_SEA) {
                float d = (VOX_SEA - h) / 14.0f;
                d = d > 1 ? 1 : d;
                c = (col_t){ 40 - 25 * d, 95 - 45 * d, 110 - 30 * d };          /* eau */
            } else if (h < VOX_SEA + 3 && slope < 6) {
                c = (col_t){ 205 + 20 * n, 188 + 20 * n, 140 + 15 * n };        /* plage */
            } else if (h > 150 - slope * 2 || (h > 120 && slope < 4)) {
                c = (col_t){ 238, 238, 245 };                                    /* neige */
            } else if (slope > 9 || h > 105) {
                float k = 0.85f + 0.3f * n;
                c = (col_t){ 120 * k, 112 * k, 100 * k };                        /* roche */
            } else if (forest > 0.56f && h < 95) {
                float k = 0.75f + 0.5f * (rn >> 8 & 0xFF) / 255.0f;
                c = (col_t){ 30 * k, 78 * k, 34 * k };                           /* forêt */
                bump = 2 + (int)(rn >> 16 & 3);
            } else if (h < 70 && slope < 7) {
                uint32_t id = 0; float edge = 9; int cx = 0, cy = 0;
                field_at(x, y, &id, &edge, &cx, &cy);
                static const col_t FIELD[] = {
                    { 78, 150, 52 }, { 98, 165, 60 }, { 120, 170, 64 }, { 64, 132, 48 },
                    { 170, 165, 80 }, { 140, 110, 70 }, { 105, 158, 70 }, { 88, 140, 44 },
                };
                c = FIELD[id % 8];
                if (id % 8 == 5 && ((x + y) & 3) == 0) { c.r *= 0.8f; c.g *= 0.8f; c.b *= 0.8f; }   /* labours */
                if (id % 8 == 4 && ((x - y) & 3) == 0) { c.r *= 0.9f; c.g *= 0.9f; }                /* foin */
                c.r += 10 * n; c.g += 10 * n;
                if (edge < 1.3f) { c = (col_t){ 40, 85, 35 }; bump = 2; }      /* haie */
                if (id % 11 == 0 && abs(x - cx) <= 1 && abs(y - cy) <= 1) {     /* fermette */
                    bool roof = (x - cx) == -1;
                    c = roof ? (col_t){ 90, 80, 85 } : (col_t){ 245, 240, 228 };
                    bump = 4;
                }
            } else {
                float k = 1 - (h - 70) / 120.0f;                                  /* landes */
                c = (col_t){ 95 + 30 * (1 - k) + 12 * n, 125 * k + 30 + 10 * n, 55 + 10 * n };
            }
            /* lumière : normale, soleil couchant, ombre portée */
            float nx = ((float)hmap[y * N + ((x - 1) & MASK)] - hmap[y * N + ((x + 1) & MASK)]);
            float ny = ((float)hmap[((y - 1) & MASK) * N + x] - hmap[((y + 1) & MASK) * N + x]);
            float nz = 4.0f, nl = sqrtf(nx * nx + ny * ny + nz * nz);
            float dif = (nx * sx * ll + ny * sy * ll + nz * lz) / nl;
            if (h < VOX_SEA) dif = 0.8f;
            dif = dif < 0 ? 0 : dif;
            float ray = h + bump + 1.0f;
            bool shadow = false;
            for (int k = 1; k < 48 && ray < 255; k++) {
                ray += rise;
                int qx = (int)(x + sx * k) & MASK, qy = (int)(y + sy * k) & MASK;
                if (hmap[qy * N + qx] > ray) { shadow = true; break; }
            }
            float lit = shadow ? 0.12f : dif;
            float amb = 0.42f;
            float r = c.r * (amb * 0.85f + lit * 1.05f);                          /* soleil doré, ombres bleutées */
            float g = c.g * (amb * 0.9f + lit * 0.88f);
            float b = c.b * (amb * 1.15f + lit * 0.62f);
            cmap[i] = g_rgb((int)r, (int)g, (int)b);
            if (bump && h >= VOX_SEA) hmap[i] = (uint8_t)(h + bump > 250 ? 250 : h + bump);
        }
        s_prog = 30 + y * 65 / N;
    }
}

static void gen_clouds(void)
{
    for (int y = 0; y < CLOUD_N; y++)
        for (int x = 0; x < CLOUD_N; x++) {
            float c = fbm(x * (float)N / CLOUD_N, y * (float)N / CLOUD_N, 4, 4, 0.55f, 777);
            float d = smooth(0.48f, 0.72f, c);
            cloud[y * CLOUD_N + x] = (uint8_t)(d * 255);
        }
}

static void generate(void)
{
    g_seed(s_seed);
    for (int i = 0; i < 64 * 64; i++) lat[i] = (hash32((uint32_t)i ^ s_seed) & 0xFFFF) / 65535.0f;
    gen_height();
    if (!s_abort) gen_color();
    if (!s_abort) gen_clouds();
    if (!s_abort) {
        for (int i = 0; i < N * N; i++) vmap[i] = cmap[i] | (uint32_t)hmap[i] << 16;
#ifdef ESP_PLATFORM
        heap_caps_free(hmap); heap_caps_free(cmap);       /* 768 Ko rendus */
        hmap = NULL; cmap = NULL;
#endif
        s_prog = 100;
    }
}

#ifdef ESP_PLATFORM
static void gen_task(void *arg)
{
    generate();
    s_task = false;
    vTaskDelete(NULL);
}
#endif

bool vox_gen_start(uint32_t seed)
{
    if (vmap) return true;
    hmap = heap_caps_malloc(N * N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cmap = heap_caps_malloc(N * N * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    vmap = heap_caps_malloc(N * N * 4, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cloud = heap_caps_malloc(CLOUD_N * CLOUD_N, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    lat = heap_caps_malloc(64 * 64 * sizeof(float), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!hmap || !cmap || !vmap || !cloud || !lat) { vox_free(); return false; }
    s_seed = seed;
    s_abort = false;
    s_prog = 0;
#ifdef ESP_PLATFORM
    s_task = true;
    if (xTaskCreatePinnedToCore(gen_task, "voxgen", 4096, NULL, 2, NULL, 0) != pdPASS) {
        s_task = false;
        generate();
    }
#else
    generate();
#endif
    return true;
}

int vox_progress(void) { return s_prog < 0 ? 0 : s_prog; }

void vox_free(void)
{
    s_abort = true;
#ifdef ESP_PLATFORM
    for (int i = 0; i < 300 && s_task; i++) vTaskDelay(pdMS_TO_TICKS(10));
#endif
    heap_caps_free(hmap); heap_caps_free(cmap); heap_caps_free(vmap); heap_caps_free(cloud); heap_caps_free(lat);
    hmap = NULL; cmap = NULL; vmap = NULL; cloud = NULL; lat = NULL;
    s_prog = -1;
}

static inline int hof(uint32_t v) { int h = (int)(v >> 16); return h < VOX_SEA ? VOX_SEA : h; }
static inline float hat(int x, int y) { return hof(vmap[(y & MASK) * N + (x & MASK)]); }

float vox_ground(float x, float y)
{
    if (s_prog != 100) return 0;
    int xi = (int)floorf(x), yi = (int)floorf(y);
    float fx = x - xi, fy = y - yi;
    float a = hat(xi, yi), b = hat(xi + 1, yi), c = hat(xi, yi + 1), d = hat(xi + 1, yi + 1);
    return a + (b - a) * fx + (c - a) * fy + (a - b - c + d) * fx * fy;
}

void vox_find_valley(float *x, float *y, float *dir)
{
    float best = 1e9f;
    *x = *y = N / 2;
    *dir = 0;
    if (s_prog != 100) return;
    for (int gy = 0; gy < N; gy += 32)
        for (int gx = 0; gx < N; gx += 32) {
            float sum = 0;
            for (int k = 0; k < 16; k++) {
                float a = k * 0.3927f, r = 20 + (k & 3) * 15;
                sum += hat(gx + (int)(cosf(a) * r), gy + (int)(sinf(a) * r));
            }
            if (sum < best) { best = sum; *x = gx; *y = gy; }
        }
    /* cap : vers la direction la plus dégagée */
    float lo = 1e9f;
    for (int k = 0; k < 16; k++) {
        float a = k * 0.3927f, s = 0;
        for (int d = 20; d <= 120; d += 20) s += hat((int)(*x + cosf(a) * d), (int)(*y + sinf(a) * d));
        if (s < lo) { lo = s; *dir = a; }
    }
}

/* ------------------------------------------------------------------ rendu */
#define ZMAX 340.0f

static float s_sin[256];
static inline float fsin(float x)                    /* sinus approché par table */
{
    return s_sin[(int)(x * (256 / 6.2831853f)) & 255];
}

static const float OCC_Z[VOX_OCC] = { 6, 10, 15, 22, 30, 40, 52, 66, 82, 100, 125, 155, 190, 230, 280, 1000 };

void vox_render(gsurf_t *s, uint8_t *occ, const vox_cam_t *c, float fy, float t)
{
    const int W = s->w, H = s->h;
    if (s_sin[64] == 0) for (int i = 0; i < 256; i++) s_sin[i] = sinf(i * 6.2831853f / 256);
    const float half = W * 0.5f;
    const float hz0 = H * 0.33f + c->pitch;
    const float ca = cosf(c->a), sa = sinf(c->a);
    /* ciel : dégradé selon la hauteur au-dessus de l'horizon */
    uint16_t sky[128];
    const uint16_t top = g_rgb(34, 30, 92), mid = g_rgb(200, 110, 120), low = g_rgb(255, 170, 105);
    for (int i = 0; i < 128; i++) {
        int k = i * 256 / 90;
        sky[i] = i < 18 ? g_blend(low, mid, i * 256 / 18) : g_blend(mid, top, k > 256 ? 256 : k);
    }
    const uint16_t fog = g_rgb(250, 168, 118);
    uint16_t fogk[64];                                    /* brume par distance, en table */
    for (int i = 0; i < 64; i++) fogk[i] = (uint16_t)(i * i * 256 / (63 * 63));
    float sd = remainderf(VOX_SUN_A - c->a, 2 * PI);
    float sun_col = fabsf(sd) < 1.2f ? half + tanf(sd) * half : -10000;
    const float sun_up = H * 0.12f;
    float inv_up[128];
    for (int i = 0; i < 128; i++) inv_up[i] = i > 1 ? 1.0f / i : 0;
    const float cloud_h = 260.0f * fy / 70.0f;
    const float roll = c->roll * 120.0f / W;              /* même inclinaison quelle que soit la largeur */
    uint16_t colbuf[128];                                 /* la colonne, en RAM interne */

    for (int col = 0; col < W; col++) {
        float rx = (col - half) / half;
        float dx = ca - sa * rx, dy = sa + ca * rx;
        float horizon = hz0 + (col - half) * roll;
        int ybuf = H, ob = 0;
        float z = 1.0f, dz = 0.5f;
        while (z < ZMAX && ybuf > 0) {
            while (z >= OCC_Z[ob]) { if (occ) occ[col * VOX_OCC + ob] = (uint8_t)ybuf; ob++; }
            float px = c->x + dx * z, py = c->y + dy * z;
            int ix = (int)floorf(px), iy = (int)floorf(py);
            uint32_t v0 = vmap[(iy & MASK) * N + (ix & MASK)];
            int hraw = (int)(v0 >> 16);
            bool water = hraw < VOX_SEA;
            float hh;
            uint32_t v1 = 0, v2 = 0, v3 = 0;
            bool near = z < 36 && !water;
            float fx = 0, fy2 = 0;
            if (near) {                                         /* près : hauteur et couleur lissées */
                fx = px - ix; fy2 = py - iy;
                v1 = vmap[(iy & MASK) * N + ((ix + 1) & MASK)];
                v2 = vmap[((iy + 1) & MASK) * N + (ix & MASK)];
                v3 = vmap[((iy + 1) & MASK) * N + ((ix + 1) & MASK)];
                float a = hof(v0), b = hof(v1), cc = hof(v2), d = hof(v3);
                hh = a + (b - a) * fx + (cc - a) * fy2 + (a - b - cc + d) * fx * fy2;
            } else hh = water ? VOX_SEA : hraw;
            int sy = (int)((c->h - hh) / z * fy + horizon);
            if (sy < ybuf) {
                uint16_t col565 = (uint16_t)v0;
                if (near) {
                    uint16_t tp = g_blend(col565, (uint16_t)v1, (int)(fx * 256)), bt = g_blend((uint16_t)v2, (uint16_t)v3, (int)(fx * 256));
                    col565 = g_blend(tp, bt, (int)(fy2 * 256));
                }
                if (water) {                                            /* reflet du ciel, rides */
                    float rip = fsin(px * 0.35f + t * 1.7f) + fsin(py * 0.29f - t * 1.3f);
                    int up = (int)(horizon - sy) + (int)(rip * 2);
                    uint16_t refl = sky[up < 0 ? 0 : up > 127 ? 127 : up];
                    col565 = g_blend(col565, refl, 120 + (int)(rip * 30));
                    if (sun_col > -1000 && fabsf(col - sun_col) < 3 + z * 0.02f && rip > 0.6f) col565 = g_white(col565, 160);
                }
                col565 = g_blend(col565, fog, fogk[(int)(z * 63 / ZMAX)]);
                int y0 = sy < 0 ? 0 : sy;
                for (int y = y0; y < ybuf; y++) colbuf[y] = col565;
                ybuf = y0;
            }
            z += dz;
            dz *= 1.022f;
        }
        if (occ) for (; ob < VOX_OCC; ob++) occ[col * VOX_OCC + ob] = (uint8_t)ybuf;
        /* ciel de la colonne */
        float dsx = col - sun_col;
        for (int y = 0; y < ybuf; y++) {
            int up = (int)(horizon - y);
            uint16_t cc = sky[up < 0 ? 0 : up > 127 ? 127 : up];
            if (up > 2 && up < 128) {
                float dist = cloud_h * inv_up[up];
                int u = (int)((c->x + dx * dist) * 0.25f + t * 2.0f) & (CLOUD_N - 1);
                int v = (int)((c->y + dy * dist) * 0.25f) & (CLOUD_N - 1);
                int d = cloud[v * CLOUD_N + u];
                if (d) {
                    int fade = up < 24 ? up * 256 / 24 : 256;
                    uint16_t cl = g_blend(g_rgb(255, 205, 190), g_rgb(150, 110, 150), up * 2 > 256 ? 256 : up * 2);
                    cc = g_blend(cc, cl, d * fade >> 8);
                }
            }
            if (sun_col > -1000) {
                float dyy = up - sun_up, d2 = dsx * dsx + dyy * dyy;
                if (d2 < 22) cc = g_rgb(255, 246, 210);
                else if (d2 < 1600) cc = g_white(cc, (int)((1600 - d2) * 110 / 1600));
            }
            colbuf[y] = cc;
        }
        uint16_t *dst = s->px + col;
        for (int y = 0; y < H; y++, dst += W) *dst = colbuf[y];
    }
}

bool vox_visible(const uint8_t *occ, int w, int x, int y, float z)
{
    if ((unsigned)x >= (unsigned)w) return false;
    int b = 0;
    while (b < VOX_OCC - 1 && OCC_Z[b] <= z) b++;
    /* relief plus proche que z : échantillons avant le seuil b-1 */
    int k = b > 0 ? b - 1 : 0;
    return y < occ[x * VOX_OCC + k];
}

bool vox_project(const gsurf_t *s, const vox_cam_t *c, float fy, float wx, float wy, float wh,
                 float *sx, float *sy, float *z)
{
    float ca = cosf(c->a), sa = sinf(c->a);
    float dx = wx - c->x, dy = wy - c->y;
    /* le monde se répète : on prend l'image la plus proche */
    dx -= N * roundf(dx / N);
    dy -= N * roundf(dy / N);
    float zz = dx * ca + dy * sa, l = -dx * sa + dy * ca;
    if (zz < 1) return false;
    float half = s->w * 0.5f;
    *sx = half + l / zz * half;
    *sy = s->h * 0.33f + c->pitch + (*sx - half) * c->roll * 120.0f / s->w + (c->h - wh) / zz * fy;
    *z = zz;
    return true;
}

#ifndef ESP_PLATFORM
uint16_t *vox_dbg_cmap(void) { return cmap; }   /* outils PC (avant libération) */
#endif
