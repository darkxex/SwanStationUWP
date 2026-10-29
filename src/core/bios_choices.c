/*
 * SwanStation - the choices of the core's three BIOS options. See bios_choices.h.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "bios_choices.h"

#include "../SwanStationPS5.h"
#include "../i18n.h"
#include "swanstation_options.h"

#include "../../third_party/rcheevos/src/rhash/md5.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_CHOICES 32
#define NAME_SIZE 64

static char names[MAX_CHOICES][NAME_SIZE];
static char texts[MAX_CHOICES][NAME_SIZE + 96];
static const char *labels[MAX_CHOICES];
static int count;

/* The core knows a BIOS by the MD5 of its contents; its table is copied here (tools/gen_bios_hashes.py). */
static const struct
{
    const char *md5, *description;
} KNOWN[] = {
#include "bios_hashes.inc"
};

static char bios_dir[SwanStationPS5_PATH_MAX];

/* The MD5 of a file as 32 hex digits; false when it can't be read or is not the size of a BIOS image. */
static bool md5_of_file(const char *path, char hex[33])
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    md5_state_t md5;
    md5_init(&md5);
    unsigned char chunk[16384];
    size_t total = 0, got;
    while ((got = fread(chunk, 1, sizeof(chunk), f)) > 0)
    {
        total += got;
        if (total > 8u * 1024 * 1024)
            break;
        md5_append(&md5, chunk, (int)got);
    }
    fclose(f);
    if (total < 64 * 1024 || total > 8u * 1024 * 1024)
        return false;
    unsigned char digest[16];
    md5_finish(&md5, digest);
    for (int i = 0; i < 16; ++i)
        snprintf(hex + 2 * i, 3, "%02x", digest[i]);
    return true;
}

/* "SCPH-5501, 5503, 7003 (v3.0 11-18-96 A) (scph5501.bin)", as the core and DuckStation name it; the file name when
 * the contents are not a BIOS it knows */
static const char *describe(int index)
{
    char path[SwanStationPS5_PATH_MAX], hex[33];
    path_join(path, sizeof(path), bios_dir, names[index]);
    if (bios_dir[0] && md5_of_file(path, hex))
        for (size_t k = 0; k < sizeof(KNOWN) / sizeof(KNOWN[0]); ++k)
            if (strcmp(hex, KNOWN[k].md5) == 0)
            {
                snprintf(texts[index], sizeof(texts[index]), "%s (%s)", KNOWN[k].description, names[index]);
                return texts[index];
            }
    return names[index];
}

/* The region of the BIOS in names[index]: the last letter of its description, "(v3.0 09-09-96 J)": 'J' Japan, 'A'
 * America, 'E' Europe; 0 when the contents are not a BIOS it knows (or the model carries no letter). Read once. */
static char region_of(int index)
{
    static char cache[MAX_CHOICES];
    static bool known[MAX_CHOICES];
    if (known[index])
        return cache[index];
    known[index] = true;
    cache[index] = 0;
    char path[SwanStationPS5_PATH_MAX], hex[33];
    path_join(path, sizeof(path), bios_dir, names[index]);
    if (!bios_dir[0] || !md5_of_file(path, hex))
        return 0;
    for (size_t k = 0; k < sizeof(KNOWN) / sizeof(KNOWN[0]); ++k)
        if (strcmp(hex, KNOWN[k].md5) == 0)
        {
            const char *end = strrchr(KNOWN[k].description, ')');
            if (end && end > KNOWN[k].description + 1 && end[-2] == ' ' && strchr("JAE", end[-1]))
                cache[index] = end[-1];
            break;
        }
    return cache[index];
}

static void init(void)
{
    if (count)
        return;
    names[0][0] = '\0';
    labels[0] = "Detect automatically";
    str_copy(names[1], NAME_SIZE, "openbios.bin");
    labels[1] = "OpenBIOS (built into the core)";
    count = 2;
}

bool ss_opt_is_bios(int opt)
{
    return opt >= 0 && opt < SS_OPT_COUNT && strncmp(SS_OPTS[opt].key, "swanstation_BIOS_Path", 21) == 0;
}

int ss_opt_count(int opt)
{
    if (!ss_opt_is_bios(opt))
        return SS_OPTS[opt].count;
    init();
    return count;
}

const char *ss_opt_value(int opt, int index)
{
    if (!ss_opt_is_bios(opt))
        return SS_OPTS[opt].values[index >= 0 && index < SS_OPTS[opt].count ? index : SS_OPTS[opt].def];
    init();
    return names[index >= 0 && index < count ? index : 0];
}

const char *const *ss_opt_labels(int opt)
{
    if (!ss_opt_is_bios(opt))
        return SS_OPTS[opt].labels;
    init();
    return labels;
}

/* Index of a name in the shared list; with `add`, appended when it is new. */
static int find_name(const char *value, bool add)
{
    init();
    for (int v = 0; v < count; ++v)
        if (strcmp(value, names[v]) == 0)
            return v;
    if (!add || count >= MAX_CHOICES || !value[0])
        return -1;
    str_copy(names[count], NAME_SIZE, value);
    labels[count] = describe(count);
    return count++;
}

int ss_opt_find(int opt, const char *value, bool add)
{
    if (ss_opt_is_bios(opt))
        return find_name(value, add);
    for (int v = 0; v < SS_OPTS[opt].count; ++v)
        if (strcmp(value, SS_OPTS[opt].values[v]) == 0)
            return v;
    return -1;
}

static int by_name(const void *a, const void *b)
{
    return str_icmp((const char *)a, (const char *)b);
}

const char *bios_auto_file(int opt, bool *core_finds)
{
    init();
    if (!ss_opt_is_bios(opt))
        return NULL;
    const char *key = SS_OPTS[opt].key;
    const char want = strstr(key, "NTSCJ") ? 'J' : strstr(key, "NTSCU") ? 'A' : strstr(key, "PAL") ? 'E' : 0;
    int fallback = -1, unknown = -1;
    for (int v = 2; v < count; ++v) /* 0 is Auto, 1 the OpenBIOS: the files are after them */
    {
        const char r = region_of(v);
        if (r && r == want)
        {
            *core_finds = true;
            return names[v];
        }
        if (r && fallback < 0)
            fallback = v;
        if (!r && unknown < 0)
            unknown = v;
    }
    *core_finds = false;
    return fallback >= 0 ? names[fallback] : unknown >= 0 ? names[unknown] : NULL;
}

void bios_choices_scan(const char *dir)
{
    init();
    str_copy(bios_dir, sizeof(bios_dir), dir);
    DIR *d = opendir(dir);
    if (!d)
        return;
    char found[MAX_CHOICES][NAME_SIZE];
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < MAX_CHOICES)
        if (str_icmp(path_ext(e->d_name), "bin") == 0 && strlen(e->d_name) < NAME_SIZE &&
            str_icmp(e->d_name, "openbios.bin") != 0)
            str_copy(found[n++], NAME_SIZE, e->d_name);
    closedir(d);
    qsort(found, (size_t)n, NAME_SIZE, by_name);
    for (int i = 0; i < n; ++i)
        find_name(found[i], true);
}
