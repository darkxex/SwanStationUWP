/*
 * SwanStation - imports the PS1 disc in the USB CD drive. See discimport.h.
 * The read loop follows ps-disc-detect (Al-Azif, GPL-3.0): READ TOC, then READ CD 16 sectors at a time into one
 * .bin of 2352-byte sectors, and a .cue with each track's mode. Unreadable sectors are retried, then zero-filled
 * and logged; too many abort the import. Files are written as .part and renamed only when everything went well.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "discimport.h"

#include "discplay.h"

#include "cdscsi.h"
#include "disc.h"
#include "covers.h"
#include "gamedb.h"
#include "platform/platform.h"

#include <SDL2/SDL.h>
#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define P(...) SwanStationPS5_log("import: " __VA_ARGS__)

#define SECTOR 2352
#define BLOCK 16        /* sectors per READ CD (37 632 bytes: under the 64 KiB the drive's pass takes) */
#define MAX_TRACKS 99
#define MAX_BAD 2000    /* unreadable sectors before the import gives up */
#define MAX_ERROR_SECONDS 20  /* time spent in reads that FAIL, added up, before it gives up: each one can take 20 s or more */
#define STALL_SECONDS 30      /* with no sector read for this long, it gives up: the drive is stuck */
#define STALL_MS 30000        /* with no progress on the bar for this long, the screen gives up */
#define TRIES 3
#define READ_TIMEOUT_MS 5000  /* a good read takes milliseconds; a failing one is cut here, not left to the drive's own retries */
#define SPEED_AFTER_ERROR 706 /* kB/s, 4x: damaged areas read better slowly */
typedef struct
{
    int number;
    bool data;
    uint32_t lba;
} Track;

static struct
{
    SDL_atomic_t state, permille, named, bad, abort, alive, last_change, dumping, total;
    char games[SwanStationPS5_PATH_MAX];
    char title[96], serial[16], key[24];
    uint8_t *cover_px;
    int cover_w, cover_h;
    SDL_atomic_t cover_ready, cover_gen, cover_none;
    struct PlatTexture *cover_tex;
} J;

DiscImportState discimport_state(void)
{
    return (DiscImportState)SDL_AtomicGet(&J.state);
}

static void cover_free(void)
{
    if (J.cover_tex)
        plat_texture_free(J.cover_tex);
    J.cover_tex = NULL;
}

void discimport_ack(void)
{
    if (discimport_state() > DI_RUNNING)
    {
        SDL_AtomicSet(&J.state, DI_IDLE);
        cover_free();
    }
}

const char *discimport_serial(void)
{
    return SDL_AtomicGet(&J.named) ? J.serial : "";
}

int discimport_total_sectors(void)
{
    return SDL_AtomicGet(&J.total);
}

bool discimport_dumping(void)
{
    return SDL_AtomicGet(&J.dumping) != 0;
}

bool discimport_cover_none(void)
{
    return SDL_AtomicGet(&J.cover_none) != 0;
}

const char *discimport_key(void)
{
    return SDL_AtomicGet(&J.named) ? J.key : "";
}

struct PlatTexture *discimport_cover(void)
{
    if (!J.cover_tex && SDL_AtomicGet(&J.cover_ready) && J.cover_px)
    {
        J.cover_tex = plat_texture_create(J.cover_px, J.cover_w, J.cover_h, true);
        free(J.cover_px);
        J.cover_px = NULL;
    }
    return J.cover_tex;
}

static void touch(void)
{
    SDL_AtomicSet(&J.last_change, (int)SDL_GetTicks());
}

bool discimport_check_stall(void)
{
    if (discimport_state() != DI_RUNNING || SDL_GetTicks() - (uint32_t)SDL_AtomicGet(&J.last_change) < STALL_MS)
        return false;
    SDL_AtomicSet(&J.abort, 1);
    SDL_bool changed = SDL_AtomicCAS(&J.state, DI_RUNNING, DI_STALLED);
    if (changed)
        P("no progress for %d s: given up", STALL_MS / 1000);
    return changed == SDL_TRUE;
}

void discimport_cancel(void)
{
    SDL_AtomicSet(&J.abort, 1);
    if (SDL_AtomicCAS(&J.state, DI_RUNNING, DI_CANCELLED))
        P("cancelled by the player");
}

const char *discimport_games_dir(void)
{
    return J.games;
}

int discimport_bad(void)
{
    return SDL_AtomicGet(&J.bad);
}

void discimport_cleanup(const char *games_dir)
{
    DIR *d = opendir(games_dir);
    if (!d)
        return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (e->d_name[0] == '.')
            continue;
        char sub[SwanStationPS5_PATH_MAX];
        path_join(sub, sizeof(sub), games_dir, e->d_name);
        DIR *s = opendir(sub);
        if (!s)
            continue;
        struct dirent *f;
        while ((f = readdir(s)))
        {
            size_t n = strlen(f->d_name);
            if (n > 5 && strcmp(f->d_name + n - 5, ".part") == 0)
            {
                char part[SwanStationPS5_PATH_MAX];
                path_join(part, sizeof(part), sub, f->d_name);
                P("removing what an interrupted import left: %s", part);
                remove(part);
            }
        }
        closedir(s);
        rmdir(sub); /* only if it is empty now */
    }
    closedir(d);
}

int discimport_permille(void)
{
    return SDL_AtomicGet(&J.permille);
}

const char *discimport_title(void)
{
    return SDL_AtomicGet(&J.named) ? J.title : "";
}

static uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t n)
{
    crc = ~crc;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}

/* READ CD: `count` sectors from `lba`; flags 0xF8 = sync, header, subheader, data and EDC/ECC (data tracks),
 * 0x10 = the 2352 bytes of user data (audio) */
static int read_cd(const CdPass *p, uint32_t lba, uint32_t count, uint8_t flags, uint8_t *buf, uint32_t *sense)
{
    uint8_t cdb[12] = {0xbe, 0, (uint8_t)(lba >> 24), (uint8_t)(lba >> 16), (uint8_t)(lba >> 8), (uint8_t)lba,
                       (uint8_t)(count >> 16), (uint8_t)(count >> 8), (uint8_t)count, flags, 0, 0};
    return cdscsi_cmd(p, cdb, 12, buf, count * SECTOR, READ_TIMEOUT_MS, sense, NULL);
}

/* the 2048 data bytes of mode 2 form 1 sectors start at offset 24 */
static bool read_user(void *ctx, uint32_t lba, uint8_t *out, size_t length)
{
    static uint8_t raw[SECTOR];
    for (size_t done = 0; done < length; done += 2048, ++lba)
    {
        bool ok = false;
        for (int t = 0; t < TRIES && !ok; ++t)
            ok = read_cd(ctx, lba, 1, 0xf8, raw, NULL) == 0;
        if (!ok)
            return false;
        size_t n = length - done < 2048 ? length - done : 2048;
        memcpy(out + done, raw + 24, n);
    }
    return true;
}

