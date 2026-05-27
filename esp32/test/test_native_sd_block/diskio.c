// FatFs diskio dispatch for native tests.
// VolToPart: required when FF_MULTI_PARTITION=1.
// Maps drive N to physical drive N with auto-partition search (pt=0).
#include "ffconf.h"
#include "ff.h"
const PARTITION VolToPart[FF_VOLUMES] = {
    {0, 0}, {1, 0}, {2, 0}, {3, 0}
};
// disk_read/write/ioctl/status/initialize are implemented here and delegate
// to a table of callbacks registered by tests via diskio_register_native().

#include "diskio.h"
#include <string.h>

typedef struct {
    DSTATUS (*init)(BYTE pdrv);
    DSTATUS (*status)(BYTE pdrv);
    DRESULT (*read)(BYTE pdrv, BYTE* buf, LBA_t sector, UINT count);
    DRESULT (*write)(BYTE pdrv, const BYTE* buf, LBA_t sector, UINT count);
    DRESULT (*ioctl)(BYTE pdrv, BYTE cmd, void* buf);
} NativeDiskio;

static NativeDiskio g_diskio[FF_VOLUMES];

void diskio_register_native(BYTE pdrv,
    DSTATUS (*init_fn)(BYTE),
    DSTATUS (*status_fn)(BYTE),
    DRESULT (*read_fn)(BYTE, BYTE*, LBA_t, UINT),
    DRESULT (*write_fn)(BYTE, const BYTE*, LBA_t, UINT),
    DRESULT (*ioctl_fn)(BYTE, BYTE, void*))
{
    if (pdrv >= FF_VOLUMES) return;
    g_diskio[pdrv].init   = init_fn;
    g_diskio[pdrv].status = status_fn;
    g_diskio[pdrv].read   = read_fn;
    g_diskio[pdrv].write  = write_fn;
    g_diskio[pdrv].ioctl  = ioctl_fn;
}

void diskio_unregister_native(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES) return;
    memset(&g_diskio[pdrv], 0, sizeof(g_diskio[pdrv]));
}

DSTATUS disk_initialize(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES || !g_diskio[pdrv].init) return STA_NOINIT;
    return g_diskio[pdrv].init(pdrv);
}

DSTATUS disk_status(BYTE pdrv) {
    if (pdrv >= FF_VOLUMES || !g_diskio[pdrv].status) return STA_NOINIT;
    return g_diskio[pdrv].status(pdrv);
}

DRESULT disk_read(BYTE pdrv, BYTE* buf, LBA_t sector, UINT count) {
    if (pdrv >= FF_VOLUMES || !g_diskio[pdrv].read) return RES_PARERR;
    return g_diskio[pdrv].read(pdrv, buf, sector, count);
}

DRESULT disk_write(BYTE pdrv, const BYTE* buf, LBA_t sector, UINT count) {
    if (pdrv >= FF_VOLUMES || !g_diskio[pdrv].write) return RES_PARERR;
    return g_diskio[pdrv].write(pdrv, buf, sector, count);
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buf) {
    if (pdrv >= FF_VOLUMES || !g_diskio[pdrv].ioctl) return RES_PARERR;
    return g_diskio[pdrv].ioctl(pdrv, cmd, buf);
}
