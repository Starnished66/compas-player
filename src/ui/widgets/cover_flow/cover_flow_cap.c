/* The console cap is rasterized once; moving it is an ordinary image blit. */
#include "cover_flow_cap.h"
#include <stdbool.h>

static bool inside(float x, float y, float inset) {
    float l = 4 + inset, r = 50 - inset, t = 3 + inset, b = 31 - inset;
    float radius = 5 - inset;
    if (x < l || x >= r || y < t || y >= b) return false;
    float dx = x < l + radius ? l + radius - x : x > r - radius ? x - r + radius : 0;
    float dy = y < t + radius ? t + radius - y : y > b - radius ? y - b + radius : 0;
    return dx * dx + dy * dy <= radius * radius;
}

static void gradient(float u, const int * a, const int * b, int * rgb) {
    for (int i = 0; i < 3; i++) rgb[i] = (int) (a[i] + (b[i] - a[i]) * u);
}

static void body(float x, float y, int * rgb) {
    static const int top[] = {70, 73, 80}, mid[] = {38, 40, 45}, bottom[] = {24, 25, 29};
    static const int inner_top[] = {92, 95, 103}, inner_bottom[] = {44, 46, 52};
    float u = (y - 3) / 27;
    gradient(u < .5f ? u * 2 : u * 2 - 1, u < .5f ? top : mid, u < .5f ? mid : bottom, rgb);
    if (x >= 7 && x < 47 && y >= 6 && y < 28) gradient((y - 6) / 21, inner_top, inner_bottom, rgb);
    float dx = x - 27;
    if (y >= 8 && y < 26) {
        static const int ridges[] = {-17, -13, -9, -5, 6, 10, 14, 18};
        for (int j = 0; j < 8; j++) {
            int k = ridges[j];
            if (dx >= k && dx < k + 1) { rgb[0] = 30; rgb[1] = 31; rgb[2] = 35; }
            if (dx >= k + 1 && dx < k + 1.5f) { rgb[0] = 100; rgb[1] = 103; rgb[2] = 111; }
        }
    }
    if (dx >= -1 && dx < 1 && y >= 6 && y < 28) { rgb[0] = rgb[1] = 240; rgb[2] = 242; }
    if (!inside(x, y, 1)) { rgb[0] = rgb[1] = 12; rgb[2] = 14; }
    else if (!inside(x, y, 2))
        for (int i = 0; i < 3; i++) rgb[i] = (rgb[i] * 227 + 255 * 28) / 255;
}

/* A small separable blur kernel, offset down three pixels. */
static int shadow(float x, float y) {
    static const int weight[] = {1, 2, 3, 4, 3, 2, 1};
    int alpha = 0;
    for (int j = -3; j <= 3; j++)
        for (int i = -3; i <= 3; i++)
            if (inside(x - i, y - j - 3, 0)) alpha += weight[i + 3] * weight[j + 3];
    return alpha * 170 / 256;
}

void cover_flow_cap_render(uint8_t * pixels, int width, int height, int stride) {
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) {
        int sum[4] = {0};
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float px = (x + (sx + .5f) / 4) * COVER_FLOW_CAP_WIDTH / width;
            float py = (y + (sy + .5f) / 4) * COVER_FLOW_CAP_HEIGHT / height;
            int rgb[3] = {0}, alpha = 255;
            if (inside(px, py, 0)) body(px, py, rgb);
            else alpha = shadow(px, py);
            for (int i = 0; i < 3; i++) sum[i] += rgb[i] * alpha;
            sum[3] += alpha;
        }
        uint8_t * p = pixels + y * stride + x * 4;
        for (int i = 0; i < 3; i++) p[2 - i] = sum[3] ? (uint8_t) (sum[i] / sum[3]) : 0;
        p[3] = (uint8_t) (sum[3] / 16);
    }
}
