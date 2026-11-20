#include "fttext.h"

#include <assert.h>
#include <malloc.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int count_nonzero(const uint32_t *px, int w, int h)
{
    int n = 0;
    for (int i = 0; i < w * h; i++) {
        if (px[i] != 0) n++;
    }
    return n;
}

static int count_channel_dom(const uint32_t *px, int w, int h, int shift_hi, int shift_lo)
{
    int n = 0;
    for (int i = 0; i < w * h; i++) {
        uint32_t p = px[i];
        uint32_t a = (p >> 24) & 0xff;
        uint32_t hi = (p >> shift_hi) & 0xff;
        uint32_t lo = (p >> shift_lo) & 0xff;
        if (a > 20 && hi > lo + 40) n++;
    }
    return n;
}

static void test_ascii(void)
{
    struct fte_layout *l = fte_layout_create("DejaVu Sans", "Hello Tomoe 123", 16.0, 0.0, false);
    assert(l != NULL);
    struct fte_rect ink = { 0 };
    int lw = 0, lh = 0;
    fte_layout_extents(l, &ink, &lw, &lh);
    assert(lw > 40);
    assert(lh >= 14);
    assert(ink.x1 > ink.x0 && ink.y1 > ink.y0);

    int w = 256, h = 64;
    uint32_t *buf = calloc((size_t)(w * h), sizeof(uint32_t));
    assert(buf != NULL);
    struct fte_rect clip = { 0, 0, w, h };
    assert(fte_layout_draw(l, (double)lw, (double)lh, buf, w, w, h, clip, 4.0, 4.0, 0, 0xffffffff));
    assert(count_nonzero(buf, w, h) > 100);
    free(buf);
    fte_layout_destroy(l);
}

static void test_bidi_and_joining(void)
{
    const char *mix_he = "Inbox \xe2\x80\x94 \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d";
    const char *mix_ar = "Docs - \xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";
    const char *ar_word = "\xd9\x85\xd8\xb1\xd8\xad\xd8\xa8\xd8\xa7";
    const char *ar_rev = "\xd8\xa7\xd8\xa8\xd8\xad\xd8\xb1\xd9\x85";
    const char *ar_spaced = "\xd9\x85 \xd8\xb1 \xd8\xad \xd8\xa8 \xd8\xa7";

    struct fte_layout *lhe = fte_layout_create("DejaVu Sans", mix_he, 16.0, 0.0, false);
    struct fte_layout *lar = fte_layout_create("DejaVu Sans", mix_ar, 16.0, 0.0, false);
    struct fte_layout *law = fte_layout_create("DejaVu Sans", ar_word, 16.0, 0.0, false);
    struct fte_layout *lrv = fte_layout_create("DejaVu Sans", ar_rev, 16.0, 0.0, false);
    struct fte_layout *lsp = fte_layout_create("DejaVu Sans", ar_spaced, 16.0, 0.0, false);
    assert(lhe && lar && law && lrv && lsp);

    int w1 = 0, h1 = 0, w2 = 0, h2 = 0, ww = 0, hw = 0, wr = 0, hr = 0, ws = 0, hs = 0;
    fte_layout_extents(lhe, NULL, &w1, &h1);
    fte_layout_extents(lar, NULL, &w2, &h2);
    fte_layout_extents(law, NULL, &ww, &hw);
    fte_layout_extents(lrv, NULL, &wr, &hr);
    fte_layout_extents(lsp, NULL, &ws, &hs);
    assert(w1 > 40 && w2 > 40 && ww > 10 && wr > 10 && ws > ww);

    int cw = 256, ch = 64;
    struct fte_rect clip = { 0, 0, cw, ch };
    uint32_t *b1 = calloc((size_t)(cw * ch), sizeof(uint32_t));
    uint32_t *b2 = calloc((size_t)(cw * ch), sizeof(uint32_t));
    uint32_t *bw = calloc((size_t)(cw * ch), sizeof(uint32_t));
    uint32_t *br = calloc((size_t)(cw * ch), sizeof(uint32_t));
    assert(b1 && b2 && bw && br);
    assert(fte_layout_draw(lhe, (double)w1, (double)h1, b1, cw, cw, ch, clip, 4, 4, 0, 0xffffffff));
    assert(fte_layout_draw(lar, (double)w2, (double)h2, b2, cw, cw, ch, clip, 4, 4, 0, 0xffffffff));
    assert(fte_layout_draw(law, (double)ww, (double)hw, bw, cw, cw, ch, clip, 4, 4, 0, 0xffffffff));
    assert(fte_layout_draw(lrv, (double)wr, (double)hr, br, cw, cw, ch, clip, 4, 4, 0, 0xffffffff));
    assert(count_nonzero(b1, cw, ch) > 80);
    assert(count_nonzero(b2, cw, ch) > 60);
    assert(memcmp(bw, br, (size_t)(cw * ch) * sizeof(uint32_t)) != 0);

    free(b1); free(b2); free(bw); free(br);
    fte_layout_destroy(lhe);
    fte_layout_destroy(lar);
    fte_layout_destroy(law);
    fte_layout_destroy(lrv);
    fte_layout_destroy(lsp);
}

