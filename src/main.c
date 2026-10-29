/*
 * SwanStation - PlayStation X Super 5
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Entry point, game start/stop and the emulation screen. The other screens
 * live in src/ui (shelf, settings, in-game menu and cheats).
 * Emulation is SwanStation, linked statically and driven through libretro.
 */
#include "drives.h"
#include "app.h"

#include "config.h"
#include "core/host.h"
#include "core/swanstation_options.h"
#include "bezels.h"
#include "controls.h"
#include "gamedb.h"
#include "profiles.h"
#include "covers.h"
#include "i18n.h"
#include "platform/platform.h"
#include "platform/crash.h"
#include "play.h"
#include "remote.h"
#include "ra/achievements.h"
#include "stats.h"
#include "ui/coverflow.h"
#include "ui/draw.h"
#include "ui/icons.h"
#include "ui/sfx.h"
#include "ui/text.h"
#include "ui/theme.h"

#include <dirent.h>
#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>
#include <math.h>
#include <SDL2/SDL.h>
#include <string.h>
#include <time.h>

#define UI_RATE 44100

App app = {.dt = 1.0f / 60.0f};

static char toast[160];
static uint64_t toast_until;
/* messages that arrive while one shows wait their turn */
static char toast_queue[4][160];
static uint64_t toast_queue_us[4];
static uint64_t toast_total_us = 2500000; /* how long the one on screen stays */
static int toast_queued;
static int covers_style_loaded = -1;
static bool covers_download_loaded;

static void show_next_toast(const char *text, uint64_t length_us)
{
    str_copy(toast, sizeof(toast), text);
    toast_total_us = length_us;
    toast_until = plat_ticks_us() + length_us;
}

/* Every notice ends as a sentence: a full stop is added unless it already ends in punctuation (or a button picture) */
static const char *toast_sentence(const char *text, char *out, size_t size)
{
    size_t n = strlen(text);
    if (n == 0 || n + 2 > size)
        return text;
    char last = text[n - 1];
    if (strchr(".!?:;]", last) || (n >= 3 && !memcmp(text + n - 3, "\xE2\x80\xA6", 3))) /* … */
        return text;
    memcpy(out, text, n);
    out[n] = '.';
    out[n + 1] = '\0';
    return out;
}

void app_toast(const char *message)
{
    app_toast_for(message, 2.5f);
}

void app_toast_for(const char *message, float seconds)
{
    SwanStationPS5_log("%s", message);
    if (app.game && app.global.nostalgia && (app.screen == SCREEN_GAME || app.screen == SCREEN_LIBRARY))
        return;
    uint64_t length_us = (uint64_t)(seconds * 1e6f);
    char sentence[sizeof(toast)];
    const char *text = toast_sentence(tr(message), sentence, sizeof(sentence));
    if (toast[0] && plat_ticks_us() < toast_until)
    {
        if (!strcmp(toast, text))
            return;
        if (toast_queued < 4)
        {
            toast_queue_us[toast_queued] = length_us;
            str_copy(toast_queue[toast_queued++], sizeof(toast_queue[0]), text);
        }
        return;
    }
    show_next_toast(text, length_us);
}

/* A message's line, with the pictures of the buttons where it says [View button], [L3] or [R3]; its width (drawn when asked) */
static float toast_runs(const char *s, float x, float y, float a, bool draw)
{
    static const struct
    {
        const char *tag;
        enum PadGlyph glyph;
    } TAGS[] = {{"[View button]", GLYPH_TOUCHPAD}, {"[L3]", GLYPH_L3}, {"[R3]", GLYPH_R3}};
    const float size = 26, gap = 10;
    float start = x;
    while (*s)
    {
        const char *next = NULL;
        int which = -1;
        for (int t = 0; t < 3; ++t)
        {
            const char *at = strstr(s, TAGS[t].tag);
            if (at && (!next || at < next))
                next = at, which = t;
        }
        char run[160];
        size_t n = next ? (size_t)(next - s) : strlen(s);
        n = n < sizeof(run) - 1 ? n : sizeof(run) - 1;
        memcpy(run, s, n);
        run[n] = '\0';
        if (n && draw)
            text_draw(x, y, size, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, run);
        x += text_width(size, FONT_REGULAR, run);
        if (!next)
            break;
        float gw = pad_glyph_width(TAGS[which].glyph, size);
        x += gap;
        if (draw)
            draw_pad_glyph(TAGS[which].glyph, x + gw * 0.5f, y + size * 0.6f, size);
        x += gw + gap;
        s = next + strlen(TAGS[which].tag);
    }
    return x - start;
}

/* The screens call app_draw_toast() as they always did; it draws nothing there: the main loop draws the toast once,
 * after everything (the import card, the boxes), so it is never behind one. */
void app_draw_toast(void)
{
}

static void toast_render(void)
{
    uint64_t now = plat_ticks_us();
    if (toast_queued && (!toast[0] || now > toast_until))
    {
        show_next_toast(toast_queue[0], toast_queue_us[0]);
        memmove(toast_queue[0], toast_queue[1], sizeof(toast_queue[0]) * 3);
        memmove(toast_queue_us, toast_queue_us + 1, sizeof(toast_queue_us[0]) * 3);
        --toast_queued;
    }
    if (!toast[0] || now > toast_until)
        return;
    /* slide up in the first 150 ms, fade out in the last 300 ms */
    float shown = (float)(toast_total_us - (toast_until - now)) / 1e6f;
    float left = (float)(toast_until - now) / 1e6f;
    float a = left < 0.3f ? left / 0.3f : 1.0f;
    float rise = shown < 0.15f ? (1.0f - shown / 0.15f) * 24.0f : 0.0f;
    /* a line break in the message makes a second line */
    char first[sizeof(toast)];
    const char *second = strchr(toast, '\n');
    size_t first_len = second ? (size_t)(second - toast) : strlen(toast);
    memcpy(first, toast, first_len);
    first[first_len] = '\0';
    if (second)
        ++second;
    const float line_gap = 36;
    float w = toast_runs(first, 0, 0, 0, false), h = second ? 64 + line_gap : 64;
    if (second)
    {
        float w2 = toast_runs(second, 0, 0, 0, false);
        w = w2 > w ? w2 : w;
    }
    w += 96;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 190 - (second ? line_gap : 0) + rise;
    draw_rrect(x, y, w, h, h * 0.5f > 32 ? 32 : h * 0.5f, argb_alpha(TH_PILL_A(0xf0), a));
    icon_draw(ICON_INFO_CIRCLE, x + 22, y + h * 0.5f - 14, 28, argb_alpha(TH_FOCUS, a));
    draw_set_glyph_alpha(a); /* the pictures of the buttons fade with the text */
    toast_runs(first, x + 62, y + 17, a, true);
    if (second)
        toast_runs(second, x + 62, y + 17 + line_gap, a, true);
    draw_set_glyph_alpha(1.0f);
}

/* ---------------------------------------------------------------- message box */

static char box_text[480];
static bool box_open;
static float box_t; /* 0..1, the fade in */

void app_message_box(const char *message)
{
    str_copy(box_text, sizeof(box_text), tr(message));
    box_open = true;
    box_t = 0.0f;
}

/* X closes it; true while it is up (the screens below get no buttons then) */
static bool message_box_input(uint32_t *pressed)
{
    if (!box_open)
        return false;
    if (*pressed & BIT(BTN_CROSS))
    {
        box_open = false;
        sfx_play(SFX_BACK);
    }
    *pressed = 0;
    return true;
}

static void message_box_draw(void)
{
    if (!box_open)
        return;
    box_t = fminf(box_t + app.dt * 8.0f, 1.0f);
    const float side = 640, pad = 48, size = 28;
    float x = (plat_width() - side) * 0.5f, y = (plat_height() - side) * 0.5f + (1.0f - box_t) * 24.0f;
    draw_rrect(x, y, side, side, TH_RADIUS, argb_alpha(TH_PILL_A(0xf0), box_t));
    icon_draw(ICON_INFO_CIRCLE, x + (side - 56) * 0.5f, y + pad, 56, argb_alpha(TH_FOCUS, box_t));
    /* the message, word-wrapped and centred */
    char line[256];
    float yy = y + pad + 56 + 28;
    const char *m = box_text;
    while (*m && yy < y + side - 110)
    {
        size_t best = 0, i = 0;
        for (;;)
        {
            size_t j = i;
            while (m[j] && m[j] != ' ')
                ++j;
            size_t n = j < sizeof(line) - 1 ? j : sizeof(line) - 1;
            memcpy(line, m, n);
            line[n] = '\0';
            if (best > 0 && text_width(size, FONT_REGULAR, line) > side - 2 * pad)
                break;
            best = n;
            if (!m[j])
                break;
            i = j + 1;
        }
        if (best == 0)
            best = strlen(m) < sizeof(line) - 1 ? strlen(m) : sizeof(line) - 1;
        memcpy(line, m, best);
        line[best] = '\0';
        text_draw(x + side * 0.5f, yy, size, FONT_REGULAR, argb_alpha(TH_TEXT, box_t), ALIGN_CENTER, line);
        yy += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
    }
    /* "(X) Close" at the bottom */
    const char *close = tr("Close");
    const float hs = 26;
    float gw = pad_glyph_width(GLYPH_CROSS, hs);
    float hw = gw + hs * 0.35f + text_width(hs, FONT_REGULAR, close);
    float hx = x + (side - hw) * 0.5f, hy = y + side - pad - hs;
    draw_pad_glyph(GLYPH_CROSS, hx + gw * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw + hs * 0.35f, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, box_t), ALIGN_LEFT, close);
}

void app_draw_hints(const int *glyphs, const char *const *labels, int count, const char *right)
{
    draw_rect(TH_MARGIN, TH_HINT_Y - 22, plat_width() - 2 * TH_MARGIN, 2, TH_DIVIDER);
    float x = TH_MARGIN;
    for (int i = 0; i < count; ++i)
        x += draw_hint(x, TH_HINT_Y, (enum PadGlyph)glyphs[i], labels[i], 26, TH_HINT);
    if (right)
        text_draw(plat_width() - TH_MARGIN, TH_HINT_Y, 26, FONT_REGULAR, TH_HINT, ALIGN_RIGHT,
                  tr(right));
}

void app_draw_hints_right(const HintCombo *items, int count)
{
    const float size = 26, gap = size * 1.4f;
    float total = 0;
    for (int i = 0; i < count; ++i)
        total += hint_combo_width(items[i].glyphs, items[i].count, items[i].label, size) +
                 (i < count - 1 ? gap : 0);
    float x = plat_width() - TH_MARGIN - total;
    for (int i = 0; i < count; ++i)
    {
        draw_hint_combo(x, TH_HINT_Y, items[i].glyphs, items[i].count, items[i].sep,
                        items[i].label, size, TH_HINT);
        x += hint_combo_width(items[i].glyphs, items[i].count, items[i].label, size) + gap;
    }
}

/* ---------------------------------------------------------------- input edges */

int app_nav_pad(void)
{
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        if (app.pads[i].connected)
            return i;
    return 0;
}

/* Button presses for menus, with auto-repeat on directions and shoulders. Only the navigating controller counts: the
 * others are for playing. */
static uint32_t nav_pressed(const PadState *pads)
{
    static uint32_t previous;
    static uint64_t repeat_at;
    static int repeats;
    uint32_t held = 0;
    {
        const int i = app_nav_pad();
        held |= pads[i].buttons;
        if (pads[i].ly < -20000)
            held |= BIT(BTN_UP);
        if (pads[i].ly > 20000)
            held |= BIT(BTN_DOWN);
        if (pads[i].lx < -20000)
            held |= BIT(BTN_LEFT);
        if (pads[i].lx > 20000)
            held |= BIT(BTN_RIGHT);
    }
    uint32_t pressed = held & ~previous;
    const uint32_t dirs = BIT(BTN_UP) | BIT(BTN_DOWN) | BIT(BTN_LEFT) | BIT(BTN_RIGHT) |
                          BIT(BTN_L1) | BIT(BTN_R1) | BIT(BTN_L2) | BIT(BTN_R2);
    uint64_t now = plat_ticks_us();
    if (pressed & dirs)
    {
        repeat_at = now + 260000;
        repeats = 0;
    }
    else if ((held & dirs) && now >= repeat_at)
    {
        pressed |= held & dirs;
        /* speeds up the longer it is held */
        repeat_at = now + (repeats < 6 ? 70000 : 40000);
        ++repeats;
    }
    previous = held;
    return pressed;
}