static void safe_name(char *dst, size_t size, const char *src)
{
    size_t n = 0;
    for (; *src && n + 1 < size; ++src)
    {
        unsigned char c = (unsigned char)*src;
        dst[n++] = (isalnum(c) || c == ' ' || c == '-' || c == '_' || c == '.' || c == '\'' || c == '(' || c == ')' ||
                    c >= 0x80)
                       ? (char)c
                       : '_';
    }
    while (n && (dst[n - 1] == ' ' || dst[n - 1] == '.'))
        --n;
    dst[n] = '\0';
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* The first drive with a disc whose table of contents can be read. */
static DiscImportState find_disc(CdPass *p, uint8_t *toc, uint32_t toc_size, uint32_t *toc_got)
{
    DiscImportState worst = DI_NO_DRIVE;
    for (unsigned unit = 0; unit < 8; ++unit)
    {
        char dev[24];
        snprintf(dev, sizeof(dev), "/dev/cd%u", unit);
        if (!exists(dev) || !cdscsi_open(unit, p))
            continue;
        worst = DI_NO_DISC;
        static const uint8_t tur[6] = {0};
        uint32_t sense = 0;
        int rc = -1;
        for (int i = 0; i < 40; ++i) /* a USB drive takes a few seconds to spin up */
        {
            rc = cdscsi_cmd(p, tur, 6, NULL, 0, 5000, &sense, NULL);
            if (rc == 0 || (sense != CDSENSE_BECOMING_READY && (sense >> 16) != 6))
                break;
            SDL_Delay(250);
        }
        static const uint8_t read_toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0x08, 0x00, 0};
        if (rc == 0 && cdscsi_cmd(p, read_toc, 10, toc, toc_size, 10000, &sense, toc_got) == 0)
        {
            P("drive cd%u has a disc", unit);
            return DI_RUNNING;
        }
        P("cd%u: no usable disc (sense %06x)", unit, sense);
        cdscsi_close(p);
    }
    return worst;
}

/* Reads the volume descriptor and SYSTEM.CNF of the first data track. False if there is no ISO 9660 volume. */
static bool identify(const CdPass *pass, const uint8_t *toc, uint32_t toc_got, uint32_t first_lba, DiscId *id,
                     uint32_t *sense)
{
    static uint8_t raw[SECTOR];
    bool has_volume = false;
    memset(id, 0, sizeof(*id));
    for (int t = 0; t < TRIES && !has_volume; ++t)
        has_volume = read_cd(pass, first_lba + 16, 1, 0xf8, raw, sense) == 0 && memcmp(raw + 25, "CD001", 5) == 0;
    if (!has_volume)
        return false;
    memcpy(id->label, raw + 24 + 40, 32);
    id->label[32] = '\0';
    for (int n = (int)strlen(id->label); n > 0 && id->label[n - 1] == ' '; --n)
        id->label[n - 1] = '\0';
    id->fingerprint = crc32_update(crc32_update(0, toc, toc_got), raw + 24, 2048);
    bool has_serial = disc_serial_from_reader(read_user, (void *)pass, id->serial, sizeof(id->serial));
    const GameInfo *info = has_serial ? gamedb_get(id->serial) : NULL;
    if (!has_serial)
        id->serial[0] = '\0';
    if (info && info->name[0])
        str_copy(id->title, sizeof(id->title), info->name);
    else if (id->label[0])
        str_copy(id->title, sizeof(id->title), id->label);
    else
        str_copy(id->title, sizeof(id->title), "Untitled disc");
    if (has_serial)
        str_copy(id->key, sizeof(id->key), id->serial);
    else
        snprintf(id->key, sizeof(id->key), "%08X", id->fingerprint);
    return true;
}

/* "Parasite Eve (Disc 2)" -> 2, and `stem` "Parasite Eve"; 0 when the title has no "(Disc N)". */
static int disc_number(const char *title, char *stem, size_t size)
{
    const char *tag = strstr(title, "(Disc ");
    if (!tag)
        return 0;
    char *end;
    long n = strtol(tag + 6, &end, 10);
    if (end == tag + 6 || *end != ')' || n < 1 || n > 9)
        return 0;
    size_t before = (size_t)(tag - title);
    while (before && title[before - 1] == ' ')
        --before;
    const char *after = end + 1;
    while (*after == ' ')
        ++after;
    snprintf(stem, size, "%.*s%s%s", (int)before, title, (before && *after) ? " " : "", after);
    return (int)n;
}

/* The folder of an earlier disc of the same game ("<stem> [<serial>]") in the games folder, or "". */
static void find_game_folder(const char *games, const char *stem, char *out, size_t size)
{
    out[0] = '\0';
    DIR *d = opendir(games);
    if (!d)
        return;
    char prefix[200];
    snprintf(prefix, sizeof(prefix), "%s [", stem);
    struct dirent *e;
    while ((e = readdir(d)))
    {
        size_t n = strlen(e->d_name);
        if (strncmp(e->d_name, prefix, strlen(prefix)) == 0 && n > 2 && e->d_name[n - 1] == ']')
        {
            str_copy(out, size, e->d_name);
            break;
        }
    }
    closedir(d);
}

/* The tracks and the lead-out from a table of contents. */
static int parse_toc(const uint8_t *toc, uint32_t toc_got, Track *tracks, uint32_t *leadout)
{
    int ntracks = 0, entries = toc_got >= 4 ? ((int)toc_got - 4) / 8 : 0;
    *leadout = 0;
    for (int i = 0; i < entries; ++i)
    {
        const uint8_t *e = toc + 4 + 8 * i;
        uint32_t lba = (uint32_t)e[4] << 24 | e[5] << 16 | e[6] << 8 | e[7];
        if (e[2] == 0xaa)
            *leadout = lba;
        else if (ntracks < MAX_TRACKS)
            tracks[ntracks++] = (Track){e[2], (e[1] & 0x04) != 0, lba};
    }
    return ntracks;
}

typedef struct
{
    int gen;
    char serial[16], title[96];
} CoverJob;

static int cover_thread(void *arg)
{
    CoverJob *job = arg;
    int w = 0, h = 0;
    uint8_t *px = job->serial[0] ? covers_for_serial(job->serial, &w, &h) : NULL;
    if (SDL_AtomicGet(&J.cover_gen) != job->gen) /* another import began meanwhile */
        free(px);
    else if (px)
    {
        J.cover_px = px;
        J.cover_w = w;
        J.cover_h = h;
        SDL_AtomicSet(&J.cover_ready, 1);
    }
    else /* no serial (homebrew), no cover for it, or no network: the screen shows the shelf's "no cover" picture */
        SDL_AtomicSet(&J.cover_none, 1);
    free(job);
    return 0;
}