static void test_combining_marks(void)
{
    struct fte_layout *lbase = fte_layout_create("DejaVu Sans", "e", 18.0, 0.0, false);
    struct fte_layout *lcomb = fte_layout_create("DejaVu Sans", "e\xcc\x81", 18.0, 0.0, false);
    assert(lbase && lcomb);
    int wb = 0, hb = 0, wc = 0, hc = 0;
    fte_layout_extents(lbase, NULL, &wb, &hb);
    fte_layout_extents(lcomb, NULL, &wc, &hc);
    assert(wb > 0 && wc > 0);
    assert(abs(wc - wb) <= 4);
    fte_layout_destroy(lbase);
    fte_layout_destroy(lcomb);
}

static void test_markup_and_alpha(void)
{
    const char *m =
        "<span foreground=\"#ff0000\">R</span>"
        "<span background=\"#0000ff\" foreground=\"#ffffff\">B</span>"
        "<b>bold</b><i>ital</i><u>u</u><s>s</s><tt>tt</tt><big>G</big><small>m</small>"
        "&amp;&lt;&gt;&quot;&apos;&#x41;";
    struct fte_layout *l = fte_layout_create("DejaVu Sans", m, 16.0, 0.0, true);
    assert(l != NULL);
    int lw = 0, lh = 0;
    fte_layout_extents(l, NULL, &lw, &lh);
    assert(lw > 60 && lh >= 14);

    int cw = 512, ch = 64;
    struct fte_rect clip = { 0, 0, cw, ch };
    uint32_t *buf = calloc((size_t)(cw * ch), sizeof(uint32_t));
    assert(buf != NULL);
    assert(fte_layout_draw(l, (double)lw, (double)lh, buf, cw, cw, ch, clip, 4, 4, 0, 0xffffffff));
    assert(count_channel_dom(buf, cw, ch, 16, 0) > 10);
    assert(count_channel_dom(buf, cw, ch, 0, 16) > 20);
    free(buf);
    fte_layout_destroy(l);

    struct fte_layout *la1 = fte_layout_create("DejaVu Sans", "<span alpha=\"100%\">M</span>", 20.0, 0.0, true);
    struct fte_layout *la2 = fte_layout_create("DejaVu Sans", "<span alpha=\"40%\">M</span>", 20.0, 0.0, true);
    assert(la1 && la2);
    uint32_t *p1 = calloc(64 * 64, sizeof(uint32_t));
    uint32_t *p2 = calloc(64 * 64, sizeof(uint32_t));
    assert(p1 && p2);
    struct fte_rect c64 = { 0, 0, 64, 64 };
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    fte_layout_extents(la1, NULL, &w1, &h1);
    fte_layout_extents(la2, NULL, &w2, &h2);
    assert(fte_layout_draw(la1, (double)w1, (double)h1, p1, 64, 64, 64, c64, 4, 4, 0, 0xffffffff));
    assert(fte_layout_draw(la2, (double)w2, (double)h2, p2, 64, 64, 64, c64, 4, 4, 0, 0xffffffff));
    uint64_t s1 = 0, s2 = 0;
    for (int i = 0; i < 64 * 64; i++) {
        s1 += (p1[i] >> 24) & 0xff;
        s2 += (p2[i] >> 24) & 0xff;
    }
    assert(s1 > 0 && s2 > 0 && s2 < s1);
    free(p1); free(p2);
    fte_layout_destroy(la1);
    fte_layout_destroy(la2);
}