/* ---------------------------------------------------------------- helpers */

void app_state_path(char *out, size_t size, int slot)
{
    char file[96];
    snprintf(file, sizeof(file), "%.80s.state%d", app.game ? app.game->id : "game", slot);
    path_join(out, size, app.paths.states, file);
}

/* The BIOS a region will run, from the choice in Console (core): that file when it is in bios/ (OpenBIOS is built into
 * the core, so it needs no file); with Auto, or a file that is gone, the BIOS of that region when the folder has one, else any it
 * holds, or OpenBIOS when it holds none. */
static void bios_for_region(const char *key, char *out, size_t size)
{
    for (int i = 0; i < SS_OPT_COUNT; ++i)
    {
        if (strcmp(SS_OPTS[i].key, key) != 0)
            continue;
        const char *file = ss_opt_value(i, app.global.ss_opt[i]); /* "" is Auto */
        char path[SwanStationPS5_PATH_MAX];
        path_join(path, sizeof(path), app.paths.bios, file);
        if (file[0] && access(path, R_OK) == 0)
        {
            str_copy(out, size, file);
            return;
        }
        if (strcmp(file, "openbios.bin") == 0)
        {
            str_copy(out, size, "OpenBIOS");
            return;
        }
        /* Auto, or a file that is gone: what the core will run (see bios_auto_file) */
        bool core_finds;
        const char *auto_file = bios_auto_file(i, &core_finds);
        str_copy(out, size, auto_file ? auto_file : "OpenBIOS");
        return;
    }
    str_copy(out, size, "?");
}

/* Settings > About > BIOS: the one each region will run */
void app_describe_bios(char *out, size_t size)
{
    char j[64], u[64], p[64];
    bios_for_region("swanstation_BIOS_PathNTSCJ", j, sizeof(j));
    bios_for_region("swanstation_BIOS_PathNTSCU", u, sizeof(u));
    bios_for_region("swanstation_BIOS_PathPAL", p, sizeof(p));
    snprintf(out, size, "NTSC-J: %s  \xc2\xb7  NTSC-U: %s  \xc2\xb7  PAL: %s", j, u, p);
}

void app_restart_covers(void)
{
    covers_start(&app.library, &app.paths, &app.global);
    covers_style_loaded = app.global.cover_style;
    covers_download_loaded = app.global.cover_download;
}



/* How long each part of the start took (microseconds since the first mark), shown at the screen's corner and in the log */
static uint64_t start_marks[8];
static int start_mark_count;

static void start_mark(void)
{
    if (start_mark_count < 8)
        start_marks[start_mark_count++] = plat_ticks_us();
}

void app_startup_report(char *out, size_t size)
{
    /* marks: 0 start, 1 the screen, 2 the font and first picture, 3 the BIOS, 4 the drives, 5 the library */
    static const char *const names[] = {"screen", "font", "bios", "drives", "library"};
    size_t len = 0;
    out[0] = '\0';
    for (int i = 1; i < start_mark_count && len + 1 < size; ++i)
        len += (size_t)snprintf(out + len, size - len, "%s %llu  ", names[i - 1],
                                (unsigned long long)((start_marks[i] - start_marks[i - 1]) / 1000));
    if (start_mark_count > 1 && len + 1 < size)
        snprintf(out + len, size - len, "(ms)");
}

void app_rescan(void)
{
    if (app.settings_game)
    {
        /* the games are listed again: the shelf's game may not be the same pointer (or there) any more */
        app_save_settings();
        app.settings_game = NULL;
        app.game_has_own = false;
        app.settings = app.global;
    }
    /* The games folder, then on every drive Windows reports (as RetroArch's UWP build lists them: a console gives a USB
     * drive whatever letter it likes) three places: <drive>/SwanStation, <drive>/SwanStation/games and the "Games
     * folder on drives" name ("psx" by default). */
    static char drive_roots[DRIVE_COUNT * 3][SwanStationPS5_PATH_MAX];
    const char *roots[1 + DRIVE_COUNT * 3];
    int root_count = 0;
    roots[root_count++] = app.paths.games;
    char letters[DRIVE_COUNT][4];
    const int drive_total = drives_present(letters);
    /* a drive that answers "access denied" (5) (the system drive is left out: the app's own folders are there). A console
     * answers so on its own system drives all the time: it only counts as blocked if no game was found (see below). */
    bool drive_denied = false;
    for (int d = 0; d < drive_total; ++d)
    {
        if (letters[d][0] == 'C')
            continue;
        char drive_root[8];
        snprintf(drive_root, sizeof(drive_root), "%s/", letters[d]);
        DIR *drive_probe = opendir(drive_root);
        if (drive_probe)
            closedir(drive_probe);
        else if (uwp_opendir_error() == 5)
        {
            /* one is enough to tell, and each question to a denied drive takes a quarter of a second on the console */
            drive_denied = true;
            break;
        }
    }
    int slot = 0;
    for (int d = 0; d < drive_total; ++d)
    {
        snprintf(drive_roots[slot], SwanStationPS5_PATH_MAX, "%s/SwanStation", letters[d]);
        roots[root_count++] = drive_roots[slot++];
        snprintf(drive_roots[slot], SwanStationPS5_PATH_MAX, "%s/SwanStation/games", letters[d]);
        roots[root_count++] = drive_roots[slot++];
        if (app.global.usb_folder[0])
        {
            snprintf(drive_roots[slot], SwanStationPS5_PATH_MAX, "%s/%s", letters[d], app.global.usb_folder);
            roots[root_count++] = drive_roots[slot++];
        }
    }
    char index[SwanStationPS5_PATH_MAX];
    path_join(index, sizeof(index), app.paths.root, "library.txt");
    char serials[SwanStationPS5_PATH_MAX];
    path_join(serials, sizeof(serials), app.paths.cache, "serials.tsv");
    library_serial_cache_load(serials);
    if (app.sandboxed)
    {
        /* Can't list folders: the sync tool's index is the library. */
        if (!library_load_index(&app.library, index))
            app.library.count = 0;
    }
    else
    {
        library_scan(&app.library, roots, root_count);
        if (app.library.count == 0)
            library_load_index(&app.library, index);
    }
    app.file_access_denied = drive_denied && app.library.count == 0;
    if (app.file_access_denied)
        SwanStationPS5_log("drives: access denied and no games found: the drives cannot be read");
    shelf_show_all();
    app_restart_covers();
}

const Paths *app_paths(void)
{
    return &app.paths;
}

void app_game_config_path(char *out, size_t size, const Game *g)
{
    char dir[SwanStationPS5_PATH_MAX], file[96];
    path_join(dir, sizeof(dir), app.paths.user, "game-settings");
    snprintf(file, sizeof(file), "%.80s.ini", g ? g->id : "none");
    path_join(out, size, dir, file);
}

void app_save_settings(void)
{
    config_save(&app.global, app.paths.config);
    const Game *own_game = app.game ? app.game : app.settings_game;
    if (own_game && app.game_has_own)
    {
        char path[SwanStationPS5_PATH_MAX], dir[SwanStationPS5_PATH_MAX];
        path_join(dir, sizeof(dir), app.paths.user, "game-settings");
        make_dirs(dir);
        app_game_config_path(path, sizeof(path), own_game);
        Settings own = app.settings;
        for (int i = 0; i < SS_OPT_COUNT; ++i)
            if (ss_opt_is_bios(i))
                own.ss_opt[i] = SS_OPTS[i].def; /* there is no BIOS per game: the file does not carry it */
        config_save(&own, path);
    }
    if (app.global.cover_style != covers_style_loaded ||
        app.global.cover_download != covers_download_loaded)
        app_restart_covers();
}

/* ---------------------------------------------------------------- the update question */

/* After an update the first screen asks whether to start from the new version's factory settings: X does, Circle
 * keeps the settings as they are (the answer is saved, so it is asked once). */
static bool question_open;
static float question_t;

/* A factory reset also drops every game's own settings. */
static void remove_game_settings(void)
{
    char dir[SwanStationPS5_PATH_MAX], file[SwanStationPS5_PATH_MAX];
    path_join(dir, sizeof(dir), app.paths.user, "game-settings");
    DIR *d = opendir(dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
        {
            path_join(file, sizeof(file), dir, e->d_name);
            remove(file);
        }
    closedir(d);
}

static void reset_to_factory(void)
{
    remove_game_settings();
    Settings d;
    config_defaults(&d);
    d.last_game = app.global.last_game; /* only where the shelf is stays; the rest, language included, is the factory's */
    app.global = d;
    app.settings = d;
    i18n_set(d.language);
    theme_apply(d.theme);
    sfx_configure(d.ui_sound, (d.ui_volume + 1) * 25);
    remote_update(d.remote);
    app_save_settings();
    app_rescan(); /* the USB folder is back to its own */
    app_toast("Factory settings restored");
}

static void reset_question_open(void)
{
    question_open = true;
    question_t = 0.0f;
}

/* true while it is up: the screens below get no buttons */
static bool question_input(uint32_t *pressed)
{
    if (!question_open)
        return false;
    if (*pressed & BIT(BTN_CROSS))
    {
        question_open = false;
        sfx_play(SFX_SELECT);
        reset_to_factory();
    }
    else if (*pressed & BIT(BTN_CIRCLE))
    {
        question_open = false;
        sfx_play(SFX_BACK);
        app_save_settings(); /* the settings stay; the file now carries this version */
    }
    *pressed = 0;
    return true;
}

static void question_draw(void)
{
    if (!question_open)
        return;
    question_t = fminf(question_t + app.dt * 8.0f, 1.0f);
    const float w = 940, h = 560, pad = 56, size = 30;
    float x = (plat_width() - w) * 0.5f, y = (plat_height() - h) * 0.5f + (1.0f - question_t) * 24.0f;
    draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xa0000000u, question_t));
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_PILL_A(0xf8), question_t));
    icon_draw(ICON_REFRESH, x + (w - 64) * 0.5f, y + pad, 64, argb_alpha(TH_FOCUS, question_t));
    text_draw_fit(x + w * 0.5f, y + pad + 64 + 18, 38, FONT_BOLD, argb_alpha(TH_TEXT, question_t), ALIGN_CENTER, w - 2 * pad,
                  tr("SwanStation was updated to the latest version!"));
    /* the question, word-wrapped and centred */
    char line[256];
    float yy = y + pad + 64 + 18 + 38 + 26;
    const char *m = tr("Resetting to the factory settings is recommended because of the new improvements. Do you want to do it?");
    while (*m && yy < y + h - 130)
    {
        size_t best = 0, i = 0;
        for (;;)
        {
            size_t j = i;
            while (m[j] && m[j] != ' ')
                ++j;
            size_t n = j < sizeof(line) - 1 ? j : sizeof(line) - 1;
            memcpy(line, m, n);
            line[n] = '\0';
            if (best > 0 && text_width(size, FONT_REGULAR, line) > w - 2 * pad)
                break;
            best = n;
            if (!m[j])
                break;
            i = j + 1;
        }
        if (best == 0)
            best = strlen(m) < sizeof(line) - 1 ? strlen(m) : sizeof(line) - 1;
        memcpy(line, m, best);
        line[best] = '\0';
        text_draw(x + w * 0.5f, yy, size, FONT_REGULAR, argb_alpha(TH_TEXT, question_t), ALIGN_CENTER, line);
        yy += size * 1.45f;
        m += best;
        while (*m == ' ')
            ++m;
    }
    /* the two answers, each with its button: (X) Reset to factory settings   (O) Keep my settings */
    const float hs = 28, gap = hs * 0.4f;
    const char *yes = tr("Reset to factory settings"), *no = tr("Keep my settings");
    const float gw1 = pad_glyph_width(GLYPH_CROSS, hs), gw2 = pad_glyph_width(GLYPH_CIRCLE, hs);
    const float w1 = gw1 + gap + text_width(hs, FONT_BOLD, yes), w2 = gw2 + gap + text_width(hs, FONT_REGULAR, no);
    const float space = 56, hy = y + h - pad - hs, total = w1 + space + w2;
    float hx = x + (w - total) * 0.5f;
    draw_pad_glyph(GLYPH_CROSS, hx + gw1 * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw1 + gap, hy, hs, FONT_BOLD, argb_alpha(TH_FOCUS, question_t), ALIGN_LEFT, yes);
    hx += w1 + space;
    draw_pad_glyph(GLYPH_CIRCLE, hx + gw2 * 0.5f, hy + hs * 0.6f, hs);
    text_draw(hx + gw2 + gap, hy, hs, FONT_REGULAR, argb_alpha(TH_TEXT, question_t), ALIGN_LEFT, no);
}

