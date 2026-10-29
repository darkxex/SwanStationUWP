/*
 * SwanStation - SCSI pass-through to a CD drive (the USB CD drive of the Xbox), shared by the drive probe and
 * the disc import. On other platforms every call fails.
 * The CAM calls follow ps-disc-detect (Al-Azif, GPL-3.0) and PS5SX2 (GPL-3.0).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef SwanStationPS5_CDSCSI_H
#define SwanStationPS5_CDSCSI_H

#include "SwanStationPS5.h"

typedef struct
{
    int fd;
    int path, target, lun;
} CdPass;

/* /dev/cd<unit> -> its pass device, ready to take commands. */
bool cdscsi_open(unsigned unit, CdPass *pass);
void cdscsi_close(CdPass *pass);

/* One command. 0 when it completed with every byte; -2 when the ioctl itself failed (the drive is gone); -3 when
 * the drive answered with an error or too few bytes, and then *sense is key<<16 | asc<<8 | ascq (0 if none).
 * `got`: for answers of variable length (the TOC) a short one is fine and *got says how many bytes came; NULL
 * asks for all of them. */
int cdscsi_cmd(const CdPass *pass, const uint8_t *cdb, int cdb_len, void *buf, uint32_t len,
               uint32_t timeout_ms, uint32_t *sense, uint32_t *got);

/* One conversation with the drive at a time: the import holds this for its whole run, the insert watcher only
 * takes it between two polls. */
void cdscsi_lock(void);
bool cdscsi_trylock(void);
void cdscsi_unlock(void);
/* While quiet, nothing is written to the log (the insert watcher polls every second). */
void cdscsi_set_quiet(bool quiet);

#define CDSENSE_NO_MEDIUM 0x023a00 /* no disc in the drive (the last byte says more: 02/3A/01 and /02 are the tray) */
#define CDSENSE_IS_NO_MEDIUM(s) (((s) >> 8) == 0x023a)
#define CDSENSE_BECOMING_READY 0x020400

#endif
