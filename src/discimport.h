/*
 * SwanStation - imports the PS1 disc in the USB CD drive: dumps it to <games>/<Title [serial]>/<Title [serial]>.bin
 * and .cue, which the library then picks up like any other game. Runs in a thread; the screen shows a bar.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_DISCIMPORT_H
#define SwanStationPS5_DISCIMPORT_H

#include "SwanStationPS5.h"

typedef enum
{
    DI_IDLE,
    DI_RUNNING,
    DI_OK,          /* imported: the library has a new game */
    DI_OK_INCOMPLETE, /* imported, but some sectors could not be read and are zeros: discimport_bad() */
    DI_ALREADY,     /* it was already in the games folder */
    DI_NO_DRIVE,    /* no CD drive found, or the app may not open it */
    DI_NO_DISC,     /* the drive is empty or not ready */
    DI_NOT_PS1,     /* the first track is not data, or the disc has no ISO 9660 volume */
    DI_UNREADABLE,  /* too many sectors could not be read */
    DI_DRIVE_LOST,  /* the disc was taken out or the drive unplugged halfway */
    DI_WRITE_FAILED, /* the games folder could not be written */
    DI_STALLED,     /* nothing was read for 30 seconds: stopped (discimport_check_stall) */
    DI_CANCELLED    /* the player held O: stopped (discimport_cancel) */
} DiscImportState;

/* Who a disc is: read from its first data track. serial is empty for a disc without SYSTEM.CNF (homebrew). */
typedef struct
{
    char serial[16];     /* SLUS-00662 */
    char title[96];      /* the game database's name, else the volume label, else "Untitled disc" */
    char label[40];      /* the volume label */
    uint32_t fingerprint; /* CRC-32 of the table of contents and the volume descriptor */
    char key[24];        /* the serial, or the fingerprint in hex: how the disc is told apart */
} DiscId;

/* Starts the import into `games_dir`; false if one is already running (or its result has not been taken). */
bool discimport_start(const char *games_dir);
void discwatch_skip_present(void);
/* DI_RUNNING while it works, then the result until discimport_ack(). */
DiscImportState discimport_state(void);
void discimport_ack(void);
int discimport_permille(void);
/* The game's name, once the disc has been identified; "" before. */
const char *discimport_title(void);
/* Call every frame while it runs: true (once) if it made no progress for 30 seconds and was given up. The state is
 * then DI_STALLED, whatever the thread is still blocked on; it deletes its partial files when it comes back. */
bool discimport_check_stall(void);
/* Stops the import now (the player asked): the state becomes DI_CANCELLED at once, and the thread deletes its partial
 * files when the drive hands it back. The same disc does not start it again until it is taken out and put back. */
void discimport_cancel(void);
/* How many sectors could not be read (zeros in the image). */
int discimport_bad(void);
const char *discimport_games_dir(void);
/* At start: removes what an interrupted import left (*.part in the games folder's subfolders). */
void discimport_cleanup(const char *games_dir);

/* The serial of the disc being imported, and its cover once it is known (a texture made on the calling thread;
 * NULL until then or when there is none). */
const char *discimport_serial(void);
/* The disc's key (the serial, or the fingerprint in hex) once it is known; "" before. */
const char *discimport_key(void);
struct PlatTexture;
struct PlatTexture *discimport_cover(void);
/* How many sectors the copy has (0 until it begins). */
int discimport_total_sectors(void);
/* True once the disc is really being copied (it is named earlier, and may turn out to be installed already). */
bool discimport_dumping(void);
/* True once it is known that there is no cover for the disc (no serial, none online): show the shelf's own picture. */
bool discimport_cover_none(void);

/* The insert watcher: while `active`, it looks at the USB CD drive every second or so, and a disc that was not
 * there before is read and reported once. discwatch_take() hands the report over (false when there is none). */
void discwatch_set_active(bool active);
bool discwatch_take(DiscId *out);
/* While paused the watcher does not touch the drive at all (the disc daemon pauses it while the app is open, which
 * watches the drive itself: two pollers would split the drive's "medium changed" notices between them). */
void discwatch_pause(bool paused);
/* How many times a disc was taken out of the drive since the app started (confirmed twice, so a drive that is
 * only busy does not count). It goes up while a game is running too. */
int discwatch_removed(void);

/* Play from disc: the disc in the drive is read into memory by a thread (the order the game asks for, else from the
 * start; the whole disc, or when that much memory cannot be had a window of 128 MB or less around the game's position, so that the emulator keeps its own memory) and the core loads it as a game from a virtual folder, so nothing is installed. It is ready (the game can
 * start) as soon as the disc is identified. */
typedef enum
{
    DP_IDLE,
    DP_PREPARING, /* the drive is being asked who the disc is */
    DP_READY,     /* discplay_path() can be loaded; the rest of the disc is still being read, or is done */
    DP_FAILED     /* discplay_error() says why; discplay_stop() clears it */
} DiscPlayState;

bool discplay_start(void);
DiscPlayState discplay_state(void);
DiscImportState discplay_error(void);
const DiscId *discplay_id(void);
/* The .cue the core loads ("/discplay/<serial>-<hex>.cue"). */
const char *discplay_path(void);
/* How much of the disc is in memory, 0 to 1000. */
int discplay_permille(void);
/* True once the disc went (or too much of it could not be read) before it was all in memory. */
bool discplay_lost(void);
/* Stops reading and frees the memory. Not while a game is loaded from it. */
void discplay_stop(void);

#endif