/* ---------------------------------------------------------------- achievements banner */

/* A home-screen forwarder (the Xbox Forwarder Format) starts the app with --rom <path> and maybe --exit-after-game:
 * that game starts at once, and with the flag the app closes when it ends. */
static char launch_rom[SwanStationPS5_PATH_MAX];
static bool exit_after_game;
static bool started_from_argument;
static uint64_t app_started_us;

/* Top right, one message at a time: slides in, holds, fades. */
/* A small card bottom right while a counted achievement moves. */
static void draw_tracker(void)
{
    char title[96], progress[24];
    float percent;
    if (!ra_tracker(title, sizeof(title), progress, sizeof(progress), &percent))
        return;
    const float w = 440, h = 92, x = plat_width() - w - 48, y = plat_height() - h - 48;
    draw_rrect(x, y, w, h, TH_RADIUS, TH_CARD_A(0xe8));
    icon_draw(ICON_TROPHY, x + 18, y + 18, 32, TH_GOLD);
    text_draw_fit(x + 62, y + 16, 22, FONT_BOLD, TH_TEXT, ALIGN_LEFT, w - 180, title);
    char right[48];
    snprintf(right, sizeof(right), "%s  %d%%", progress, (int)(percent + 0.5f));
    text_draw(x + w - 20, y + 18, 20, FONT_REGULAR, TH_FOCUS, ALIGN_RIGHT, right);
    float bw = w - 40, k = percent < 0 ? 0 : percent > 100 ? 1 : percent / 100.0f;
    draw_rrect(x + 20, y + 62, bw, 10, 5, TH_PILL);
    if (k > 0)
        draw_rrect(x + 20, y + 62, bw * k < 10 ? 10 : bw * k, 10, 5, TH_GOLD);
}

/* Splits `s` into lines no wider than max_w, at its own line breaks and at spaces (or between characters when a word is wider, as in Japanese).
 * Returns how many lines were filled; what does not fit in max_lines is dropped. */
#define WRAP_LINE 200
static int wrap_lines(const char *s, float size, int weight, float max_w, char out[][WRAP_LINE], int max_lines)
{
    int n = 0, len = 0, space = -1;
    char line[WRAP_LINE];
    line[0] = '\0';
    for (const char *p = s; *p && n < max_lines;)
    {
        const unsigned char b = (unsigned char)*p;
        if (b == '\n')
        {
            memcpy(out[n++], line, (size_t)len + 1); /* a break the text asks for */
            len = 0;
            line[0] = '\0';
            space = -1;
            ++p;
            continue;
        }
        int c = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : 4;
        if (len + c >= WRAP_LINE)
            break;
        memcpy(line + len, p, (size_t)c);
        len += c;
        line[len] = '\0';
        if (b == ' ')
            space = len - 1;
        if (len > c && text_width(size, weight, line) > max_w)
        {
            int cut = space > 0 ? space : len - c;
            int rest = space > 0 ? space + 1 : len - c;
            memcpy(out[n], line, (size_t)cut);
            out[n++][cut] = '\0';
            memmove(line, line + rest, (size_t)(len - rest) + 1);
            len -= rest;
            space = -1;
            for (int i = 0; i < len; ++i)
                if (line[i] == ' ')
                    space = i;
        }
        p += c;
    }
    if (len > 0 && n < max_lines)
    {
        memcpy(out[n], line, (size_t)len + 1);
        ++n;
    }
    return n;
}

static void draw_achievement(void)
{
    const bool nostalgia = app.game && app.global.nostalgia && app.screen == SCREEN_GAME;
    if (app.screen == SCREEN_GAME && app.global.ra_tracker && !nostalgia)
        draw_tracker();
    static char title[96], detail[192];
    static uint64_t shown_at;
    const uint64_t length = 4500000;
    static bool showing, quiet;
    uint64_t now = plat_ticks_us();
    if (!showing || now > shown_at + length)
    {
        showing = ra_next_message(title, sizeof(title), detail, sizeof(detail), &quiet);
        if (!showing)
            return;
        /* pop-ups off: messages that arrive during play are dropped, not shown */
        if ((!app.global.ra_popups || nostalgia) && app.screen == SCREEN_GAME)
        {
            showing = false;
            return;
        }
        shown_at = now;
        /* quiet: the summary shown when a game opens; the sound is for the rest */
        if (!quiet)
            sfx_play(SFX_SELECT);
    }
    float t = (float)(now - shown_at) / 1e6f, total = length / 1e6f;
    float in = t < 0.25f ? t / 0.25f : 1.0f, out = total - t < 0.4f ? (total - t) / 0.4f : 1.0f;
    float a = in < out ? in : out, slide = (1.0f - in) * (1.0f - in) * 80.0f;
    if (app.global.ra_popup_style == 1)
    {
        /* compact: a line at the top, two when it does not fit */
        char line[300], rows[2][WRAP_LINE];
        snprintf(line, sizeof(line), "%s  \xc2\xb7  %s", title, detail);
        const float room = plat_width() - 200 - 80;
        int n = wrap_lines(line, 22, FONT_REGULAR, room, rows, 2);
        float lw = text_width(22, FONT_REGULAR, line) + 90;
        if (n > 1 || lw > plat_width() - 200)
            lw = plat_width() - 200;
        float lh = 52 + (n > 1 ? (n - 1) * 28 : 0);
        float lx = (plat_width() - lw) * 0.5f, ly = 24 - slide * 0.5f;
        draw_rrect(lx, ly, lw, lh, n > 1 ? TH_RADIUS_SMALL : 26, argb_alpha(TH_CARD_A(0xf0), a));
        icon_draw(ICON_TROPHY, lx + 18, ly + 11, 30, argb_alpha(TH_GOLD, a));
        for (int i = 0; i < n; ++i)
            text_draw_fit(lx + 60, ly + 13 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT, a), ALIGN_LEFT, lw - 80,
                          rows[i]);
        return;
    }
    if (app.global.ra_popup_style == 2)
    {
        /* big trophy, in the middle */
        char rows[3][WRAP_LINE];
        int n = wrap_lines(detail, 22, FONT_REGULAR, 700, rows, 3);
        float bw = 760, bh = 300 + (n > 1 ? (n - 1) * 28 : 0), bx = (plat_width() - bw) * 0.5f,
              by = (plat_height() - bh) * 0.5f - 40 + slide * 0.4f;
        draw_rrect(bx, by, bw, bh, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
        draw_circle(bx + bw * 0.5f, by + 92, 62, argb_alpha(0xff2a2410u, a));
        icon_draw(ICON_TROPHY, bx + bw * 0.5f - 44, by + 48, 88, argb_alpha(TH_GOLD, a));
        text_draw_fit(bx + bw * 0.5f, by + 176, 34, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_CENTER, bw - 60, title);
        for (int i = 0; i < n; ++i)
            text_draw_fit(bx + bw * 0.5f, by + 230 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_CENTER,
                          bw - 60, rows[i]);
        return;
    }
    char rows[3][WRAP_LINE];
    int n = wrap_lines(detail, 22, FONT_REGULAR, 660 - 140, rows, 3);
    const float w = 660, h = 116 + (n > 1 ? (n - 1) * 28 : 0), x = plat_width() - w - 48 + slide, y = 48;
    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), a));
    draw_rrect(x + 18, y + 18, 80, 80, TH_RADIUS_SMALL, argb_alpha(0xff2a2410u, a));
    icon_draw(ICON_TROPHY, x + 30, y + 30, 56, argb_alpha(TH_GOLD, a));
    text_draw_fit(x + 120, y + 20, 30, FONT_BOLD, argb_alpha(TH_TEXT, a), ALIGN_LEFT, w - 140, title);
    for (int i = 0; i < n; ++i)
        text_draw_fit(x + 120, y + 66 + i * 28, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, a), ALIGN_LEFT, w - 140,
                      rows[i]);
}

/* ---------------------------------------------------------------- game start/stop */

static void draw_timer(void);

static bool speed_changed; /* fast forward or rewind: not the game's normal speed */

void app_draw_game(uint8_t dim)
{
    int w, h, fmt;
    size_t pitch;
    bool fresh;
    const void *pixels = host_frame(&w, &h, &pitch, &fmt, &fresh);
    if (pixels && fresh)
        plat_upload_game(pixels, w, h, pitch, fmt, app.settings.upscale, app.settings.upscale_filter);
    else if (!pixels && fresh && w > 0)
        plat_upload_game_gpu(w, h); /* the core rendered through Vulkan */
    Settings view = app.settings;
    if (bezel_shown(&view))
        view.border = 0; /* the bezel's window is the full-height 4:3 picture */

    /* around the picture: black, the current background, or a TV */
    int gx, gy, gw, gh;
    plat_game_rect(&gx, &gy, &gw, &gh);
    if (view.border && !bezel_shown(&view) && gw > 0 && gw < plat_width() - 8)
    {
        float k = dim / 255.0f;
        if (view.border == 1)
        {
            shelf_backdrop();
            draw_rect(0, 0, plat_width(), plat_height(), argb_alpha(0xff000000u, 0.45f + 0.55f * (1 - k)));
        }
        else
        {
            /* a 90s TV: dark room, plastic body, recessed screen, a power light */
            draw_rect(0, 0, plat_width(), plat_height(), argb_lerp(0xff000000u, 0xff0c0d14u, k));
            draw_rrect(gx - 74, gy - 58, gw + 148, gh + 130, 56, argb_lerp(0xff000000u, 0xff26262du, k));
            draw_rrect(gx - 34, gy - 58, gw + 68, 8, 4, argb_lerp(0xff000000u, 0xff36363fu, k));
            draw_rrect(gx - 26, gy - 24, gw + 52, gh + 48, 34, argb_lerp(0xff000000u, 0xff0e0e11u, k));
            draw_circle(gx + gw + 30, gy + gh + 44, 7, argb_lerp(0xff000000u, 0xff3cd070u, k));
            text_draw(gx + gw * 0.5f, gy + gh + 32, 22, FONT_BOLD, argb_lerp(0xff000000u, 0xff5a5a66u, k),
                      ALIGN_CENTER, SwanStationPS5_TITLE);
        }
    }
    plat_set_colour(view.brightness, view.colour, view.sharpen);
    plat_set_fsr(app.settings.fsr);
    {
        double core_hz = host_fps();
        double speed = app.fps > 1.0f && core_hz > 1.0 ? app.fps * 100.0 / core_hz : 100.0;
        plat_set_framegen(app.settings.framegen == 2 ? 3 : app.settings.framegen, core_hz, speed, !speed_changed);
    }
    plat_draw_game(&view, play_widescreen_active() ? 16.0f / 9.0f : host_aspect(), dim);
    bezel_draw(&view, dim);
    if (app.screen == SCREEN_GAME)
        draw_timer();
}