static void test_line_height(void)
{
    struct fte_layout *l0 = fte_layout_create("DejaVu Sans", "Height", 14.0, 0.0, false);
    struct fte_layout *l48 = fte_layout_create("DejaVu Sans", "Height", 14.0, 48.0, false);
    assert(l0 && l48);
    struct fte_rect i0 = { 0 }, i48 = { 0 };
    int w0 = 0, h0 = 0, w48 = 0, h48 = 0;
    fte_layout_extents(l0, &i0, &w0, &h0);
    fte_layout_extents(l48, &i48, &w48, &h48);
    assert(h48 == h0);
    assert(i48.y0 == i0.y0 && i48.y1 == i0.y1);
    fte_layout_destroy(l0);
    fte_layout_destroy(l48);
}

static void test_rotated_spans(void)
{
    const char *m = "<span background=\"#224488\"><u>Rot</u></span>";
    struct fte_layout *l = fte_layout_create("DejaVu Sans", m, 14.0, 0.0, true);
    assert(l != NULL);
    int lw = 0, lh = 0;
    fte_layout_extents(l, NULL, &lw, &lh);
    int rots[3] = { 90, 180, 270 };
    for (int r = 0; r < 3; r++) {
        uint32_t *buf = calloc(128 * 128, sizeof(uint32_t));
        assert(buf != NULL);
        struct fte_rect clip = { 0, 0, 128, 128 };
        assert(fte_layout_draw(l, (double)lw, (double)lh, buf, 128, 128, 128,
            clip, 20.0, 20.0, rots[r], 0xffffffff));
        assert(count_nonzero(buf, 128, 128) > 50);
        free(buf);
    }
    fte_layout_destroy(l);
}

static void test_lru_cache_stress(void)
{
    char markup[4096];
    size_t pos = 0;
    for (int i = 0; i < 80; i++) {
        int n = snprintf(markup + pos, sizeof(markup) - pos,
            "<span size=\"%dpt\">a</span>", 10 + i);
        assert(n > 0 && pos + (size_t)n < sizeof(markup));
        pos += (size_t)n;
    }
    struct fte_layout *l = fte_layout_create("DejaVu Sans", markup, 12.0, 0.0, true);
    assert(l != NULL);
    int lw = 0, lh = 0;
    fte_layout_extents(l, NULL, &lw, &lh);
    assert(lw > 200);
    fte_layout_destroy(l);
}

static int width_of(const char *font, const char *text, double size, bool markup)
{
    struct fte_layout *l = fte_layout_create(font, text, size, 0.0, markup);
    assert(l != NULL);
    int w = 0, h = 0;
    fte_layout_extents(l, NULL, &w, &h);
    fte_layout_destroy(l);
    return w;
}

static int height_of(const char *font, const char *text, double size, bool markup)
{
    struct fte_layout *l = fte_layout_create(font, text, size, 0.0, markup);
    assert(l != NULL);
    int w = 0, h = 0;
    fte_layout_extents(l, NULL, &w, &h);
    fte_layout_destroy(l);
    return h;
}