static void finish(DiscImportState result)
{
    SDL_AtomicSet(&J.permille, result == DI_OK || result == DI_OK_INCOMPLETE ? 1000 : 0);
    SDL_AtomicCAS(&J.state, DI_RUNNING, result); /* not if the screen gave up on it meanwhile */
}

static void import_run(void)
{
    CdPass pass;
    uint8_t toc[2048];
    uint32_t toc_got = 0;
    memset(toc, 0, sizeof(toc));
    DiscImportState found = find_disc(&pass, toc, sizeof(toc), &toc_got);
    touch();
    if (found != DI_RUNNING)
    {
        finish(found);
        return;
    }

    /* the tracks and the lead-out, from the table of contents */
    Track tracks[MAX_TRACKS];
    uint32_t leadout = 0;
    int ntracks = parse_toc(toc, toc_got, tracks, &leadout);
    if (!ntracks || !leadout || leadout <= tracks[0].lba)
    {
        P("the table of contents is not usable");
        cdscsi_close(&pass);
        finish(DI_NOT_PS1);
        return;
    }
    if (!tracks[0].data)
    {
        P("track 1 is audio: not a PS1 game disc");
        cdscsi_close(&pass);
        finish(DI_NOT_PS1);
        return;
    }
    P("%d tracks, lead-out at %u", ntracks, leadout);

    /* A USB drive spins a data CD slowly (quietly) until it is asked for speed: SET CD SPEED (BB) with "as fast as
     * possible". (SET STREAMING, B6, was also tried: the drive takes it, and the copy takes exactly as long.) */
    {
        uint8_t set_speed[12] = {0xbb, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
        uint32_t sense_speed = 0;
        int r = cdscsi_cmd(&pass, set_speed, 12, NULL, 0, 5000, &sense_speed, NULL);
        P("speed: SET CD SPEED %s (sense %06x)", r == 0 ? "ok" : "failed", sense_speed);
    }

    /* who it is: the serial from SYSTEM.CNF, else the volume label and a fingerprint of the table of contents */
    uint8_t raw[SECTOR * BLOCK];
    uint32_t sense = 0;
    DiscId id;
    if (!identify(&pass, toc, toc_got, tracks[0].lba, &id, &sense))
    {
        P("no ISO 9660 volume at sector 16 (sense %06x)", sense);
        cdscsi_close(&pass);
        finish(CDSENSE_IS_NO_MEDIUM(sense) ? DI_DRIVE_LOST : DI_NOT_PS1);
        return;
    }
    const char *serial = id.serial, *label = id.label, *title = id.title;
    char folder[160], dirname[160], safe[96];
    safe_name(safe, sizeof(safe), title);
    if (!safe[0])
        str_copy(safe, sizeof(safe), "Disc");
    snprintf(folder, sizeof(folder), "%s [%s]", safe, id.key); /* the files' name */
    str_copy(dirname, sizeof(dirname), folder);               /* and the folder's */
    bool multi = false;
    {
        /* a disc of a game with several: they all go in one folder, which the library turns into one game (.m3u) */
        char stem[96], safe_stem[96];
        if (serial[0] && disc_number(title, stem, sizeof(stem)))
        {
            safe_name(safe_stem, sizeof(safe_stem), stem);
            char existing[160];
            find_game_folder(J.games, safe_stem, existing, sizeof(existing));
            if (existing[0])
                str_copy(dirname, sizeof(dirname), existing);
            else
                snprintf(dirname, sizeof(dirname), "%s [%s]", safe_stem, serial);
            multi = true;
        }
    }
    str_copy(J.title, sizeof(J.title), title);
    str_copy(J.serial, sizeof(J.serial), serial);
    str_copy(J.key, sizeof(J.key), id.key);
    SDL_AtomicSet(&J.named, 1);
    touch();
    P("disc: '%s', serial '%s', label '%s', files '%s' in '%s'", title, serial, label, folder, dirname);
    {
        /* the cover for the card on screen, fetched meanwhile (it also stays in the cache for the shelf); a card with
         * the title when there is none */
        CoverJob *job = malloc(sizeof(*job));
        if (job)
        {
            job->gen = SDL_AtomicGet(&J.cover_gen);
            str_copy(job->serial, sizeof(job->serial), serial);
            str_copy(job->title, sizeof(job->title), title);
            SDL_Thread *t = SDL_CreateThread(cover_thread, "disc-cover", job);
            if (t)
                SDL_DetachThread(t);
            else
                free(job);
        }
    }

    char dir[SwanStationPS5_PATH_MAX], bin[SwanStationPS5_PATH_MAX], cue[SwanStationPS5_PATH_MAX],
        bin_part[SwanStationPS5_PATH_MAX + 8], cue_part[SwanStationPS5_PATH_MAX + 8];
    path_join(dir, sizeof(dir), J.games, dirname);
    char file[200];
    snprintf(file, sizeof(file), "%s.bin", folder);
    path_join(bin, sizeof(bin), dir, file);
    snprintf(file, sizeof(file), "%s.cue", folder);
    path_join(cue, sizeof(cue), dir, file);
    snprintf(bin_part, sizeof(bin_part), "%s.part", bin);
    snprintf(cue_part, sizeof(cue_part), "%s.part", cue);
    if (exists(cue))
    {
        P("%s is already there", cue);
        cdscsi_close(&pass);
        finish(DI_ALREADY);
        return;
    }
    if (!make_dirs(dir))
    {
        cdscsi_close(&pass);
        finish(DI_WRITE_FAILED);
        return;
    }

    /* the dump */
    SDL_AtomicSet(&J.dumping, 1); /* only now is it a copy: not for a game that was already there */
    SDL_AtomicSet(&J.total, (int)(leadout - tracks[0].lba));
    DiscImportState result = DI_OK;
    FILE *fb = fopen(bin_part, "wb"), *fc = fopen(cue_part, "wb");
    uint32_t first = tracks[0].lba, total = leadout - first, done = 0, bad = 0;
    uint64_t t_start = SDL_GetTicks64(), t_mark = t_start, error_ms = 0, last_good = t_start;
    bool slowed = false;
    uint32_t next_mark = total / 20, marked = 0; /* one line in the log every 5 % */
    if (!fb || !fc)
        result = DI_WRITE_FAILED;
    if (fc)
        fprintf(fc, "FILE \"%s.bin\" BINARY\r\n", folder);
    for (int t = 0; t < ntracks && result == DI_OK; ++t)
    {
        uint32_t start = tracks[t].lba, end = t + 1 < ntracks ? tracks[t + 1].lba : leadout;
        uint8_t flags = tracks[t].data ? 0xf8 : 0x10;
        const char *mode = "AUDIO";
        if (tracks[t].data)
        {
            mode = "MODE2/2352";
            if (read_cd(&pass, start, 1, 0xf8, raw, NULL) == 0 && raw[15] == 1)
                mode = "MODE1/2352";
        }
        uint32_t rel = start - first;
        fprintf(fc, "  TRACK %02d %s\r\n    INDEX 01 %02u:%02u:%02u\r\n", tracks[t].number, mode, rel / 4500,
                rel / 75 % 60, rel % 75);
        P("track %d: %s, LBA %u..%u", tracks[t].number, mode, start, end);
        for (uint32_t lba = start; lba < end && result == DI_OK; lba += BLOCK)
        {
            if (SDL_AtomicGet(&J.abort))
            {
                P("stopped at LBA %u: nothing was read for 30 seconds", lba);
                result = DI_STALLED;
                break;
            }
            uint32_t n = end - lba < BLOCK ? end - lba : BLOCK;
            bool ok = false;
            uint64_t t_try = SDL_GetTicks64();
            for (int attempt = 0; attempt < TRIES && !ok; ++attempt)
            {
                int rc = read_cd(&pass, lba, n, flags, raw, &sense);
                ok = rc == 0;
                if (rc == -2 || (rc == -3 && CDSENSE_IS_NO_MEDIUM(sense)))
                {
                    P("the disc or the drive is gone at LBA %u", lba);
                    result = DI_DRIVE_LOST;
                    break;
                }
                if (!ok && (sense >> 16) == 3)
                    break; /* a medium error: the drive has already retried by itself, more tries only wait */
            }
            if (result != DI_OK)
                break;
            if (ok)
                last_good = SDL_GetTicks64();
            if (!ok) /* sector by sector: the good ones are kept, the bad ones become zeros */
            {
                error_ms += SDL_GetTicks64() - t_try;
                if (!slowed)
                {
                    uint8_t set_speed[12] = {0xbb, 0, SPEED_AFTER_ERROR >> 8, SPEED_AFTER_ERROR & 0xff, 0xff, 0xff,
                                             0, 0, 0, 0, 0, 0};
                    cdscsi_cmd(&pass, set_speed, 12, NULL, 0, 5000, NULL, NULL);
                    slowed = true;
                    P("read errors at LBA %u: the drive is slowed to 4x", lba);
                }
                for (uint32_t i = 0; i < n && result == DI_OK; ++i)
                {
                    if (SDL_AtomicGet(&J.abort))
                    {
                        result = DI_STALLED;
                        break;
                    }
                    bool one = false;
                    bool medium = (sense >> 16) == 3;
                    uint64_t t_sector = SDL_GetTicks64();
                    for (int attempt = 0; attempt < (medium ? 1 : 2) && !one; ++attempt)
                        one = read_cd(&pass, lba + i, 1, flags, raw + i * SECTOR, &sense) == 0;
                    if (!one)
                        error_ms += SDL_GetTicks64() - t_sector;
                    if (one)
                    {
                        last_good = SDL_GetTicks64();
                        touch(); /* only a sector that was read counts as progress: zeros do not */
                    }
                    if (!one)
                    {
                        memset(raw + i * SECTOR, 0, SECTOR);
                        P("sector %u could not be read (sense %06x): zeros", lba + i, sense);
                        SDL_AtomicSet(&J.bad, (int)++bad);
                        if (bad > MAX_BAD)
                            result = DI_UNREADABLE;
                        if (CDSENSE_IS_NO_MEDIUM(sense))
                            result = DI_DRIVE_LOST;
                    }
                    SDL_AtomicSet(&J.permille, (int)((uint64_t)(done + i) * 1000 / total));
                    if (result == DI_OK && SDL_GetTicks64() - last_good > (uint64_t)STALL_SECONDS * 1000)
                    {
                        P("no sector could be read for %d s (LBA %u): giving up", STALL_SECONDS, lba + i);
                        result = DI_UNREADABLE;
                    }
                    if (result == DI_OK && error_ms > (uint64_t)MAX_ERROR_SECONDS * 1000)
                    {
                        P("%d s lost on reads that fail (LBA %u, %u bad sectors): giving up", MAX_ERROR_SECONDS, lba + i,
                          bad);
                        result = DI_UNREADABLE;
                    }
                }
            }
            if (result == DI_OK && fwrite(raw, SECTOR, n, fb) != n)
                result = DI_WRITE_FAILED;
            done += n;
            SDL_AtomicSet(&J.permille, (int)((uint64_t)done * 1000 / total));
            touch();
            if (done >= next_mark)
            {
                uint64_t now = SDL_GetTicks64();
                double secs = (double)(now - t_mark) / 1000.0;
                P("%u%%: %.0f sectors/s (%.1fx), %u bad so far", (done * 100) / total,
                  secs > 0 ? (done - marked) / secs : 0.0, secs > 0 ? (done - marked) / secs / 75.0 : 0.0, bad);
                marked = done;
                t_mark = now;
                next_mark = done + total / 20;
            }
        }
    }
    cdscsi_close(&pass);
    if (result == DI_OK && SDL_AtomicGet(&J.abort)) /* asked to stop just as it finished: nothing is kept */
        result = DI_STALLED;
    if (fb && fclose(fb) != 0 && result == DI_OK)
        result = DI_WRITE_FAILED;
    if (fc && fclose(fc) != 0 && result == DI_OK)
        result = DI_WRITE_FAILED;
    if (result == DI_OK && (rename(bin_part, bin) != 0 || rename(cue_part, cue) != 0))
        result = DI_WRITE_FAILED;
    if (multi && (result == DI_OK))
    {
        /* the playlist of the discs before this one is out of date: the library writes a new one on its next scan */
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)))
        {
            size_t n = strlen(e->d_name);
            if (n > 4 && strcmp(e->d_name + n - 4, ".m3u") == 0)
            {
                char m3u[SwanStationPS5_PATH_MAX];
                path_join(m3u, sizeof(m3u), dir, e->d_name);
                remove(m3u);
            }
        }
        if (d)
            closedir(d);
    }
    if (result != DI_OK && result != DI_OK_INCOMPLETE)
    {
        remove(bin_part);
        remove(cue_part);
        remove(bin);
        remove(cue);
        rmdir(dir);
    }
    if (result == DI_OK && bad)
        result = DI_OK_INCOMPLETE;
    P("done: result %d, %u sectors, %u unreadable, %.0f s", (int)result, done, bad,
      (double)(SDL_GetTicks64() - t_start) / 1000.0);
    finish(result);
    return;
}