/* Is there a BIOS file (any .bin) in dir? When the folder can't be listed (sandboxed mode) we can't tell: yes. */
static bool bios_dir_has_bin(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return true;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
        found = str_icmp(path_ext(e->d_name), "bin") == 0;
    closedir(d);
    return found;
}

/* Is there a BIOS file (any .bin) in bios/? */
static bool bios_file_present(void)
{
    return bios_dir_has_bin(app.paths.bios);
}

/* A BIOS (a .bin) in dir: unlike bios_dir_has_bin, a folder that cannot be opened has none. */
static bool bios_dir_really_has_bin(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)))
        found = str_icmp(path_ext(e->d_name), "bin") == 0;
    closedir(d);
    return found;
}

/* The BIOS is looked for in the app's own bios folder; when that holds none, in <drive>/bios and
 * <drive>/SwanStation/bios on every drive (a USB with bios/ beside psx/, not inside it: a folder of the games folder
 * would be taken for a game), and the first folder with a .bin becomes the BIOS folder: the core is told it as its
 * system directory. The core takes the BIOS of the game's region from it (scph5500 for Japan, 5501 USA, 5502 Europe).
 * Before the settings are read. */
static void bios_use_drive_dir(void)
{
    if (bios_dir_really_has_bin(app.paths.bios))
        return;
    char letters[DRIVE_COUNT][4];
    const int total = drives_present(letters);
    for (int d = 0; d < total; ++d)
        for (int k = 0; k < 2; ++k)
        {
            char dir[SwanStationPS5_PATH_MAX];
            snprintf(dir, sizeof(dir), k == 0 ? "%s/bios" : "%s/SwanStation/bios", letters[d]);
            if (!bios_dir_really_has_bin(dir))
                continue;
            str_copy(app.paths.bios, sizeof(app.paths.bios), dir);
            SwanStationPS5_log("bios: none in our bios folder, using %s", dir);
            return;
        }
}

/* A libcrypt game (most PAL discs) needs its .sbi file beside the disc image, with the image's name. The package holds
 * them (assets/sbi_files/<serial>.sbi: patches, from psxdatacenter): when the disc has none, its copy is put there. */

/* Spyro: Year of the Dragon and CTR have two pressings under one serial, each with its own .sbi: they are told apart by
 * the CRC-32 of the first .bin of the disc (Redump's). */
static const struct
{
    const char *serial;
    uint32_t crc;
    const char *file;
} SBI_BY_CRC[] = {
    {"SCES-02105", 0xf21229a3u, "SCES-02105_NoEDC.sbi"}, /* CTR, original */
    {"SCES-02105", 0x2a98762fu, "SCES-02105_EDC.sbi"},   /* CTR, Platinum */
    {"SCES-02835", 0x5d3785eeu, "SCES-02835_v1.0.sbi"},  /* Spyro, original */
    {"SCES-02835", 0x9bcfefe1u, "SCES-02835_v1.1.sbi"},  /* Spyro, Platinum */
};

/* CRC-32 of a file; `permille` (0..1000) says how far it is, for the bar */
static bool crc32_of_file(const char *path, uint32_t *out, SDL_atomic_t *permille)
{
    static uint32_t table[256];
    if (!table[1])
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k)
                c = c & 1 ? 0xedb88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fseek(f, 0, SEEK_END);
    long total = ftell(f);
    fseek(f, 0, SEEK_SET);
    static uint8_t buf[256 * 1024];
    uint32_t crc = 0xffffffffu;
    size_t n;
    long done = 0;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ buf[i]) & 0xff] ^ (crc >> 8);
        done += (long)n;
        SDL_AtomicSet(permille, total > 0 ? (int)(done * 1000 / total) : 0);
    }
    fclose(f);
    *out = ~crc;
    return true;
}

/* The disc's first .bin: the image itself, or the first FILE of its .cue (beside it) */
static bool first_bin_of(const char *image, char *bin, size_t size)
{
    const char *ext = path_ext(image);
    if (!strcasecmp(ext, "bin"))
    {
        str_copy(bin, size, image);
        return true;
    }
    if (strcasecmp(ext, "cue"))
        return false;
    FILE *f = fopen(image, "rb");
    if (!f)
        return false;
    char line[512], name[SwanStationPS5_PATH_MAX] = "";
    while (fgets(line, sizeof(line), f))
    {
        const char *p = line;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (strncasecmp(p, "FILE", 4))
            continue;
        const char *q = strchr(p, '"'), *e = q ? strchr(q + 1, '"') : NULL;
        if (!e)
            break;
        size_t len = (size_t)(e - q - 1);
        if (len < sizeof(name))
        {
            memcpy(name, q + 1, len);
            name[len] = '\0';
        }
        break;
    }
    fclose(f);
    if (!name[0])
        return false;
    str_copy(bin, size, image);
    char *slash = strrchr(bin, '/');
    size_t dir = slash ? (size_t)(slash - bin) + 1 : 0;
    if (dir + strlen(name) >= size)
        return false;
    strcpy(bin + dir, name);
    return true;
}

/* The CRC-32 runs in a thread, with a bar on screen meanwhile; the game starts when it is done */
static struct
{
    bool active;
    SDL_atomic_t done, permille;
    bool ok;
    uint32_t crc;
    char bin[SwanStationPS5_PATH_MAX], serial[16], target[SwanStationPS5_PATH_MAX];
    int game;
    bool resume;
} SC;
static int sbi_checked_game = -1; /* its CRC was read: start it, whatever the result was */

static int sbi_crc_thread(void *unused)
{
    (void)unused;
    SC.ok = crc32_of_file(SC.bin, &SC.crc, &SC.permille);
    SDL_AtomicSet(&SC.done, 1);
    return 0;
}

/* The copy that matches the CRC-32, once it is known */
static void sbi_finish(void)
{
    char name[64], source[SwanStationPS5_PATH_MAX];
    if (!SC.ok)
        return;
    for (size_t i = 0; i < sizeof(SBI_BY_CRC) / sizeof(SBI_BY_CRC[0]); ++i)
    {
        if (strcmp(SBI_BY_CRC[i].serial, SC.serial) || SBI_BY_CRC[i].crc != SC.crc)
            continue;
        snprintf(name, sizeof(name), "sbi_files/%s", SBI_BY_CRC[i].file);
        plat_asset_path(source, sizeof(source), name);
        file_copy(source, SC.target);
        return;
    }
}

/* false when the CRC-32 is being read: the game starts when it is done */
static bool sbi_prepare(int index, bool resume)
{
    const Game *g = &app.library.games[index];
    if (index == sbi_checked_game)
    {
        sbi_checked_game = -1;
        return true;
    }
    if (SC.active || !g->serial[0] || !strcasecmp(path_ext(g->path), "m3u"))
        return !SC.active;
    char target[SwanStationPS5_PATH_MAX], name[64], source[SwanStationPS5_PATH_MAX];
    str_copy(target, sizeof(target), g->path);
    char *slash = strrchr(target, '/'), *dot = strrchr(target, '.');
    if (!dot || (slash && dot < slash) || (size_t)(dot - target) + 5 > sizeof(target))
        return true;
    strcpy(dot, ".sbi");
    FILE *have = fopen(target, "rb");
    if (have)
    {
        fclose(have);
        return true;
    }
    snprintf(name, sizeof(name), "sbi_files/%s.sbi", g->serial);
    plat_asset_path(source, sizeof(source), name);
    if (file_copy(source, target))
        return true;
    /* a serial with two pressings: the disc's CRC-32 says which */
    bool listed = false;
    for (size_t i = 0; i < sizeof(SBI_BY_CRC) / sizeof(SBI_BY_CRC[0]); ++i)
        listed |= !strcmp(SBI_BY_CRC[i].serial, g->serial);
    if (!listed || !first_bin_of(g->path, SC.bin, sizeof(SC.bin)))
        return true;
    str_copy(SC.serial, sizeof(SC.serial), g->serial);
    str_copy(SC.target, sizeof(SC.target), target);
    SC.game = index;
    SC.resume = resume;
    SC.ok = false;
    SDL_AtomicSet(&SC.done, 0);
    SDL_AtomicSet(&SC.permille, 0);
    SDL_Thread *t = SDL_CreateThread(sbi_crc_thread, "sbi-crc", NULL);
    if (!t)
        return true; /* no thread: no patch, the box says why the game does not start */
    SDL_DetachThread(t);
    SC.active = true;
    return false;
}

/* Every frame while it runs: the buttons stay out of the screens below; when it is done, the game starts */
static bool sbi_check_input(uint32_t *pressed)
{
    if (!SC.active)
        return false;
    *pressed = 0;
    if (SDL_AtomicGet(&SC.done))
    {
        SC.active = false;
        sbi_finish();
        sbi_checked_game = SC.game;
        app_start_game(SC.game, SC.resume);
    }
    return true;
}

/* The bar, as the one that comes up when O is held to close the app: a panel with the message and the progress */
static void sbi_check_draw(void)
{
    if (!SC.active)
        return;
    const char *msg = tr("Extracting the CRC-32 to try to apply the LibCrypt patch...");
    float w = text_width(26, FONT_REGULAR, msg) + 96, h = 84;
    float x = (plat_width() - w) * 0.5f, y = plat_height() - 300;
    float p = SDL_AtomicGet(&SC.permille) / 1000.0f;
    draw_rrect(x, y, w, h, TH_RADIUS_SMALL, 0xf0000000u | (TH_PILL & 0xffffffu));
    text_draw(plat_width() * 0.5f, y + 12, 26, FONT_REGULAR, TH_TEXT, ALIGN_CENTER, msg);
    draw_rrect(x + 32, y + 56, w - 64, 12, 6, TH_DIVIDER);
    draw_rrect(x + 32, y + 56, fmaxf(12.0f, (w - 64) * fminf(p, 1.0f)), 12, 6, TH_FOCUS);
}

static void crop_ratio_text(char *out, size_t size, float r)
{
    static const struct
    {
        int w, h;
    } COMMON[] = {{4, 3}, {16, 9}, {3, 2}, {1, 1}, {5, 4}, {8, 7}, {2, 1}, {21, 9}, {10, 7}, {16, 10}, {19, 9}};
    for (size_t i = 0; i < sizeof(COMMON) / sizeof(COMMON[0]); ++i)
        if (fabsf(r * COMMON[i].h / COMMON[i].w - 1.0f) < 0.01f)
        {
            snprintf(out, size, "%d:%d", COMMON[i].w, COMMON[i].h);
            return;
        }
    snprintf(out, size, "%.2f:1", r);
}

static const char *menu_combo_hint(void)
{
    if (app.global.menu_hold == 0)
        return app.global.menu_combo == 1   ? "Press {L3} + {R3} to open the game menu."
               : app.global.menu_combo == 2 ? "Press {Options} + {View button} to open the game menu."
                                            : "Press {View button} + {R3} to open the game menu.";
    return app.global.menu_combo == 1   ? "Hold {L3} + {R3} to open the game menu."
           : app.global.menu_combo == 2 ? "Hold {Options} + {View button} to open the game menu."
                                        : "Hold {View button} + {R3} to open the game menu.";
}