static void test_pango_metrics(void)
{
    assert(width_of("DejaVu Sans", "iiii", 13.0, false) == 4 * width_of("DejaVu Sans", "i", 13.0, false));
    int space = width_of("DejaVu Sans", " ", 14.0, false);
    assert(space > 0);
    assert(width_of("DejaVu Sans", "x\t", 14.0, false) == 8 * space);
    assert(width_of("DejaVu Sans", "x\ty", 14.0, false) > 8 * space);
    assert(width_of("DejaVu Sans", "\t", 14.0, false) == 8 * space);
    assert(width_of("DejaVu Sans", "x\t\t", 14.0, false) == 16 * space);
    char *tabs = malloc(60001);
    assert(tabs != NULL);
    memset(tabs, '\t', 60000);
    tabs[60000] = '\0';
    assert(width_of("DejaVu Sans", tabs, 14.0, false) > 0);
    free(tabs);
    assert(width_of("DejaVu Sans", "<span size='12pt'>x</span>", 14.0, true) ==
        width_of("DejaVu Sans", "<span size='16px'>x</span>", 14.0, true));
    assert(width_of("DejaVu Sans", "<span size='12288'>x</span>", 14.0, true) ==
        width_of("DejaVu Sans", "<span size='16px'>x</span>", 14.0, true));
    assert(width_of("DejaVu Sans", "<span font='Bold'>abc</span>", 14.0, true) ==
        width_of("DejaVu Sans", "<b>abc</b>", 14.0, true));
    assert(width_of("DejaVu Sans", "<span font_weight='700'>abc</span>", 14.0, true) ==
        width_of("DejaVu Sans", "<b>abc</b>", 14.0, true));
    assert(height_of("DejaVu Sans", "<span size='30pt'>A</span>a", 14.0, true) >
        height_of("DejaVu Sans", "a", 14.0, true));

    char deep[4096];
    size_t at = (size_t)snprintf(deep, sizeof(deep), "<span size='30px'>");
    for (int i = 0; i < 40; i++) at += (size_t)snprintf(deep + at, sizeof(deep) - at, "<b>");
    at += (size_t)snprintf(deep + at, sizeof(deep) - at, "x");
    for (int i = 0; i < 40; i++) at += (size_t)snprintf(deep + at, sizeof(deep) - at, "</b>");
    snprintf(deep + at, sizeof(deep) - at, "mmm</span>");
    assert(width_of("DejaVu Sans", deep, 14.0, true) ==
        width_of("DejaVu Sans", "<span size='30px'><b>x</b>mmm</span>", 14.0, true));
}

static void test_colours(void)
{
    const char *forms[] = {
        "<span foreground='#f00'>M</span>",
        "<span foreground='#ff0000'>M</span>",
        "<span foreground='#ffff00000000'>M</span>",
        "<span foreground='#ff0000ff'>M</span>",
    };
    uint32_t *ref = NULL;
    for (int f = 0; f < 4; f++) {
        struct fte_layout *l = fte_layout_create("DejaVu Sans", forms[f], 20.0, 0.0, true);
        assert(l != NULL);
        int w = 0, h = 0;
        fte_layout_extents(l, NULL, &w, &h);
        uint32_t *px = calloc(48 * 48, sizeof(uint32_t));
        assert(px != NULL);
        struct fte_rect clip = { 0, 0, 48, 48 };
        assert(fte_layout_draw(l, (double)w, (double)h, px, 48, 48, 48, clip, 2, 2, 0, 0x0000ffff));
        assert(count_channel_dom(px, 48, 48, 16, 0) > 10);
        if (!ref) ref = px;
        else {
            assert(memcmp(ref, px, 48 * 48 * sizeof(uint32_t)) == 0);
            free(px);
        }
        fte_layout_destroy(l);
    }
    free(ref);
}

static void test_emoji(void)
{
    int check = width_of("DejaVu Sans", "\xe2\x9c\x85", 14.0, false);
    assert(check >= 6 && check <= 32);
    int keycap = width_of("DejaVu Sans", "1\xef\xb8\x8f\xe2\x83\xa3", 14.0, false);
    assert(keycap == check);
    int family = width_of("DejaVu Sans",
        "\xf0\x9f\x91\xa8\xe2\x80\x8d\xf0\x9f\x91\xa9\xe2\x80\x8d\xf0\x9f\x91\xa7", 14.0, false);
    assert(family == check);
    assert(width_of("DejaVu Sans", "\xe2\x9c\x85 ", 14.0, false) ==
        check + width_of("DejaVu Sans", " ", 14.0, false));
    int mixed = width_of("DejaVu Sans", "build \xe2\x9c\x85 done", 14.0, false);
    assert(mixed >= 40 && mixed <= 180);
}

