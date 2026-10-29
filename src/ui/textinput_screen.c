/*
 * SwanStation - a text field typed with an on-screen keyboard drawn by the app (the system keyboard
 * module stops the title from launching, see platform/ps5_shims.c). Used by settings that take a name,
 * such as the games folder on USB drives. The keys are those of the sign-in screen's keyboard; the last
 * row is Space and Apply, and Apply hands the text back.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "../app.h"
#include "../i18n.h"
#include "../platform/platform.h"
#include "coverflow.h"
#include "draw.h"
#include "sfx.h"
#include "text.h"
#include "theme.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#define CENTER_X (plat_width() * 0.5f)
#define TEXT_MAX 511

/* letters and digits, or symbols; the last row is Space and Apply */
static const char *const PAGE_LETTERS[4] = {"1234567890", "qwertyuiop", "asdfghjkl_", "zxcvbnm.-/"};
static const char *const PAGE_SYMBOLS[4] = {"!@#$%^&*()", "-_=+[]{};:", "'\",.<>/?\\|", "~`"};
#define KEY_ROWS 4

static struct
{
    enum Screen back_to;
    char title[64], prompt[160];
    char text[TEXT_MAX + 1];
    TextInputDone done;
    int cur, view; /* where typing goes, and the first character shown (the field scrolls) */
    int row, col; /* row KEY_ROWS is Space (col 0) and Apply (col 1) */
    bool shift, symbols;
} T;

void textinput_open(enum Screen back_to, const char *title, const char *prompt, const char *initial,
                    TextInputDone done)
{
    T.back_to = back_to;
    str_copy(T.title, sizeof(T.title), title);
    str_copy(T.prompt, sizeof(T.prompt), prompt);
    str_copy(T.text, sizeof(T.text), initial ? initial : "");
    T.done = done;
    T.cur = (int)strlen(T.text);
    T.view = 0;
    T.row = 1;
    T.col = 0;
    T.shift = T.symbols = false;
    app.screen = SCREEN_TEXT_INPUT;
}

static int row_length(int row)
{
    return row == KEY_ROWS ? 2 : (int)strlen((T.symbols ? PAGE_SYMBOLS : PAGE_LETTERS)[row]);
}

static char key_at(int row, int col)
{
    char c = (T.symbols ? PAGE_SYMBOLS : PAGE_LETTERS)[row][col];
    return T.shift && !T.symbols ? (char)toupper((unsigned char)c) : c;
}

static void type(char c)
{
    size_t n = strlen(T.text);
    if (n < TEXT_MAX)
    {
        memmove(T.text + T.cur + 1, T.text + T.cur, n - (size_t)T.cur + 1);
        T.text[T.cur++] = c;
    }
}

static void erase(void)
{
    size_t n = strlen(T.text);
    if (T.cur > 0)
    {
        memmove(T.text + T.cur - 1, T.text + T.cur, n - (size_t)T.cur + 1);
        --T.cur;
    }
}

static float span_width(float size, int from, int to)
{
    char part[TEXT_MAX + 1];
    int n = to - from;
    memcpy(part, T.text + from, (size_t)n);
    part[n] = '\0';
    return text_width(size, FONT_BOLD, part);
}

static void apply(void)
{
    TextInputDone done = T.done;
    char text[TEXT_MAX + 1];
    str_copy(text, sizeof(text), T.text);
    app.screen = T.back_to;
    sfx_play(SFX_SELECT);
    if (done)
        done(text);
}