void app_start_game(int index, bool resume)
{
    if (index < 0 || index >= app.library.count)
        return;
    if (!sbi_prepare(index, resume))
        return;
    const Game *g = &app.library.games[index];
    char error[480], own[SwanStationPS5_PATH_MAX];
    app_game_config_path(own, sizeof(own), g);
    app.game_has_own = config_load_game(&app.settings, &app.global, own);
    bool translated = play_prepare_patch(g);
    play_rewind_reset();
    controls_start(g, &app.settings);
    app.play_seconds = 0;
    host_set_special(controls_special_for(g, &app.settings));
    {
        const GameInfo *info = gamedb_get(g->serial);
        unsigned fixes = info && app.settings.game_fixes ? info->flags & GDB_FIXES : 0;
        /* a DualShock starts in digital mode until its ANALOG button is pressed;
         * games that use the sticks get it in analog mode (Gran Turismo...) */
        if (info && (info->flags & GDB_ANALOG) && !(info->flags & GDB_NO_AUTO_ANALOG))
            fixes |= GDB_ANALOG;
        host_set_fixes(fixes, info && app.settings.game_fixes ? info : NULL);
        if (fixes || (info && app.settings.game_fixes && info->set))
            SwanStationPS5_log("start: known fixes %x, values %x (game database)", fixes, info ? info->set : 0u);
    }
    SwanStationPS5_log("start: %s (%s) from %s%s", shelf_game_title(g), g->serial, g->path,
              app.game_has_own ? " with its own settings" : "");
    if (!host_load(g->path, g->serial, &app.paths, &app.settings, error, sizeof(error)))
    {
        controls_stop();
        app.settings = app.global;
        if (strstr(error, "libcrypt protection"))
            app_message_box(error); /* it stays until X: it says what to do */
        else
            app_toast(error);
        return;
    }
    app.game = g;
    app.game_index = index;
    app.global.last_game = index;
    config_save(&app.global, app.paths.config);
    GameStats *st = stats_get(g->id);
    if (st)
    {
        st->last_played = (int64_t)time(NULL);
        stats_save();
    }
    if (!bios_file_present()) /* after the load: it blocks, and the notice would run out before it was drawn */
        app_toast_for("BIOS not found, SCPH1001.BIN is recommended due to the low compatibility of OpenBios.", 10.0f);
    if (app.settings.renderer == 1)
        app_toast_for("Rendering on the CPU, using the GPU is recommended.", 5.0f);
    {
        char hint[200];
        snprintf(hint, sizeof(hint), "%s", tr(menu_combo_hint()));
        /* a DualShock: how to press its ANALOG button (the core's combo, Settings > Controls) */
        if (app.settings.analog && !controls_special_for(g, &app.settings))
        {
            static const char *const COMBOS[] = {
                "{L1} + {R1} + {L3} + {R3}", "{L1} + {L2} + {R1} + {R2} + {Options} + {View button}",
                "{L1} + {R1} + {View button}", "{L1} + {R1} + {Options}", "{L1} + {R1} + {L3}", "{L1} + {R1} + {R3}",
                "{L2} + {R2} + {View button}", "{L2} + {R2} + {Options}", "{L2} + {R2} + {L3}", "{L2} + {R2} + {R3}",
                "{L3} + {R3}"};
            int combo = 2; /* L1 + R1 + Select, the core's own default */
            for (int i = 0; i < SS_OPT_COUNT; ++i)
                if (!strcmp(SS_OPTS[i].key, "swanstation_Controller_AnalogCombo"))
                {
                    int v = app.settings.ss_opt[i];
                    combo = v >= 0 && v < 11 ? v : SS_OPTS[i].def;
                }
            size_t n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, "\n");
            n = strlen(hint);
            snprintf(hint + n, sizeof(hint) - n, tr("%s for the ANALOG button."), COMBOS[combo]);
        }
        app_toast_for(hint, 8.0f);
    }
    plat_audio_open(host_sample_rate());
    plat_audio_clear();
    ra_game_loaded(g->path);
    if (ra_hardcore())
        cheats_clear(&app.cheats); /* hardcore: no cheats */
    else if (cheats_load(&app.cheats, g, app.paths.cheats))
    {
        cheats_apply(&app.cheats);
        int on = 0;
        for (int i = 0; i < app.cheats.count; ++i)
            on += app.cheats.items[i].enabled;
        if (on)
        {
            char msg[64];
            snprintf(msg, sizeof(msg), tr(on == 1 ? "%d cheat active" : "%d cheats active"), on);
            app_toast(msg);
        }
    }
    if (play_widescreen())
        app_toast("Widescreen on");
    else if (app.settings.widescreen && !ra_hardcore())
        app_toast("This game has no widescreen code: playing in 4:3");
    if (translated)
        app_toast("Translation patch applied");
    if (resume)
        play_load_resume();
    app.screen = SCREEN_GAME;
}

void app_start_bios(void)
{
    static Game bios;
    memset(&bios, 0, sizeof(bios));
    str_copy(bios.title, sizeof(bios.title), "BIOS");
    str_copy(bios.id, sizeof(bios.id), "bios");
    app_close_shelf_settings(); /* the settings in effect are the global ones, not those of the game under the cursor */
    char error[480];
    app.game_has_own = false;
    app.settings = app.global;
    play_rewind_reset();
    controls_start(&bios, &app.settings);
    app.play_seconds = 0;
    host_set_special(0);
    host_set_fixes(0, NULL);
    static const char *const boot_regions[] = {"NTSC-U", "NTSC-J", "PAL"};
    const char *boot_region = boot_regions[app.global.bios_start >= 0 && app.global.bios_start < 3 ? app.global.bios_start : 0];
    host_set_region_override(boot_region); /* the BIOS the console's choice for that region names */
    SwanStationPS5_log("start: BIOS with no disc (%s)", boot_region);
    if (!host_load("", "", &app.paths, &app.settings, error, sizeof(error)))
    {
        host_set_region_override(NULL);
        controls_stop();
        app_toast(error);
        return;
    }
    app.game = &bios;
    app.game_index = -1;
    if (!bios_file_present())
        app_toast_for("BIOS not found, SCPH1001.BIN is recommended due to the low compatibility of OpenBios.", 10.0f);
    plat_audio_open(host_sample_rate());
    plat_audio_clear();
    cheats_clear(&app.cheats);
    app_toast_for(menu_combo_hint(), 8.0f);
    app.screen = SCREEN_GAME;
}

/* A game leaves memory behind that the heap cannot give back (a console that rests with it mapped does not wake up), so
 * going back to the library starts the title again: the system ends this process and runs the title's executable. */
static void restart_for_clean_memory(void)
{
#if defined(__PROSPERO__)
    static bool asked;
    if (asked)
        return;
    asked = true;
    app_save_settings();
    stats_save();
    char path[SwanStationPS5_PATH_MAX];
    path_join(path, sizeof(path), app.paths.root, "open-library");
    FILE *f = fopen(path, "w");
    if (f)
    {
        fprintf(f, "%lld\n", (long long)time(NULL));
        fclose(f);
    }
    SwanStationPS5_log("restart: the title starts again to give the game's memory back");
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    int rc = sceSystemServiceLoadExec("/app0/eboot.bin", NULL);
    SwanStationPS5_log("restart: refused (0x%x), staying in this process", (unsigned)rc);
    remove(path);
    asked = false;
#endif
}

void app_stop_game(void)
{
    host_set_region_override(NULL);
    play_save_resume(false);
    play_rewind_reset();
    int unlocked, total;
    GameStats *st = app.game && !game_is_bios(app.game) ? stats_get(app.game->id) : NULL;
    if (st && ra_game_progress(&unlocked, &total))
    {
        st->ach_unlocked = unlocked;
        st->ach_total = total;
    }
    stats_save();
    if (!game_is_bios(app.game))
        ra_game_unloaded();
    controls_stop();
    host_unload();
    plat_audio_open(UI_RATE);
    plat_audio_clear();
    cheats_clear(&app.cheats);
    app.game = NULL;
    sfx_silence(false);
    app.game_has_own = false;
    app.settings = app.global;
    app.screen = SCREEN_LIBRARY;
    if (started_from_argument && exit_after_game)
    {
        SwanStationPS5_log("forwarder: the game ended, closing (--exit-after-game)");
        app.quit_requested = true;
    }
    else
        restart_for_clean_memory();
}

/* The library game a forwarder's --rom names: that file, else the game in the folder holding it (or in that folder).
 * -1 when none. */
static int game_for_launch_path(const char *path)
{
    for (int i = 0; i < app.library.count; ++i)
        if (strcmp(app.library.games[i].path, path) == 0)
            return i;
    char folder[SwanStationPS5_PATH_MAX];
    str_copy(folder, sizeof(folder), path);
    size_t length = strlen(folder);
    while (length > 1 && folder[length - 1] == '/')
        folder[--length] = '\0';
    for (int i = 0; i < app.library.count; ++i)
        if (strcmp(app.library.games[i].folder, folder) == 0)
            return i;
    char *slash = strrchr(folder, '/');
    if (slash && slash != folder)
    {
        *slash = '\0';
        for (int i = 0; i < app.library.count; ++i)
            if (strcmp(app.library.games[i].folder, folder) == 0 &&
                strcmp(app.library.games[i].folder, app.paths.games) != 0)
                return i;
    }
    return -1;
}

/* Settings from the shelf: the game under the cursor can have its own, as from the game's menu */
void app_open_shelf_settings(const Game *game)
{
    app.settings_game = game;
    if (game)
    {
        char own[SwanStationPS5_PATH_MAX];
        app_game_config_path(own, sizeof(own), game);
        app.game_has_own = config_load_game(&app.settings, &app.global, own);
    }
    app_open_settings(SCREEN_LIBRARY);
}

void app_close_shelf_settings(void)
{
    if (app.game || !app.settings_game)
        return;
    app.settings_game = NULL;
    app.game_has_own = false;
    app.settings = app.global;
}

void app_open_settings(enum Screen back_to)
{
    app.settings_return = back_to;
    app.screen = SCREEN_SETTINGS;
    settings_opened();
}

/* ---------------------------------------------------------------- emulation */

/* The player's button mapping: each controller button presses its mapped PS1
 * button (Settings > Controls > Button mapping). */
static uint32_t map_buttons(uint32_t buttons)
{
    uint32_t out = buttons & ~0xffffu;
    for (int b = 0; b < 16; ++b)
        if (buttons & BIT(b))
        {
            int to = app.settings.button_map[b];
            if (to >= 0 && to < 16)
                out |= BIT(to);
        }
    return out;
}

/* Speedrun timer: View button + Triangle starts/pauses, View button + Circle resets.
 * It counts only while the game runs (not in the SwanStation menu). */
static struct
{
    bool shown, running;
    double seconds;
} timer;

static void draw_timer(void)
{
    if (!timer.shown)
        return;
    int cs = (int)(timer.seconds * 100.0);
    char text[32];
    if (cs >= 360000)
        snprintf(text, sizeof(text), "%d:%02d:%02d.%02d", cs / 360000, cs / 6000 % 60, cs / 100 % 60, cs % 100);
    else
        snprintf(text, sizeof(text), "%02d:%02d.%02d", cs / 6000, cs / 100 % 60, cs % 100);
    float w = text_width(34, FONT_BOLD, "00:00:00.00") + 48;
    draw_rrect(40, 40, w, 64, TH_RADIUS_SMALL, TH_CARD_A(0xd0));
    text_draw(40 + w * 0.5f, 50, 34, FONT_BOLD, timer.running ? TH_TEXT : TH_GOLD, ALIGN_CENTER, text);
}

static float auto_since, auto_shown; /* auto-save: time since the last, and its icon */

/* Buttons still held from the menu when the game comes back (the Cross that
 * chose Resume): the game doesn't see them until they're let go. */
static uint32_t held_from_menu;
static bool input_reported;      /* the first button that reached the game was logged */
static uint64_t input_start_us;  /* when the game screen was entered */