static uint32_t max_alpha(const uint32_t *px, int n)
{
    uint32_t m = 0;
    for (int i = 0; i < n; i++)
        if ((px[i] >> 24) > m) m = px[i] >> 24;
    return m;
}

static void test_alpha_replaces(void)
{
    struct fte_layout *l = fte_layout_create("DejaVu Sans", "<span alpha='50%'>M</span>", 24.0, 0.0, true);
    assert(l != NULL);
    int w = 0, h = 0;
    fte_layout_extents(l, NULL, &w, &h);
    uint32_t *px = calloc(64 * 64, sizeof(uint32_t));
    assert(px != NULL);
    struct fte_rect clip = { 0, 0, 64, 64 };
    assert(fte_layout_draw(l, (double)w, (double)h, px, 64, 64, 64, clip, 4, 4, 0, 0xffffff40));
    uint32_t a = max_alpha(px, 64 * 64);
    assert(a >= 120 && a <= 130);
    free(px);
    fte_layout_destroy(l);

    struct fte_layout *o = fte_layout_create("DejaVu Sans",
        "<span alpha='50%'>g\xcc\xb6H\xcc\xb6</span>", 32.0, 0.0, true);
    assert(o != NULL);
    fte_layout_extents(o, NULL, &w, &h);
    uint32_t *po = calloc(96 * 64, sizeof(uint32_t));
    assert(po != NULL);
    struct fte_rect co = { 0, 0, 96, 64 };
    assert(fte_layout_draw(o, (double)w, (double)h, po, 96, 96, 64, co, 4, 4, 0, 0xffffffff));
    uint32_t oa = max_alpha(po, 96 * 64);
    assert(oa >= 120 && oa <= 130);
    free(po);
    fte_layout_destroy(o);
}

static void test_bounds(void)
{
    struct fte_layout *l = fte_layout_create("DejaVu Sans", "<span background='#ff0000'>Bounds</span>", 30.0, 0.0, true);
    assert(l != NULL);
    int w = 0, h = 0;
    fte_layout_extents(l, NULL, &w, &h);
    uint32_t *px = calloc(32 * 32, sizeof(uint32_t));
    assert(px != NULL);
    int rots[4] = { 0, 90, 180, 270 };
    for (int r = 0; r < 4; r++) {
        struct fte_rect clip = { -50, -50, 500, 500 };
        assert(fte_layout_draw(l, (double)w, (double)h, px, 32, 32, 32, clip, -10.5, -7.25, rots[r], 0xffffffff));
        assert(fte_layout_draw(l, (double)w, (double)h, px, 32, 32, 32, clip, 1e9, -1e9, rots[r], 0xffffffff));
    }
    free(px);
    fte_layout_destroy(l);

    size_t n = 60000;
    char *amps = malloc(n + 2);
    assert(amps != NULL);
    memset(amps, '&', n);
    amps[n] = 'x';
    amps[n + 1] = '\0';
    struct fte_layout *la = fte_layout_create("DejaVu Sans", amps, 12.0, 0.0, true);
    assert(la != NULL);
    fte_layout_destroy(la);
    free(amps);

    const char *huge[] = {
        "<span size='1e300pt'>x</span>",
        "<span font='Sans 1e300'>x</span>",
        "<span size='1e300%'><span size='1e300%'>x</span></span>",
        "<big><big><big><big><big><big><big><big><big><big>x</big></big></big></big></big></big></big></big></big></big>",
        "<span size='1e-300pt'>x</span>",
    };
    for (size_t i = 0; i < sizeof(huge) / sizeof(huge[0]); i++) {
        struct fte_layout *lh = fte_layout_create("DejaVu Sans", huge[i], 16000.0, 0.0, true);
        assert(lh != NULL);
        fte_layout_destroy(lh);
    }

    char many[4096];
    size_t at = 0;
    for (uint32_t cp = 0x21; cp < 0x7f && at + 8 < sizeof(many); cp++) many[at++] = (char)cp;
    for (uint32_t cp = 0xc0; cp < 0x180 && at + 8 < sizeof(many); cp++) {
        many[at++] = (char)(0xc0 | (cp >> 6));
        many[at++] = (char)(0x80 | (cp & 0x3f));
    }
    many[at] = '\0';
    struct fte_layout *lm = fte_layout_create("DejaVu Sans", many, 11.37, 0.0, false);
    assert(lm != NULL);
    int mw = 0, mh = 0;
    fte_layout_extents(lm, NULL, &mw, &mh);
    uint32_t *pm = calloc((size_t)(mw + 16) * 64, sizeof(uint32_t));
    assert(pm != NULL);
    struct fte_rect cm = { 0, 0, mw + 16, 64 };
    for (int r = 0; r < 4; r++)
        assert(fte_layout_draw(lm, (double)mw, (double)mh, pm, mw + 16, mw + 16, 64, cm,
            2.0 + r * 0.25, 2.5, 0, 0xffffff80));
    free(pm);
    fte_layout_destroy(lm);
}