void textinput_screen(uint32_t pressed)
{
    int row = T.row, col = T.col;
    if (pressed & BIT(BTN_UP))
        T.row = (T.row + KEY_ROWS) % (KEY_ROWS + 1);
    if (pressed & BIT(BTN_DOWN))
        T.row = (T.row + 1) % (KEY_ROWS + 1);
    if (pressed & BIT(BTN_LEFT))
        --T.col;
    if (pressed & BIT(BTN_RIGHT))
        ++T.col;
    int len = row_length(T.row);
    if (T.col < 0)
        T.col = len - 1;
    if (T.col >= len)
        T.col = (pressed & BIT(BTN_RIGHT)) ? 0 : len - 1;
    if (T.row != row || T.col != col)
        sfx_play(SFX_CLICK);
    if ((pressed & BIT(BTN_L1)) && T.cur > 0)
    {
        --T.cur;
        sfx_play(SFX_CLICK);
    }
    if ((pressed & BIT(BTN_R1)) && T.text[T.cur])
    {
        ++T.cur;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_L2))
    {
        T.symbols = !T.symbols;
        if (T.col >= row_length(T.row))
            T.col = row_length(T.row) - 1;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_TRIANGLE))
    {
        T.shift = !T.shift;
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_SQUARE))
    {
        erase();
        sfx_play(SFX_CLICK);
    }
    if (pressed & BIT(BTN_START))
    {
        apply();
        return;
    }
    if (pressed & BIT(BTN_CROSS))
    {
        if (T.row < KEY_ROWS)
        {
            type(key_at(T.row, T.col));
            sfx_play(SFX_CLICK);
        }
        else if (T.col == 0)
        {
            type(' ');
            sfx_play(SFX_CLICK);
        }
        else
        {
            apply();
            return;
        }
    }
    if (pressed & BIT(BTN_CIRCLE))
    {
        sfx_play(SFX_BACK);
        app.screen = T.back_to;
        return;
    }

    /* ------------------------------------------------ drawing */
    if (app.game)
    {
        app_draw_game(40);
        draw_rect(0, 0, plat_width(), plat_height(), TH_SCRIM);
    }
    else
        shelf_backdrop();
    text_draw(CENTER_X, 44, 44, FONT_BOLD, TH_TEXT, ALIGN_CENTER, tr(T.title));
    text_draw_fit(CENTER_X, 104, 22, FONT_REGULAR, TH_TEXT_DIM, ALIGN_CENTER, plat_width() - 200, tr(T.prompt));

    const float fw = 760, fx = CENTER_X - fw * 0.5f;
    draw_rrect(fx, 160, fw, 84, TH_RADIUS_SMALL, TH_ROW_SELECTED);
    draw_rrect_outline(fx, 160, fw, 84, TH_RADIUS_SMALL, 3, TH_FOCUS);
    {
        const float size = 36, room = fw - 56;
        const int len = (int)strlen(T.text);
        if (T.view > T.cur)
            T.view = T.cur;
        while (T.view < T.cur && span_width(size, T.view, T.cur) > room)
            ++T.view;
        int end = T.cur;
        while (end < len && span_width(size, T.view, end + 1) <= room)
            ++end;
        char shown[TEXT_MAX + 1];
        memcpy(shown, T.text + T.view, (size_t)(end - T.view));
        shown[end - T.view] = '\0';
        text_draw(fx + 24, 184, size, FONT_BOLD, TH_TEXT, ALIGN_LEFT, shown);
        draw_rect(fx + 24 + span_width(size, T.view, T.cur), 182, 3, 44, TH_FOCUS);
    }

    const float key = 104, gap = 12, kh = 88;
    float ky = 270;
    for (int r = 0; r < KEY_ROWS; ++r)
    {
        int rl = row_length(r);
        float x = CENTER_X - (rl * (key + gap) - gap) * 0.5f;
        for (int c = 0; c < rl; ++c)
        {
            bool on = r == T.row && c == T.col;
            float kx = x + c * (key + gap);
            draw_rrect(kx, ky, key, kh, TH_RADIUS_SMALL, on ? TH_ROW_SELECTED : TH_CARD);
            if (on)
                draw_rrect_outline(kx, ky, key, kh, TH_RADIUS_SMALL, 3, TH_FOCUS);
            char label[2] = {key_at(r, c), '\0'};
            text_draw(kx + key * 0.5f, ky + 20, 38, FONT_BOLD, TH_TEXT, ALIGN_CENTER, label);
        }
        ky += kh + gap;
    }
    {
        const float sw = 5 * (key + gap) - gap, space_w = sw * 0.62f, go_w = sw - space_w - gap;
        const float sx = CENTER_X - sw * 0.5f;
        for (int c = 0; c < 2; ++c)
        {
            bool on = T.row == KEY_ROWS && T.col == c;
            float kx = c == 0 ? sx : sx + space_w + gap, kw = c == 0 ? space_w : go_w;
            draw_rrect(kx, ky, kw, kh, TH_RADIUS_SMALL, on ? TH_ROW_SELECTED : (c ? TH_PILL : TH_CARD));
            if (on)
                draw_rrect_outline(kx, ky, kw, kh, TH_RADIUS_SMALL, 3, TH_FOCUS);
            text_draw(kx + kw * 0.5f, ky + 24, 30, FONT_BOLD, c ? TH_FOCUS : TH_TEXT_DIM, ALIGN_CENTER,
                      c ? tr("Apply") : tr("Space"));
        }
    }

    static const int glyphs[] = {GLYPH_CROSS, GLYPH_SQUARE, GLYPH_TRIANGLE, GLYPH_START, GLYPH_CIRCLE};
    static const char *const labels[] = {"Type", "Delete", "Shift", "Apply", "Back"};
    app_draw_hints(glyphs, labels, 5, NULL);
    const HintCombo right[2] = {{{GLYPH_L1, GLYPH_R1}, 2, '/', "Cursor"}, {{GLYPH_L2}, 1, '+', "Symbols"}};
    app_draw_hints_right(right, 2);
    app_draw_toast();
}