static void game_screen(PadState *pads)
{
    uint32_t held_now = 0;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        held_now |= pads[i].buttons;
    held_from_menu &= held_now;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
        pads[i].buttons &= ~held_from_menu;
    if (!input_reported)
    {
        uint32_t reaching = 0;
        for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
            reaching |= pads[i].buttons;
        if (reaching)
        {
            input_reported = true;
            SwanStationPS5_log("input: the first buttons reach the game (0x%x) %.1f s after it began", reaching,
                      (double)(plat_ticks_us() - input_start_us) / 1e6);
        }
    }

    /* View button: a tap is the PS1's Select (the Xbox SDL driver has no Create
     * button); the menu is the View button held for 1 second. */
    static uint64_t touch_since[SwanStationPS5_MAX_PADS];
    static bool touch_used[SwanStationPS5_MAX_PADS];
    static int select_frames[SwanStationPS5_MAX_PADS];
    bool touch_of[SwanStationPS5_MAX_PADS], chord_of[SwanStationPS5_MAX_PADS];
    bool touch = false, chord = false;
    uint64_t t_now = plat_ticks_us();
    bool open_menu = false;
    /* View button held + R2: fast forward; + L2: rewind. The shortcuts are player 1's; on the other
     * pads the View button is just that pad's Select */
    uint32_t all = pads[0].buttons;
    /* View button + two shoulder buttons: the DualShock's analog mode combo (L1 + R1 + Select...): the
     * View button is the Select held down, and the shoulders go to the game */
    const uint32_t shoulders = BIT(BTN_L1) | BIT(BTN_R1) | BIT(BTN_L2) | BIT(BTN_R2);
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        touch_of[i] = pads[i].buttons & BIT(BTN_MENU);
        chord_of[i] = touch_of[i] && __builtin_popcount(pads[i].buttons & shoulders) >= 2;
    }
    touch = touch_of[0];
    chord = chord_of[0];
    bool fast = touch && !chord && (all & BIT(BTN_R2)), back = touch && !chord && (all & BIT(BTN_L2));
    speed_changed = fast || back;
    if (touch && (fast || back || chord))
        touch_used[0] = true; /* a combo, not Select or the menu */
    /* View button + Square: screenshot; + Triangle / Circle: timer */
    static uint32_t combo_all_prev;
    uint32_t newly = touch ? all & ~combo_all_prev : 0;
    combo_all_prev = all;
    const uint32_t combo_keys = BIT(BTN_SQUARE) | BIT(BTN_TRIANGLE) | BIT(BTN_CIRCLE);
    if (touch && !chord && (all & combo_keys))
        touch_used[0] = true;
    if (newly & BIT(BTN_SQUARE))
    {
        bool ok = play_screenshot();
        app_toast(ok ? "Screenshot saved in LocalState/SwanStation/screenshots" : "Could not save the screenshot");
        sfx_play(ok ? SFX_SELECT : SFX_BACK);
    }
    if (newly & BIT(BTN_TRIANGLE))
    {
        timer.shown = true;
        timer.running = !timer.running;
        sfx_play(SFX_CLICK);
    }
    if (newly & BIT(BTN_CIRCLE))
    {
        timer.running = false;
        timer.seconds = 0;
        timer.shown = false;
        app_toast("Timer reset");
    }
    /* The View button is also the shortcuts' key, so Select waits this long to be sure no shortcut
     * follows; after it, holding the View button holds Select until a shortcut is used. */
    const uint64_t select_hold_us = 0, tap_us = 150000;
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (touch_of[i])
        {
            if (!touch_since[i])
                touch_since[i] = t_now;
        }
        else
        {
            /* a quick tap: Select for ~100 ms, long enough for every game to see it (a longer
             * touch has been held as Select while it lasted, below) */
            if (touch_since[i] && !touch_used[i] && t_now - touch_since[i] <= tap_us)
                select_frames[i] = 6;
            touch_since[i] = 0;
            touch_used[i] = false;
        }
    }
    /* player 1's menu combination held for the chosen time: the SwanStation menu */
    static uint64_t menu_since;
    static const uint64_t MENU_HOLD_US[3] = {0, 500000, 1000000};
    const uint32_t first_pad = pads[0].buttons;
    const int menu_combo = app.global.menu_combo;
    const bool menu_held = menu_combo == 1 ? (first_pad & BIT(BTN_L3)) && (first_pad & BIT(BTN_R3)) && !(first_pad & shoulders)
                           : menu_combo == 2 ? touch_of[0] && !chord_of[0] && (first_pad & BIT(BTN_START))
                                             : touch_of[0] && !chord_of[0] && (first_pad & BIT(BTN_R3));
    if (!menu_held)
        menu_since = 0;
    else if (!menu_since)
        menu_since = t_now;
    if (menu_held && t_now - menu_since >= MENU_HOLD_US[app.global.menu_hold])
    {
        open_menu = true;
        menu_since = 0;
        touch_used[0] = true; /* not a Select when it is let go */
    }
    if (open_menu)
    {
        menu_open();
        app_draw_game(255);
        return;
    }
    for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
    {
        if (i == 0 && (fast || back))
            pads[i].buttons &= ~(BIT(BTN_L2) | BIT(BTN_R2));
        if (i == 0 && touch) /* the combo buttons go to SwanStation, not the game */
            pads[i].buttons &= ~(chord ? combo_keys & ~BIT(BTN_R1) : combo_keys);
        pads[i].buttons = map_buttons(pads[i].buttons & ~BIT(BTN_MENU));
        if (chord_of[i])
            pads[i].buttons |= BIT(BTN_SELECT);
        if (touch_of[i] && !touch_used[i] && t_now - touch_since[i] >= select_hold_us)
            pads[i].buttons |= BIT(BTN_SELECT);
        if (select_frames[i] > 0)
        {
            pads[i].buttons |= BIT(BTN_SELECT);
            --select_frames[i];
        }
        /* A digital pad has no sticks: let the left stick drive the D-pad. With a
         * DualShock, Auto leaves that to the core (only while the game keeps it in
         * digital mode: AnalogDPadInDigitalMode in host.c), Always does it here. */
        bool map = app.settings.stick_dpad == STICK_DPAD_ALWAYS ||
                   (app.settings.stick_dpad == STICK_DPAD_AUTO && !app.settings.analog);
        if (map)
        {
            const int16_t dead = 16000;
            if (pads[i].lx < -dead) pads[i].buttons |= BIT(BTN_LEFT);
            if (pads[i].lx > dead) pads[i].buttons |= BIT(BTN_RIGHT);
            if (pads[i].ly < -dead) pads[i].buttons |= BIT(BTN_UP);
            if (pads[i].ly > dead) pads[i].buttons |= BIT(BTN_DOWN);
        }
    }
    controls_apply(pads, &app.settings, app.dt);
    host_set_pads(pads);

    /* Pace by the audio queue: the core's 59.94/50 Hz never matches the TV
     * exactly, so run 0-2 frames per refresh to keep ~3 frames of sound queued. */
    static uint64_t last_us, fps_window_start;
    static int fps_frames;
    static uint32_t fps_news_at, fps_repeats_at;
    uint64_t now = plat_ticks_us();
    size_t per_frame = (size_t)(host_sample_rate() / host_fps());
    size_t queued = plat_audio_queued_frames();
    int runs = 1;
    if (queued < per_frame * 2)
        runs = 2;
    else if (queued > per_frame * 5)
        runs = 0;
    /* Optimal frame pacing: one frame per refresh, the sound stretched (at most 0.5 %) to keep the queue where it is.
     * Only when the screen's rate and the core's are within 3 % (else 0 and 2 frames per refresh, as before). */
    double sound_ratio = 1.0;
    bool optimal_active = false;
    {
        const double display_hz = plat_display_hz(), core_hz = host_fps();
        if (app.settings.frame_pacing && !app.settings.framegen && !back && !fast && display_hz > 20.0 && core_hz > 20.0 &&
            display_hz > core_hz * 0.97 && display_hz < core_hz * 1.03)
        {
            runs = 1;
            optimal_active = true;
            const double target = (double)per_frame * 3.0;
            double error = ((double)queued - target) / target * 0.05; /* too full: play a little faster */
            sound_ratio = 1.0 + (error < -0.005 ? -0.005 : error > 0.005 ? 0.005 : error);
        }
    }
    plat_audio_set_ratio(sound_ratio);
    static bool rewind_hint;
    if (back)
    {
        /* step back two recorded moments a second... per refresh: smooth enough */
        static int pace;
        if (++pace >= 3)
        {
            pace = 0;
            if (play_rewind_step())
                host_run_frame(); /* draws the restored moment */
            else if (!app.settings.rewind && !rewind_hint)
            {
                app_toast("Turn on Rewind in Settings > System");
                rewind_hint = true;
            }
        }
        plat_audio_clear();
        runs = 0;
    }
    else if (fast && !ra_hardcore())
    {
        runs = 4;
        if (queued > per_frame * 3)
            plat_audio_clear(); /* keep up instead of queueing sound */
    }
    if (timer.running)
        timer.seconds += app.dt;
    static uint64_t emu_us, loops_since;
    static int emu_frames, loops;
    ++loops;
    /* what the pacing did, for comparing Optimal frame pacing on and off: refreshes by frames run, the sound's stretch */
    static int refreshes_by_runs[3], optimal_refreshes, pace_refreshes;
    static double ratio_min = 2.0, ratio_max = 0.0;
    if (!back && !fast)
    {
        ++refreshes_by_runs[runs < 0 ? 0 : runs > 2 ? 2 : runs];
        ++pace_refreshes;
        optimal_refreshes += optimal_active;
        if (optimal_active)
        {
            ratio_min = sound_ratio < ratio_min ? sound_ratio : ratio_min;
            ratio_max = sound_ratio > ratio_max ? sound_ratio : ratio_max;
        }
    }
    uint64_t emu_start = plat_ticks_us();
    /* Run-ahead: run the real frame, keep it, run 1 or 2 more to show where the
     * game is about to be (silently), then go back to the real one. What's on
     * screen answers the buttons that many frames sooner. */
    static void *ahead_state;
    static size_t ahead_size;
    /* run-ahead shows frames of a look-ahead that are not the game's own: Double frames needs the real ones */
    int ahead = !fast && !back && !app.settings.framegen ? app.settings.run_ahead : 0;
    if (ahead > 0 && runs > 0 && (!ahead_state || host_state_size() != ahead_size))
    {
        free(ahead_state); /* another game's states are another size */
        ahead_size = host_state_size();
        ahead_state = ahead_size ? malloc(ahead_size) : NULL;
    }
    for (int i = 0; i < runs; ++i)
    {
        host_run_frame();
        ra_frame();
        play_rewind_record();
        if (ahead > 0 && i == runs - 1 && ahead_state && host_state_size() == ahead_size &&
            host_serialize(ahead_state, ahead_size))
        {
            host_set_speculative(true);
            for (int k = 0; k < ahead; ++k)
                host_run_frame();
            host_unserialize(ahead_state, ahead_size);
            host_set_speculative(false);
        }
    }
    /* quick resume: a background save every three minutes */
    static float since_resume;
    since_resume += app.dt;
    if (since_resume > 180.0f)
    {
        since_resume = 0;
        play_save_resume(true);
    }
    /* auto-save: every 5, 10 or 15 minutes of play, the oldest of three slots */
    if (app.play_seconds < app.dt * 2)
        auto_since = 0; /* a game just started */
    app.play_seconds += app.dt;
    if (app.settings.autosave > 0 && !ra_hardcore())
    {
        auto_since += app.dt;
        if (auto_since > app.settings.autosave * 300.0f)
        {
            auto_since = 0;
            if (play_save_auto())
                auto_shown = 2.0f;
        }
    }
    emu_us += plat_ticks_us() - emu_start;
    emu_frames += runs;
    if (emu_frames >= 240)
    {
        SwanStationPS5_log("emu: %.1f ms per emulated frame, %.1f fps measured, the game's own frames %u new, %u repeated",
                  emu_us / 1000.0 / emu_frames, app.fps, host_frames_new(), host_frames_repeated());
        {
            double core_hz = host_fps();
            SwanStationPS5_log("speed: game at %.1f%% (%.1f of %.2f fps), screen %.2f Hz, %.1f presents a second, Generate frames %s, frames shown for each of the game's: %d",
                               core_hz > 1.0 ? app.fps * 100.0 / core_hz : 0.0, app.fps, core_hz, plat_display_hz(),
                               loops_since && now > loops_since ? loops * 1000000.0 / (double)(now - loops_since) : 0.0,
                               app.settings.framegen == 2 ? "x4" : app.settings.framegen ? "x2" : "off",
                               plat_framegen_times() > 1 ? plat_framegen_times() : 1);
        }
        emu_us = 0;
        emu_frames = 0;
        loops = 0;
        loops_since = now;
        if (pace_refreshes)
        {
            SwanStationPS5_log("pacing: %s, %d refreshes: %d with no frame, %d with one, %d with two or more; sound speed %.4f to %.4f",
                               optimal_refreshes * 2 > pace_refreshes ? "optimal" : "usual", pace_refreshes,
                               refreshes_by_runs[0], refreshes_by_runs[1], refreshes_by_runs[2],
                               optimal_refreshes ? ratio_min : 1.0, optimal_refreshes ? ratio_max : 1.0);
            refreshes_by_runs[0] = refreshes_by_runs[1] = refreshes_by_runs[2] = 0;
            optimal_refreshes = pace_refreshes = 0;
            ratio_min = 2.0;
            ratio_max = 0.0;
        }
    }
    fps_frames += runs;
    if (now - fps_window_start >= 1000000)
    {
        app.fps = fps_frames * 1000000.0f / (float)(now - fps_window_start);
        /* the game's own frames among them: a 30 fps game on a 60 fps core repeats every picture */
        uint32_t news = host_frames_new(), repeats = host_frames_repeated();
        uint32_t all_frames = news - fps_news_at + repeats - fps_repeats_at;
        app.real_fps = all_frames ? app.fps * (float)(news - fps_news_at) / (float)all_frames : app.fps;
        fps_news_at = news;
        fps_repeats_at = repeats;
        fps_frames = 0;
        fps_window_start = now;
    }
    if (runs == 0 && now - last_us < 4000)
        plat_sleep_us(1000);
    last_us = now;

    app_draw_game(255);
    if (auto_shown > 0)
    {
        /* a quiet sign that an auto-save was written */
        auto_shown -= app.dt;
        float a = auto_shown > 1.6f ? (2.0f - auto_shown) / 0.4f : auto_shown < 0.5f ? auto_shown / 0.5f : 1.0f;
        draw_rrect(plat_width() - 96, plat_height() - 96, 64, 64, 32, argb_alpha(0xb0000000u, a));
        icon_draw(ICON_DEVICE_FLOPPY, plat_width() - 82, plat_height() - 82, 36, argb_alpha(TH_TEXT, a));
    }
    if (app.settings.show_fps)
    {
        char f[64];
        double fg_hz = plat_framegen_hz();
        if (fg_hz > 1.0)
        {
            /* frames being generated: the frames the eye gets, each of the game's own and those generated, up to the screen's rate (25 -> 50) */
            double own = app.real_fps > 1.0f ? app.real_fps : app.fps;
            int times = plat_framegen_times();
            double shown = own * (times > 1 ? times : 2) < fg_hz ? own * (times > 1 ? times : 2) : fg_hz;
            snprintf(f, sizeof(f), "%.1f FPS (%s %.1f) | FG x%d", shown, tr("game"), own, times > 1 ? times : 2);
        }
        else if (app.real_fps > 0.0f && app.fps - app.real_fps >= 1.0f)
            snprintf(f, sizeof(f), "%.1f FPS (%s %.1f)", app.fps, tr("game"), app.real_fps);
        else
            snprintf(f, sizeof(f), "%.1f FPS", app.fps);
        float w = text_width(26, FONT_BOLD, f) + 40;
        draw_rrect(24, 24, w, 52, 26, 0xb0000000u);
        text_draw(44, 36, 26, FONT_BOLD, TH_GOOD, ALIGN_LEFT, f);
    }
    if (fast || back)
    {
        int icon = back ? ICON_HISTORY : ICON_PLAYER_TRACK_NEXT;
        draw_rrect(plat_width() - 110, 30, 80, 60, 30, 0xb0000000u);
        icon_draw(icon, plat_width() - 92, 38, 44, TH_TEXT);
    }
    app_draw_toast();
}