static void test_markup_size_bounds(void)
{
    const char *cases[] = {
        "<span size='1e999px'>x</span>",
        "<span size='200%'><big><big><big><big><big><big><big><big><big><big>x</big></big></big></big></big></big></big></big></big></big></span>",
        "<span size='999999999'>x</span>",
        "<span size='-5px'>x</span>",
        "<span size='0'>x</span>",
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        struct fte_layout *l = fte_layout_create("DejaVu Sans", cases[i], 12.0, 0.0, true);
        assert(l != NULL);
        int w = 0, h = 0;
        struct fte_rect ink = { 0 };
        fte_layout_extents(l, &ink, &w, &h);
        assert(w >= 0 && w < 100000);
        assert(h >= 0 && h < 100000);
        uint32_t px[64];
        struct fte_rect clip = { 0, 0, 8, 8 };
        fte_layout_draw(l, (double)w, (double)h, px, 8, 8, 8, clip, 0, 0, 0, 0xffffffffu);
        fte_layout_destroy(l);
    }
}

static void test_steady_state(void)
{
    static const char *const cycle[] = {
        "Hello Tomoe",
        "<b>bold</b> <span foreground='#ff4080' background='#10203080'>x &amp; y</span>",
        "build \xe2\x9c\x85 done 1\xef\xb8\x8f\xe2\x83\xa3",
        "\xd8\xb3\xd9\x84\xd8\xa7\xd9\x85 abc \xd7\xa9\xd7\x9c\xd7\x95\xd7\x9d",
        "tab\tstop",
    };
    uint32_t *px = calloc(512 * 128, sizeof(uint32_t));
    assert(px != NULL);
    struct fte_rect clip = { 0, 0, 512, 128 };
    size_t before = 0;
    for (int round = 0; round < 400; round++) {
        if (round == 100) before = mallinfo2().uordblks;
        for (size_t k = 0; k < sizeof(cycle) / sizeof(cycle[0]); k++) {
            struct fte_layout *l = fte_layout_create("DejaVu Sans", cycle[k], 14.0, 20.0, k == 1);
            assert(l != NULL);
            int w = 0, h = 0;
            fte_layout_extents(l, NULL, &w, &h);
            assert(fte_layout_draw(l, (double)w, (double)h, px, 512, 512, 128, clip, 4.5, 4.25,
                (round % 4) * 90, 0xffffffff));
            fte_layout_destroy(l);
        }
    }
    assert(mallinfo2().uordblks < before + 65536);
    free(px);
}

int main(void)
{
    test_ascii();
    test_bidi_and_joining();
    test_combining_marks();
    test_markup_and_alpha();
    test_line_height();
    test_markup_size_bounds();
    test_rotated_spans();
    test_lru_cache_stress();
    test_pango_metrics();
    test_colours();
    test_emoji();
    test_alpha_replaces();
    test_bounds();
    test_steady_state();
    puts("fttext-test: ok");
    return 0;
}