static int import_thread(void *unused)
{
    (void)unused;
    cdscsi_lock(); /* the insert watcher stays out of the drive meanwhile */
    import_run();
    cdscsi_unlock();
    SDL_AtomicSet(&J.alive, 0);
    return 0;
}

bool discimport_start(const char *games_dir)
{
    if (discimport_state() != DI_IDLE)
        return false;
    if (SDL_AtomicGet(&J.alive)) /* a stalled one is still blocked in the drive: wait for it to come back */
        return false;
    str_copy(J.games, sizeof(J.games), games_dir);
    J.title[0] = '\0';
    J.serial[0] = '\0';
    J.key[0] = '\0';
    SDL_AtomicAdd(&J.cover_gen, 1); /* a cover still being fetched for an earlier import is dropped */
    cover_free();
    free(J.cover_px);
    J.cover_px = NULL;
    SDL_AtomicSet(&J.cover_ready, 0);
    SDL_AtomicSet(&J.cover_none, 0);
    SDL_AtomicSet(&J.dumping, 0);
    SDL_AtomicSet(&J.named, 0);
    SDL_AtomicSet(&J.permille, 0);
    SDL_AtomicSet(&J.bad, 0);
    SDL_AtomicSet(&J.abort, 0);
    SDL_AtomicSet(&J.alive, 1);
    touch();
    SDL_AtomicSet(&J.state, DI_RUNNING);
    SDL_Thread *t = SDL_CreateThread(import_thread, "disc-import", NULL);
    if (!t)
    {
        SDL_AtomicSet(&J.alive, 0);
        SDL_AtomicSet(&J.state, DI_IDLE);
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

/* ---------------------------------------------------------------- the insert watcher */

static struct
{
    SDL_atomic_t active, have, removed, paused;
    DiscId pending;
    SDL_Thread *thread;
} W;

/* TEST UNIT READY: 1 ready, 2 ready but the disc changed since the last look, 0 not ready (spinning up...),
 * -1 no disc in the drive */
static int unit_ready(const CdPass *pass)
{
    static const uint8_t tur[6] = {0};
    uint32_t sense = 0;
    int rc = cdscsi_cmd(pass, tur, 6, NULL, 0, 3000, &sense, NULL);
    if (rc == 0)
        return 1;
    if (rc == -2) /* the ioctl itself failed: the drive was unplugged */
        return -1;
    if ((sense >> 16) == 6) /* unit attention: the medium may have changed; ask again */
        return cdscsi_cmd(pass, tur, 6, NULL, 0, 3000, &sense, NULL) == 0 ? 2 : 0;
    return CDSENSE_IS_NO_MEDIUM(sense) ? -1 : 0;
}

/* Reads who the disc in `pass` is, as a PS1 game would be; false if it is not one. */
static bool read_disc(const CdPass *pass, DiscId *id)
{
    static const uint8_t read_toc[10] = {0x43, 0, 0, 0, 0, 0, 0, 0x08, 0x00, 0};
    uint8_t toc[2048];
    uint32_t got = 0, sense = 0;
    Track tracks[MAX_TRACKS];
    uint32_t leadout;
    memset(toc, 0, sizeof(toc));
    if (cdscsi_cmd(pass, read_toc, 10, toc, sizeof(toc), 10000, &sense, &got) != 0)
        return false;
    int n = parse_toc(toc, got, tracks, &leadout);
    if (!n || !leadout || !tracks[0].data)
        return false;
    return identify(pass, toc, got, tracks[0].lba, id, &sense);
}

static bool skip_present;

void discwatch_skip_present(void)
{
    skip_present = true;
}

static int watch_thread(void *unused)
{
    (void)unused;
    bool was_ready[8] = {false}, need[8], skipped[8] = {false};
    int gone[8] = {0}; /* polls in a row that found the drive empty after a disc */
    for (int u = 0; u < 8; ++u)
        need[u] = !skip_present; /* a disc already in when the app starts is news too, unless the app only started again after a game */
    bool first = true;
    for (;;)
    {
        if (!first)
            SDL_Delay(1200);
        first = false; /* the first look is at once: a disc already in when the app opens starts without waiting */
        if (SDL_AtomicGet(&W.paused))
            continue; /* another program (the app, for the disc daemon) is looking at the drive */
        if (!cdscsi_trylock())
            continue; /* an import is running */
        cdscsi_set_quiet(true);
        for (unsigned u = 0; u < 8; ++u)
        {
            char dev[24];
            snprintf(dev, sizeof(dev), "/dev/cd%u", u);
            CdPass pass;
            if (!exists(dev) || !cdscsi_open(u, &pass))
            {
                if (was_ready[u] && ++gone[u] >= 2) /* the drive itself is gone, with its disc */
                {
                    was_ready[u] = false;
                    gone[u] = 0;
                    SDL_AtomicAdd(&W.removed, 1);
                    P("watch: the drive cd%u was unplugged", u);
                }
                continue;
            }
            int ready = unit_ready(&pass);
            if (ready == -1)
            {
                if (was_ready[u] && ++gone[u] >= 2) /* the disc is out: the same one put back is news again */
                {
                    was_ready[u] = false;
                    gone[u] = 0;
                    SDL_AtomicAdd(&W.removed, 1);
                    P("watch: the disc in cd%u was taken out", u);
                }
            }
            else if (ready > 0)
            {
                gone[u] = 0;
                /* ALLOW MEDIUM REMOVAL: if anyone (the system, when it recognised the disc) left the tray locked, the
                 * drive's eject button does nothing; this frees it. Only while nothing is being installed: the watcher
                 * does not poll then. */
                static const uint8_t allow_removal[6] = {0x1e, 0, 0, 0, 0, 0};
                cdscsi_cmd(&pass, allow_removal, 6, NULL, 0, 3000, NULL, NULL);
                if (ready == 2) /* "the medium may have changed": the disc was swapped, even if the drive was never seen empty */
                {
                    SDL_AtomicAdd(&W.removed, 1);
                    P("watch: the disc in cd%u was changed", u);
                }
                if (!was_ready[u] || ready == 2)
                {
                    if (skip_present && !skipped[u])
                        skipped[u] = true; /* the disc that was in when the app started again is not news */
                    else
                        need[u] = true;
                }
                was_ready[u] = true;
                if (need[u] && SDL_AtomicGet(&W.active) && !SDL_AtomicGet(&W.have))
                {
                    need[u] = false;
                    DiscId id;
                    if (read_disc(&pass, &id))
                    {
                        P("watch: a disc in cd%u: '%s' (%s)", u, id.title, id.key);
                        W.pending = id;
                        SDL_AtomicSet(&W.have, 1);
                    }
                    else
                        P("watch: the disc in cd%u is not a PS1 game", u);
                }
            }
            cdscsi_close(&pass);
        }
        cdscsi_set_quiet(false);
        cdscsi_unlock();
    }
    return 0;
}

void discwatch_set_active(bool active)
{
    SDL_AtomicSet(&W.active, active ? 1 : 0); /* before the thread exists: its first look at the drive counts */
    if (active && !W.thread)
    {
        W.thread = SDL_CreateThread(watch_thread, "disc-watch", NULL);
        if (W.thread)
            SDL_DetachThread(W.thread);
        else
            W.thread = (SDL_Thread *)1; /* could not start: do not try every frame */
    }
}

void discwatch_pause(bool paused)
{
    SDL_AtomicSet(&W.paused, paused ? 1 : 0);
}

int discwatch_removed(void)
{
    return SDL_AtomicGet(&W.removed);
}

bool discwatch_take(DiscId *out)
{
    if (!SDL_AtomicGet(&W.have))
        return false;
    *out = W.pending;
    SDL_AtomicSet(&W.have, 0);
    return true;
}

#ifndef SwanStationPS5_PAYLOAD
/* ---------------------------------------------------------------- play from disc */

/* The disc is kept whole in memory when that much can be had. When it cannot, a window of it is: blocks in slots, the
 * ones around the game's position, read ahead as the game goes on and the farthest dropped. */
typedef struct
{
    uint32_t lba;
    uint8_t n, data;
} PBlock;

/* What the disc may take: the emulator needs the rest of the memory the app has (about 400 MB; with a window of 384 MB
 * it could not even make its own 8 MB arena) */
#define MEMORY_CAP ((uint64_t)128 << 20)
#define SLOT_BYTES ((size_t)BLOCK * SECTOR)

static struct
{
    SDL_Thread *thread;
    SDL_atomic_t state, error, abort, permille, lost;
    SDL_mutex *mu;
    SDL_cond *cv, *wake;
    char stem[40], path[64], cue[8192];
    size_t cue_len;
    DiscId id;
    uint32_t first, total;
    uint64_t size;
    PBlock *blk;
    uint8_t *have;
    int nblk, loaded, demand;
    uint8_t *ram;
    size_t ram_bytes;
    bool window;
    int32_t *slot_of, *block_of;
    int nslots, ahead, pos;
} D;

DiscPlayState discplay_state(void)
{
    return (DiscPlayState)SDL_AtomicGet(&D.state);
}

DiscImportState discplay_error(void)
{
    return (DiscImportState)SDL_AtomicGet(&D.error);
}

const DiscId *discplay_id(void)
{
    return &D.id;
}

const char *discplay_path(void)
{
    return D.path;
}

int discplay_permille(void)
{
    return SDL_AtomicGet(&D.permille);
}

bool discplay_lost(void)
{
    return SDL_AtomicGet(&D.lost) != 0;
}

static void play_fail(DiscImportState why)
{
    SDL_AtomicSet(&D.error, (int)why);
    SDL_AtomicSet(&D.state, DP_FAILED);
}

/* Where block b is in memory (a window: it has to have a slot) */
static uint8_t *play_ptr(int b)
{
    if (D.window)
        return D.ram + (size_t)D.slot_of[b] * SLOT_BYTES;
    return D.ram + (uint64_t)(D.blk[b].lba - D.first) * SECTOR;
}

static bool play_alloc(void)
{
    D.window = false;
    D.ram = MAP_FAILED;
    if (D.size <= MEMORY_CAP)
        D.ram = mmap(NULL, (size_t)D.size, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (D.ram != MAP_FAILED)
    {
        D.ram_bytes = (size_t)D.size;
        P("play: the whole disc is kept in memory (%llu bytes)", (unsigned long long)D.size);
        return true;
    }
    D.ram = NULL;
    static const size_t SIZES_MB[] = {128, 96, 64, 32};
    for (size_t k = 0; k < sizeof(SIZES_MB) / sizeof(SIZES_MB[0]); ++k)
    {
        const size_t bytes = SIZES_MB[k] << 20;
        if (bytes >= D.size)
            continue;
        D.ram = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
        if (D.ram == MAP_FAILED)
        {
            D.ram = NULL;
            continue;
        }
        D.ram_bytes = bytes;
        D.window = true;
        D.nslots = (int)(bytes / SLOT_BYTES);
        D.ahead = D.nslots * 3 / 4;
        D.slot_of = malloc((size_t)D.nblk * sizeof(int32_t));
        D.block_of = malloc((size_t)D.nslots * sizeof(int32_t));
        if (!D.slot_of || !D.block_of)
            return false;
        for (int i = 0; i < D.nblk; ++i)
            D.slot_of[i] = -1;
        for (int i = 0; i < D.nslots; ++i)
            D.block_of[i] = -1;
        D.pos = 0;
        P("play: the whole disc (%llu bytes) is not kept: a window of %zu bytes (%d blocks) is kept",
          (unsigned long long)D.size, bytes, D.nslots);
        return true;
    }
    return false;
}

static void play_release(void)
{
    if (D.ram)
        munmap(D.ram, D.ram_bytes);
    D.ram = NULL;
    free(D.blk);
    free(D.have);
    free(D.slot_of);
    free(D.block_of);
    D.blk = NULL;
    D.have = NULL;
    D.slot_of = D.block_of = NULL;
    D.nblk = D.loaded = D.nslots = 0;
    D.window = false;
}

/* 0 read, 1 the disc is gone or too much of it cannot be read */
static int play_block(const CdPass *pass, const PBlock *b, uint8_t *raw, bool *slowed, uint32_t *bad)
{
    const uint8_t flags = b->data ? 0xf8 : 0x10;
    uint32_t sense = 0;
    for (int attempt = 0; attempt < TRIES; ++attempt)
    {
        int rc = read_cd(pass, b->lba, b->n, flags, raw, &sense);
        if (rc == 0)
            return 0;
        if (rc == -2 || (rc == -3 && CDSENSE_IS_NO_MEDIUM(sense)))
            return 1;
        if ((sense >> 16) == 3)
            break;
    }
    if (!*slowed)
    {
        uint8_t set_speed[12] = {0xbb, 0, SPEED_AFTER_ERROR >> 8, SPEED_AFTER_ERROR & 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
        cdscsi_cmd(pass, set_speed, 12, NULL, 0, 5000, NULL, NULL);
        *slowed = true;
    }
    for (uint32_t i = 0; i < b->n; ++i)
    {
        bool one = false;
        for (int attempt = 0; attempt < 2 && !one; ++attempt)
            one = read_cd(pass, b->lba + i, 1, flags, raw + i * SECTOR, &sense) == 0;
        if (one)
            continue;
        if (CDSENSE_IS_NO_MEDIUM(sense))
            return 1;
        memset(raw + i * SECTOR, 0, SECTOR);
        P("play: sector %u could not be read (sense %06x): zeros", b->lba + i, sense);
        if (++*bad > MAX_BAD)
            return 1;
    }
    return 0;
}

/* The slot for block b (the mutex is held): a free one, else the block farthest from the game's position, the ones
 * behind it first */
static int play_slot(int b)
{
    int best = 0;
    long best_score = -1;
    for (int i = 0; i < D.nslots; ++i)
    {
        const int held = D.block_of[i];
        if (held < 0)
        {
            best = i;
            best_score = -2;
            break;
        }
        const long score = held < D.pos ? 1000000L + (D.pos - held) : (long)(held - D.pos);
        if (score > best_score)
        {
            best = i;
            best_score = score;
        }
    }
    if (best_score != -2)
    {
        D.have[D.block_of[best]] = 0;
        D.slot_of[D.block_of[best]] = -1;
        --D.loaded;
    }
    D.block_of[best] = b;
    D.slot_of[b] = best;
    return best;
}

static void play_run(void)
{
    CdPass pass;
    uint8_t toc[2048];
    uint32_t toc_got = 0;
    memset(toc, 0, sizeof(toc));
    DiscImportState found = find_disc(&pass, toc, sizeof(toc), &toc_got);
    if (found != DI_RUNNING)
    {
        play_fail(found);
        return;
    }
    Track tracks[MAX_TRACKS];
    uint32_t leadout = 0;
    int ntracks = parse_toc(toc, toc_got, tracks, &leadout);
    if (!ntracks || !leadout || leadout <= tracks[0].lba || !tracks[0].data)
    {
        P("play: the disc is not a PS1 game");
        cdscsi_close(&pass);
        play_fail(DI_NOT_PS1);
        return;
    }
    {
        uint8_t set_speed[12] = {0xbb, 0, 0xff, 0xff, 0xff, 0xff, 0, 0, 0, 0, 0, 0};
        cdscsi_cmd(&pass, set_speed, 12, NULL, 0, 5000, NULL, NULL);
    }
    uint32_t sense = 0;
    DiscId id;
    if (!identify(&pass, toc, toc_got, tracks[0].lba, &id, &sense))
    {
        cdscsi_close(&pass);
        play_fail(CDSENSE_IS_NO_MEDIUM(sense) ? DI_DRIVE_LOST : DI_NOT_PS1);
        return;
    }
    D.id = id;
    D.first = tracks[0].lba;
    D.total = leadout - D.first;
    D.size = (uint64_t)D.total * SECTOR;
    snprintf(D.stem, sizeof(D.stem), "%s-%08x", id.serial[0] ? id.serial : "DISC", id.fingerprint);
    snprintf(D.path, sizeof(D.path), DISCPLAY_DIR "/%s.cue", D.stem);

    static uint8_t raw[SECTOR * BLOCK];
    D.cue_len = (size_t)snprintf(D.cue, sizeof(D.cue), "FILE \"%s.bin\" BINARY\r\n", D.stem);
    int nblk = 0;
    for (int t = 0; t < ntracks; ++t)
    {
        uint32_t start = tracks[t].lba, end = t + 1 < ntracks ? tracks[t + 1].lba : leadout;
        const char *mode = "AUDIO";
        if (tracks[t].data)
        {
            mode = "MODE2/2352";
            if (read_cd(&pass, start, 1, 0xf8, raw, NULL) == 0 && raw[15] == 1)
                mode = "MODE1/2352";
        }
        uint32_t rel = start - D.first;
        if (D.cue_len < sizeof(D.cue) - 128)
            D.cue_len += (size_t)snprintf(D.cue + D.cue_len, sizeof(D.cue) - D.cue_len,
                                          "  TRACK %02d %s\r\n    INDEX 01 %02u:%02u:%02u\r\n", tracks[t].number, mode,
                                          rel / 4500, rel / 75 % 60, rel % 75);
        nblk += (int)((end - start + BLOCK - 1) / BLOCK);
    }
    D.nblk = nblk;
    D.blk = malloc((size_t)nblk * sizeof(PBlock));
    D.have = calloc((size_t)nblk, 1);
    if (!D.blk || !D.have || !play_alloc())
    {
        cdscsi_close(&pass);
        play_release();
        play_fail(DI_WRITE_FAILED);
        return;
    }
    {
        int b = 0;
        for (int t = 0; t < ntracks; ++t)
        {
            uint32_t start = tracks[t].lba, end = t + 1 < ntracks ? tracks[t + 1].lba : leadout;
            for (uint32_t lba = start; lba < end; lba += BLOCK, ++b)
                D.blk[b] = (PBlock){lba, (uint8_t)(end - lba < BLOCK ? end - lba : BLOCK), tracks[t].data};
        }
    }
    D.loaded = 0;
    D.demand = -1;
    P("play: '%s' (%s), %d tracks, %u sectors in %d blocks, as %s", id.title, id.key, ntracks, D.total, nblk, D.path);
    SDL_AtomicSet(&D.state, DP_READY);

    int cursor = 0, next_pct = 5;
    bool slowed = false;
    uint32_t bad = 0;
    const uint64_t t_start = SDL_GetTicks64();
    uint64_t t_log = t_start;
    bool ahead_full = false;
    while (!SDL_AtomicGet(&D.abort))
    {
        int b = -1;
        SDL_LockMutex(D.mu);
        if (D.demand >= 0 && D.demand < D.nblk && !D.have[D.demand])
            b = D.demand;
        D.demand = -1;
        if (b < 0 && D.window)
        {
            for (int i = 0; i < D.ahead && D.pos + i < D.nblk; ++i)
                if (!D.have[D.pos + i])
                {
                    b = D.pos + i;
                    break;
                }
        }
        else if (b < 0)
        {
            for (int i = 0; i < D.nblk; ++i)
            {
                int k = (cursor + i) % D.nblk;
                if (!D.have[k])
                {
                    b = k;
                    break;
                }
            }
        }
        if (b < 0)
        {
            if (!D.window) /* all of it is in */
            {
                SDL_UnlockMutex(D.mu);
                break;
            }
            /* the window is full ahead of the game: wait for it to move, and look at the drive meanwhile */
            if (!ahead_full)
            {
                ahead_full = true;
                P("play: the window is full ahead of the game (%d%% of the disc is in memory, the game at %d%% of the disc, %.0f s)",
                  D.loaded * 100 / D.nblk, D.pos * 100 / D.nblk, (double)(SDL_GetTicks64() - t_start) / 1000.0);
            }
            SDL_CondWaitTimeout(D.wake, D.mu, 1000);
            SDL_UnlockMutex(D.mu);
            static const uint8_t tur[6] = {0};
            uint32_t tsense = 0;
            int rc = cdscsi_cmd(&pass, tur, 6, NULL, 0, 3000, &tsense, NULL);
            if (rc == -2 || (rc == -3 && CDSENSE_IS_NO_MEDIUM(tsense)))
            {
                P("play: the disc is gone");
                SDL_AtomicSet(&D.lost, 1);
                break;
            }
            continue;
        }
        if (D.window && D.slot_of[b] < 0)
            play_slot(b);
        SDL_UnlockMutex(D.mu);
        int rc = play_block(&pass, &D.blk[b], raw, &slowed, &bad);
        if (rc)
        {
            P("play: the disc is gone or unreadable at LBA %u (%d blocks in memory)", D.blk[b].lba, D.loaded);
            SDL_AtomicSet(&D.lost, 1);
            break;
        }
        memcpy(play_ptr(b), raw, (size_t)D.blk[b].n * SECTOR);
        SDL_LockMutex(D.mu);
        D.have[b] = 1;
        ++D.loaded;
        SDL_CondBroadcast(D.cv);
        SDL_UnlockMutex(D.mu);
        cursor = b + 1;
        SDL_AtomicSet(&D.permille, (int)((uint64_t)(D.window ? b : D.loaded) * 1000 / (uint64_t)D.nblk));
        if (D.window)
        {
            ahead_full = false;
            if (SDL_GetTicks64() - t_log >= 3000)
            {
                t_log = SDL_GetTicks64();
                P("play: %d%% of the disc is in memory (window %d%% full, %d of %d blocks), the game at %d%% of the disc",
                  D.loaded * 100 / D.nblk, D.loaded * 100 / D.nslots, D.loaded, D.nslots, D.pos * 100 / D.nblk);
            }
        }
        if (!D.window && D.loaded * 100 / D.nblk >= next_pct)
        {
            P("play: %d%% of the disc is in memory (%.0f s)", D.loaded * 100 / D.nblk,
              (double)(SDL_GetTicks64() - t_start) / 1000.0);
            next_pct = D.loaded * 100 / D.nblk + 5;
        }
    }
    cdscsi_close(&pass);
    if (!D.window && D.loaded >= D.nblk)
        P("play: the whole disc is in memory (%u unreadable sectors, %.0f s)", bad,
          (double)(SDL_GetTicks64() - t_start) / 1000.0);
    SDL_LockMutex(D.mu);
    SDL_CondBroadcast(D.cv); /* readers waiting for what will not come */
    SDL_UnlockMutex(D.mu);
}

static int play_thread(void *unused)
{
    (void)unused;
    cdscsi_lock();
    play_run();
    cdscsi_unlock();
    return 0;
}

bool discplay_start(void)
{
    if (discplay_state() != DP_IDLE)
        return false;
    if (!D.mu)
    {
        D.mu = SDL_CreateMutex();
        D.cv = SDL_CreateCond();
        D.wake = SDL_CreateCond();
        if (!D.mu || !D.cv || !D.wake)
            return false;
    }
    memset(&D.id, 0, sizeof(D.id));
    D.path[0] = D.stem[0] = '\0';
    D.cue_len = 0;
    SDL_AtomicSet(&D.abort, 0);
    SDL_AtomicSet(&D.lost, 0);
    SDL_AtomicSet(&D.permille, 0);
    SDL_AtomicSet(&D.error, 0);
    SDL_AtomicSet(&D.state, DP_PREPARING);
    D.thread = SDL_CreateThread(play_thread, "disc-play", NULL);
    if (!D.thread)
    {
        SDL_AtomicSet(&D.state, DP_IDLE);
        return false;
    }
    return true;
}

void discplay_stop(void)
{
    SDL_AtomicSet(&D.abort, 1);
    if (D.mu)
    {
        SDL_LockMutex(D.mu);
        SDL_CondBroadcast(D.cv);
        SDL_CondBroadcast(D.wake);
        SDL_UnlockMutex(D.mu);
    }
    if (D.thread)
        SDL_WaitThread(D.thread, NULL);
    D.thread = NULL;
    play_release();
    D.path[0] = D.stem[0] = '\0';
    SDL_AtomicSet(&D.state, DP_IDLE);
}

int discplay_file(const char *name, uint64_t *size)
{
    if (discplay_state() != DP_READY)
        return 0;
    const size_t n = strlen(name), s = strlen(D.stem);
    if (n != s + 4 || strncmp(name, D.stem, s) != 0)
        return 0;
    if (strcmp(name + s, ".cue") == 0)
    {
        *size = D.cue_len;
        return 1;
    }
    if (strcmp(name + s, ".bin") == 0)
    {
        *size = D.size;
        return 2;
    }
    return 0;
}

size_t discplay_read(int kind, uint64_t off, void *dst, size_t n)
{
    uint8_t *out = dst;
    if (discplay_state() != DP_READY)
        return 0;
    if (kind == 1)
    {
        if (off >= D.cue_len)
            return 0;
        size_t take = D.cue_len - off < n ? (size_t)(D.cue_len - off) : n;
        memcpy(out, D.cue + off, take);
        return take;
    }
    size_t got = 0;
    while (got < n && off < D.size)
    {
        const uint32_t lba = D.first + (uint32_t)(off / SECTOR);
        int lo = 0, hi = D.nblk - 1;
        while (lo < hi) /* the last block that starts at or before the sector */
        {
            int mid = (lo + hi + 1) / 2;
            if (D.blk[mid].lba <= lba)
                lo = mid;
            else
                hi = mid - 1;
        }
        SDL_LockMutex(D.mu);
        D.pos = lo;
        SDL_CondSignal(D.wake);
        while (!D.have[lo] && !SDL_AtomicGet(&D.abort) && !SDL_AtomicGet(&D.lost))
        {
            D.demand = lo;
            SDL_CondSignal(D.wake);
            SDL_CondWaitTimeout(D.cv, D.mu, 200);
        }
        size_t take = 0;
        if (D.have[lo])
        {
            const uint64_t block_start = (uint64_t)(D.blk[lo].lba - D.first) * SECTOR;
            const uint64_t block_len = (uint64_t)D.blk[lo].n * SECTOR;
            take = block_start + block_len - off < n - got ? (size_t)(block_start + block_len - off) : n - got;
            memcpy(out + got, play_ptr(lo) + (off - block_start), take);
        }
        SDL_UnlockMutex(D.mu);
        if (!take)
            break;
        got += take;
        off += take;
    }
    return got;
}
#endif