/* Play time: counted while a game runs (not while its menu is open). */
static void count_play_time(void)
{
    static float seconds;
    static uint64_t saved_at;
    if (!app.game || app.screen != SCREEN_GAME)
        return;
    seconds += app.dt;
    if (seconds < 1.0f)
        return;
    GameStats *st = game_is_bios(app.game) ? NULL : stats_get(app.game->id);
    if (st)
        st->seconds += (uint32_t)seconds;
    seconds -= (float)(uint32_t)seconds;
    uint64_t now = plat_ticks_us();
    if (now - saved_at > 60000000) /* write it down once a minute */
    {
        int unlocked, total;
        if (st && ra_game_progress(&unlocked, &total))
        {
            st->ach_unlocked = unlocked;
            st->ach_total = total;
        }
        stats_save();
        saved_at = now;
    }
}

/* ---------------------------------------------------------------- main */

#if defined(__PROSPERO__)
static int stop_services(void *done)
{
    remote_update(false);
    ra_shutdown();
    covers_stop();
    SDL_AtomicSet((SDL_atomic_t *)done, 1);
    return 0;
}
#endif

#if defined(SwanStationXbox_UWP)
/* uwp_app.cpp runs this on the UI thread, once the window is active */
int swanstation_main(int argc, char **argv)
#else
int main(int argc, char **argv)
#endif
{
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc)
            str_copy(launch_rom, sizeof(launch_rom), argv[++i]);
        else if (strcmp(argv[i], "--exit-after-game") == 0)
            exit_after_game = true;
    }
    /* First thing: proves the loader accepted the title and main() runs. (Turned off: the notice showed on screen.)
    plat_notify("SwanStation " SwanStationPS5_VERSION " starting...");
    */
    char root[SwanStationPS5_PATH_MAX];
    plat_default_root(root, sizeof(root));
    config_paths(&app.paths, root);

    /* Unlock before SDL starts any thread: the HEN changes this process's
     * credentials, which Porpoise did not survive with threads running. */
    app.sandboxed = !plat_prepare_storage(app.sandbox_reason, sizeof(app.sandbox_reason));
    /* An empty file called reset-settings, beside the settings file, asks for clean settings by hand: removed before
     * anything reads it (and the signed-in profile's once the profiles are known). */
    char reset_marker[SwanStationPS5_PATH_MAX];
    path_join(reset_marker, sizeof(reset_marker), app.paths.root, "reset-settings");
    const bool settings_reset = path_exists(reset_marker);
    if (settings_reset)
        remove(app.paths.config);
    /* A new version after settings written by another: the first screen asks whether to start from its factory
     * settings (the files of versions before 1.1.4 carry no version). */
    char saved_version[32] = "";
    const bool version_changed =
        !settings_reset && path_exists(app.paths.config) &&
        (!config_peek_string(app.paths.config, "version", saved_version, sizeof(saved_version)) ||
         strcmp(saved_version, SwanStationPS5_VERSION) != 0);
    /* frame interpolation wants the 120 Hz mode, chosen when the screen opens */
    char video_out[8] = "";
    config_peek_string(app.paths.config, "video_out", video_out, sizeof(video_out));
    const bool high_refresh = atoi(video_out) == 1;
    plat_want_high_refresh(high_refresh);
    plat_want_output(2); /* always 4K (1080p when the TV lacks the mode) */

    /* the log first: plat_init opens the GPU, and a driver that crashes there must leave its lines behind */
    make_dirs(app.paths.root);
    make_dirs(app.paths.logs);
    char log_path[SwanStationPS5_PATH_MAX];
    path_join(log_path, sizeof(log_path), app.paths.logs, "SwanStationPS5.log");
    SwanStationPS5_log_open(log_path);
    start_mark();
    if (!plat_init())
    {
        char msg[300];
        snprintf(msg, sizeof(msg), "SwanStation: %s", plat_init_error());
        plat_notify(msg);
        for (;;)
            plat_sleep_us(1000000);
    }
    start_mark(); /* the screen is open */
    if (!text_init())
        plat_notify("SwanStation: interface font missing from assets/fonts");
    /* The first picture, before the slow part (the drives, the library): a console shows black until one is drawn */
    plat_begin_frame(0xff000000u);
    text_draw(plat_width() * 0.5f, plat_height() * 0.5f - 40, 56, FONT_BOLD, 0xffffffffu, ALIGN_CENTER, SwanStationPS5_TITLE);
    text_draw(plat_width() * 0.5f, plat_height() * 0.5f + 40, 30, FONT_REGULAR, 0xffa0a0a0u, ALIGN_CENTER, "Loading...");
    plat_end_frame();
    start_mark(); /* the font and that picture */

    /* Sandboxed or not, files in /data open and save; only listing differs. */
    const char *dirs[] = {app.paths.root,   app.paths.games,  app.paths.bios,
                          app.paths.saves,  app.paths.states, app.paths.cheats,
                          app.paths.covers, app.paths.logs,   app.paths.cache};
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i)
        make_dirs(dirs[i]);
    bios_use_drive_dir();
    start_mark(); /* ... the BIOS folder is chosen (the font scan before it counts here too) */
    bios_choices_scan(app.paths.bios); /* before the settings are read: the BIOS options list the folder's files */
    char probe[SwanStationPS5_PATH_MAX];
    path_join(probe, sizeof(probe), app.paths.root, ".write-test");
    FILE *pf = fopen(probe, "w");
    if (pf)
    {
        fclose(pf);
        remove(probe);
    }
    else
        snprintf(app.storage_error, sizeof(app.storage_error), "Windows error %u|%s (%s)", uwp_last_error(), app.paths.root,
                 path_is_dir(app.paths.root) ? "the folder is there, writing fails" : "the folder is not there");
    ps5_crash_install(log_path);
#if defined(__PROSPERO__)
    /* the log's stderr lines go to klog: nc <Xbox IP> 3232 shows them live */
    extern int ps5_klog_capture_stderr(const char *prefix);
    ps5_klog_capture_stderr("");
#endif
    /* the drives the system reports now: the games are looked for on every one */
    char found_drives[256];
    app.drive_count = drives_list(found_drives, sizeof(found_drives), -1);
    for (int i = 0; i < app.drive_count && i < DRIVE_COUNT; ++i)
        drives_list(app.drive_text[i], sizeof(app.drive_text[i]), i);
    SwanStationPS5_log("drives found: %s", app.drive_count ? found_drives : "none");
    start_mark(); /* the drives are listed */
    app_started_us = plat_ticks_us();
    SwanStationPS5_log("SwanStation %s starting, data root %s", SwanStationPS5_VERSION, app.paths.root);
    SwanStationPS5_log("screen: %s", plat_screen_info());
#if defined(__PROSPERO__)
    extern size_t ps5_heap_size_mb(void);
    SwanStationPS5_log("heap: %zu MB of address range%s", ps5_heap_size_mb(),
              ps5_heap_size_mb() ? "" : " (unavailable: using the system heap)");
