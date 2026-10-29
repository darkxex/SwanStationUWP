/*
 * SwanStation - SCSI pass-through to a CD drive. See cdscsi.h.
 * The command counts as done only when the status is CAM_REQ_CMP and nothing is left untransferred (resid == 0):
 * on the USB path of the Xbox a drive has answered "complete" with 0 bytes (PS5SX2, review of its PR #34).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "cdscsi.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <SDL2/SDL.h>

static SDL_mutex *drive_mutex;

static SDL_mutex *the_mutex(void)
{
    if (!drive_mutex)
        drive_mutex = SDL_CreateMutex(); /* first used from the main thread, before any thread starts */
    return drive_mutex;
}

void cdscsi_lock(void)
{
    SDL_LockMutex(the_mutex());
}

bool cdscsi_trylock(void)
{
    return SDL_TryLockMutex(the_mutex()) == 0;
}

void cdscsi_unlock(void)
{
    SDL_UnlockMutex(the_mutex());
}

static bool quiet_mode;

void cdscsi_set_quiet(bool quiet)
{
    quiet_mode = quiet; /* only changed while holding the drive lock */
}

#if defined(__PROSPERO__)
#include <sys/types.h>
#include <sys/ioctl.h>
#include <cam/cam.h>
#include <cam/cam_ccb.h>
#include <cam/scsi/scsi_all.h>
#include <cam/scsi/scsi_message.h>
#include <cam/scsi/scsi_pass.h>

#define P(...) (quiet_mode ? (void)0 : SwanStationPS5_log("cdscsi: " __VA_ARGS__))

int cdscsi_cmd(const CdPass *p, const uint8_t *cdb, int cdb_len, void *buf, uint32_t len, uint32_t timeout_ms,
               uint32_t *sense, uint32_t *got)
{
    union ccb *ccb = calloc(1, sizeof(*ccb));
    if (!ccb)
        return -1;
    cam_fill_csio(&ccb->csio, 1, NULL, (len ? CAM_DIR_IN : CAM_DIR_NONE) | CAM_DEV_QFRZDIS, MSG_SIMPLE_Q_TAG,
                  (uint8_t *)buf, len, SSD_FULL_SIZE, (uint8_t)cdb_len, timeout_ms);
    ccb->ccb_h.path_id = (path_id_t)p->path;
    ccb->ccb_h.target_id = (target_id_t)p->target;
    ccb->ccb_h.target_lun = (lun_id_t)p->lun;
    memcpy(ccb->csio.cdb_io.cdb_bytes, cdb, (size_t)cdb_len);
    int rc = ioctl(p->fd, CAMIOCOMMAND, ccb);
    int result = 0;
    if (sense)
        *sense = 0;
    if (rc < 0)
    {
        P("ioctl CAMIOCOMMAND failed: %s", strerror(errno));
        result = -2;
    }
    else if ((ccb->ccb_h.status & CAM_STATUS_MASK) != CAM_REQ_CMP || (ccb->csio.resid != 0 && !got))
    {
        result = -3;
        if (sense && (ccb->ccb_h.status & CAM_AUTOSNS_VALID))
        {
            const uint8_t *s = (const uint8_t *)&ccb->csio.sense_data;
            if ((s[0] & 0x7e) == 0x72) /* descriptor format */
                *sense = (uint32_t)(s[1] & 0x0f) << 16 | (uint32_t)s[2] << 8 | s[3];
            else
                *sense = (uint32_t)(s[2] & 0x0f) << 16 | (uint32_t)s[12] << 8 | s[13];
        }
        P("command %02x: status 0x%x scsi %u resid %u sense %06x", cdb[0], ccb->ccb_h.status,
          ccb->csio.scsi_status, ccb->csio.resid, sense ? *sense : 0);
    }
    if (got)
        *got = result == 0 ? len - ccb->csio.resid : 0;
    free(ccb);
    return result;
}

bool cdscsi_open(unsigned unit, CdPass *p)
{
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    int xpt = open("/dev/xpt0", O_RDWR);
    if (xpt < 0)
    {
        P("open /dev/xpt0: %s", strerror(errno));
        return false;
    }
    union ccb *ccb = calloc(1, sizeof(*ccb));
    bool ok = false;
    if (ccb)
    {
        ccb->ccb_h.func_code = XPT_GDEVLIST;
        snprintf(ccb->cgdl.periph_name, sizeof(ccb->cgdl.periph_name), "cd");
        ccb->cgdl.unit_number = unit;
        if (ioctl(xpt, CAMGETPASSTHRU, ccb) < 0)
            P("CAMGETPASSTHRU for cd%u: %s", unit, strerror(errno));
        else
        {
            char path[40];
            snprintf(path, sizeof(path), "/dev/%s%u", ccb->cgdl.periph_name, ccb->cgdl.unit_number);
            P("cd%u is %s", unit, path);
            p->fd = open(path, O_RDWR);
            if (p->fd < 0)
                P("open %s: %s", path, strerror(errno));
            else
            {
                memset(ccb, 0, sizeof(*ccb));
                if (ioctl(p->fd, CAMGETPASSTHRU, ccb) < 0)
                    P("CAMGETPASSTHRU on the pass: %s", strerror(errno));
                else
                {
                    p->path = ccb->ccb_h.path_id;
                    p->target = ccb->ccb_h.target_id;
                    p->lun = ccb->ccb_h.target_lun;
                    P("pass path %d target %d lun %d", p->path, p->target, p->lun);
                    ok = true;
                }
                if (!ok)
                {
                    close(p->fd);
                    p->fd = -1;
                }
            }
        }
        free(ccb);
    }
    close(xpt);
    return ok;
}

void cdscsi_close(CdPass *p)
{
    if (p->fd >= 0)
        close(p->fd);
    p->fd = -1;
}

#else

bool cdscsi_open(unsigned unit, CdPass *p)
{
    (void)unit;
    memset(p, 0, sizeof(*p));
    p->fd = -1;
    return false;
}

void cdscsi_close(CdPass *p)
{
    (void)p;
}

int cdscsi_cmd(const CdPass *p, const uint8_t *cdb, int cdb_len, void *buf, uint32_t len, uint32_t timeout_ms,
               uint32_t *sense, uint32_t *got)
{
    (void)p, (void)cdb, (void)cdb_len, (void)buf, (void)len, (void)timeout_ms;
    if (sense)
        *sense = 0;
    if (got)
        *got = 0;
    return -2;
}

#endif