#endif
    SwanStationPS5_log(app.sandboxed ? "storage: sandboxed (%s)" : "storage: unlocked%s",
              app.sandboxed ? app.sandbox_reason : "");
    SwanStationPS5_log("storage probe before unlock: %s", plat_sandbox_probe());

    if (!app.storage_error[0])
        profiles_startup(); /* whose saves, settings and stats */
    if (settings_reset)
    {
        remove(app.paths.config);
        remove(reset_marker);
        remove_game_settings();
        SwanStationPS5_log("settings: clean start, asked for by hand");
    }
    if (version_changed)
    {
        SwanStationPS5_log("settings: saved by %s, this is %s: asking", saved_version[0] ? saved_version : "an older version",
                           SwanStationPS5_VERSION);
        reset_question_open();
    }
    config_load(&app.global, app.paths.config);
    app.settings = app.global;
    i18n_set(app.global.language);
    theme_apply(app.global.theme);
    stats_load(app.paths.user);
    plat_audio_open(UI_RATE);
    sfx_init(UI_RATE);
    sfx_configure(app.global.ui_sound, (app.global.ui_volume + 1) * 25);
    if (!app.storage_error[0])
    {
        ra_init(&app.paths);
        gamedb_start(); /* DuckStation's game database, downloaded once */
    }
    remote_update(app.global.remote && !app.storage_error[0]);
    shelf_init(app.global.last_game);
    if (!app.storage_error[0])
        app_rescan();
    start_mark(); /* the library is read */
    {
        char took[200];
        app_startup_report(took, sizeof(took));
        SwanStationPS5_log("startup: %s", took);
    }
    shelf_select_game(app.global.last_game);
    if (launch_rom[0] && !app.storage_error[0])
    {
        const int index = game_for_launch_path(launch_rom);
        SwanStationPS5_log("forwarder: --rom %s -> %s%s", launch_rom,
                           index >= 0 ? app.library.games[index].path : "not in the library",
                           exit_after_game ? " (exit after the game)" : "");
        if (index >= 0)
        {
            shelf_select_game(index);
            app_start_game(index, false);
            started_from_argument = app.screen == SCREEN_GAME;
        }
    }
    SwanStationPS5_log("build: %s %s", __DATE__, __TIME__);
    /* (Turned off: the notice showed on screen.)
    plat_notify("SwanStation ready");
    */

    bool quit = false;
    PadState pads[SwanStationPS5_MAX_PADS];
    uint64_t last = plat_ticks_us();
    const uint64_t opened = last;
    while (!quit)
    {
        uint64_t now = plat_ticks_us();
        app.dt = (float)(now - last) / 1e6f;
        if (app.dt > 0.1f)
            app.dt = 0.1f;
        last = now;

        plat_poll(pads, &quit);
        if (app.quit_requested)
            quit = true;
        memcpy(app.pads, pads, sizeof(app.pads));
        uint32_t pressed = nav_pressed(pads);
        if (!question_input(&pressed) && !sbi_check_input(&pressed))
            message_box_input(&pressed);
        /* the shelf and settings paint a full-screen backdrop: no clear needed */
        bool backdrop = app.screen == SCREEN_LIBRARY || (app.screen == SCREEN_SETTINGS && !app.game);
        plat_begin_frame(backdrop ? 0 : 0xff000000u);
        static const char *const screen_names[] = {"library", "game",         "menu",
                                                   "settings", "cheats", "achievements",
                                                   "memory cards", "library stats", "manual",
                                                   "cheat search", "guide", "profile", "sign in"};
        ps5_crash_step(screen_names[app.screen]);
        static enum Screen last_screen = SCREEN_LIBRARY;
        enum Screen this_screen = app.screen;
        switch (app.screen)
        {
        case SCREEN_LIBRARY: shelf_screen(pressed); break;
        case SCREEN_GAME:
            if (last_screen != SCREEN_GAME)
            {
                int connected = 0;
                uint32_t held = 0;
                for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                {
                    held_from_menu |= pads[i].buttons;
                    held |= pads[i].buttons;
                    connected += pads[i].connected;
                }
                if (last_screen != SCREEN_MENU && last_screen != SCREEN_SETTINGS)
                {
                    input_reported = false;
                    input_start_us = plat_ticks_us();
                    SwanStationPS5_log("input: game screen, %d pad(s) connected, buttons held at the start 0x%x", connected, held);
                }
            }
            game_screen(pads);
            break;
        case SCREEN_MENU: menu_screen(pressed); break;
        case SCREEN_SETTINGS:
            if (app.game && !app.settings_preview && (pressed & BIT(BTN_MENU)) && settings_preview_allowed())
            {
                app.settings_preview = true;
                app.settings_preview_time = 0;
                if (app.settings.manual_crop)
                    manual_crop_fix();
            }
            else if (app.settings_preview && (pressed & BIT(BTN_MENU)))
            {
                app.settings_preview = false;
                if (app.settings.manual_crop)
                    app_save_settings();
                sfx_play(SFX_BACK);
            }
            if (app.settings_preview && app.game)
            {
                app.settings_preview_time += app.dt;
                /* the game keeps running, without the pad: its picture, with the settings as they are now */
                PadState idle[SwanStationPS5_MAX_PADS];
                memcpy(idle, pads, sizeof(idle));
                for (int i = 0; i < SwanStationPS5_MAX_PADS; ++i)
                {
                    idle[i].buttons = 0;
                    idle[i].lx = idle[i].ly = idle[i].rx = idle[i].ry = 0;
                    idle[i].l2 = idle[i].r2 = 0;
                }
                if (app.settings.manual_crop)
                {
                    const int up = (pressed & BIT(BTN_UP)) != 0, down = (pressed & BIT(BTN_DOWN)) != 0,
                              left = (pressed & BIT(BTN_LEFT)) != 0, right = (pressed & BIT(BTN_RIGHT)) != 0;
                    if (pressed & BIT(BTN_SQUARE))
                        manual_crop_edit(0, 0, true);
                    else
                        manual_crop_edit(right - left, 2 * (down - up), false);
                }
                game_screen(idle);
                if (app.settings.manual_crop)
                {
                    const float t = app.settings_preview_time, in = t < 0.25f ? t / 0.25f : 1.0f;
                    const float w = 900, h = 230, x = (plat_width() - w) * 0.5f, y = plat_height() - h - 36 + (1.0f - in) * 60.0f;
                    char values[96], ratio[24];
                    draw_rrect(x, y, w, h, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), in));
                    text_draw_fit(x + 36, y + 22, 26, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 72,
                                  tr("Manual crop"));
                    text_draw_fit(x + 36, y + 66, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Left} {Right}  Stretch or narrow the picture, keeping its aspect ratio"));
                    text_draw_fit(x + 36, y + 102, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Up} {Down}  Move the picture up or down"));
                    text_draw_fit(x + 36, y + 138, 24, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT, w - 72,
                                  tr("{Square}  Back to the original area"));
                    crop_ratio_text(ratio, sizeof(ratio),
                                    (play_widescreen_active() ? 16.0f / 9.0f : host_aspect()) * app.settings.mc_w /
                                        app.settings.mc_h);
                    snprintf(values, sizeof(values), "%d%%   %+d   %s", app.settings.mc_w, app.settings.mc_y, ratio);
                    text_draw_fit(x + 36, y + 180, 22, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 72, values);
                }
                {
                    const float t = app.settings_preview_time, in = t < 0.25f ? t / 0.25f : 1.0f,
                                slide = (1.0f - in) * (1.0f - in) * 80.0f;
                    const float w = 660, h = 116, x = (plat_width() - w) * 0.5f, y = -slide;
                    draw_rrect(x, y - TH_RADIUS, w, h + TH_RADIUS, TH_RADIUS, argb_alpha(TH_CARD_A(0xf0), in));
                    icon_draw(ICON_DEVICE_TV, x + 30, y + 30, 56, argb_alpha(TH_TEXT, in));
                    text_draw_fit(x + 120, y + 20, 30, FONT_BOLD, argb_alpha(TH_TEXT, in), ALIGN_LEFT, w - 140,
                                  tr("Preview"));
                    text_draw(x + 120, y + 66, 22, FONT_REGULAR, argb_alpha(TH_TEXT_DIM, in), ALIGN_LEFT,
                              tr("Press {View button} to return to settings..."));
                }
                app_draw_toast();
            }
            else
                settings_screen(pressed);
            break;
        case SCREEN_CHEATS: cheats_screen(pressed); break;
        case SCREEN_ACHIEVEMENTS: achievements_screen(pressed); break;
        case SCREEN_MEMCARDS: memcards_screen(pressed); break;
        case SCREEN_STATS: library_stats_screen(pressed); break;
        case SCREEN_MANUAL: manual_screen(pressed); break;
        case SCREEN_CHEAT_SEARCH: cheat_search_screen(pressed); break;
        case SCREEN_GUIDE: guide_screen(pressed); break;
        case SCREEN_PROFILE: profile_screen(pressed); break;
        case SCREEN_RA_LOGIN: login_screen(pressed); break;
        case SCREEN_TEXT_INPUT: textinput_screen(pressed); break;
        default: break;
        }
        last_screen = this_screen;
        count_play_time();
        remote_frame();
        /* a game's cheat file arrived from libretro-database */
        {
            const bool nostalgia = app.game && app.global.nostalgia && app.screen == SCREEN_GAME;
            sfx_silence(nostalgia);
        }
        if (cheats_fetch_finished() && app.game && !ra_hardcore() &&
            cheats_load(&app.cheats, app.game, app.paths.cheats))
        {
            cheats_apply(&app.cheats);
            play_widescreen(); /* the file may hold the game's widescreen code */
        }
        if (app.screen != SCREEN_SETTINGS)
            app.settings_preview = false;
        if (app.screen != SCREEN_GAME)
            ra_idle();
        draw_achievement();
        sbi_check_draw();
        message_box_draw();
        question_draw();
        toast_render();
        plat_end_frame();
    }

    /* Only the desktop build gets here; the Xbox shell closes the title. */
    ps5_crash_step("shutdown");
    SwanStationPS5_log("shutdown: leaving the main loop");
    if (app.game)
        app_stop_game();
    config_save(&app.global, app.paths.config);
    stats_save();
    SwanStationPS5_log("shutdown: settings and stats saved");
#if !defined(__PROSPERO__)
    /* the console takes the title down with its threads and sockets: waiting for them (a cover download, a
     * RetroAchievements request, the phone's server) could leave the screen frozen for up to half a minute */
    remote_update(false);
    ra_shutdown();
    SwanStationPS5_log("shutdown: network and RetroAchievements stopped");
    covers_stop();
    plat_audio_close();
    icons_shutdown();
    text_shutdown();
    library_free(&app.library);
    SwanStationPS5_log("shutdown: interface freed");
    plat_shutdown();
    SwanStationPS5_log("shutdown: done");
#endif
#if defined(__PROSPERO__)
    /* The system closes the title: exit()/_exit() raise SIGSYS on the console. As ProsperoStore does it: LoadExec
     * "exit" ends the title normally. */
    extern int sceSystemServiceLoadExec(const char *path, const char **arguments);
    /* extern int sceSystemServiceGetAppIdOfRunningBigApp(void);
     * extern int sceSystemServiceKillApp(int app_id, int how, int reason, int core_dump); */
    SwanStationPS5_log("shutdown: closing the title");
    {
        static SDL_atomic_t services_done;
        SDL_Thread *stopper = SDL_CreateThread(stop_services, "stop-services", &services_done);
        for (int waited = 0; stopper && !SDL_AtomicGet(&services_done) && waited < 2000; waited += 10)
            plat_sleep_us(10000);
        if (stopper && SDL_AtomicGet(&services_done))
        {
            SDL_WaitThread(stopper, NULL);
            SwanStationPS5_log("shutdown: network and RetroAchievements stopped");
        }
        else
        {
            if (stopper)
                SDL_DetachThread(stopper);
            SwanStationPS5_log("shutdown: services did not stop in 2 s, closing anyway");
        }
    }
    icons_shutdown();
    text_shutdown();
    library_free(&app.library);
    SwanStationPS5_log("shutdown: interface freed");
    plat_release();
    fflush(NULL);
    for (;;)
    {
        /* sceSystemServiceKillApp(sceSystemServiceGetAppIdOfRunningBigApp(), -1, 0, 0); */
        int rc = sceSystemServiceLoadExec("exit", NULL);
        SwanStationPS5_log("shutdown: LoadExec exit refused rc=0x%x", (unsigned)rc);
        plat_sleep_us(1000000); /* until the shell takes the title down; asked again if it did not */
    }
#endif
    return 0;
}
