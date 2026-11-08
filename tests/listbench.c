/*
 * Read-only benchmark: list one folder on an MTP device two ways.
 *  A) LIBMTP_Get_Files_And_Folders  - libmtp's per-object path (GetObjectInfo per file)
 *  B) GetObjectPropList, depth 1    - all properties of all children in ONE transaction
 *
 * Usage: listbench [folder path, default /DCIM/Camera]
 * Nothing is written to the device.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "libmtp.h"
#include "ptp.h"

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

static uint32_t find_child_folder(LIBMTP_mtpdevice_t *dev, uint32_t storage,
                                  uint32_t parent, const char *name)
{
    uint32_t id = 0;
    LIBMTP_file_t *f = LIBMTP_Get_Files_And_Folders(dev, storage, parent);
    while (f) {
        LIBMTP_file_t *next = f->next;
        if (!id && f->filetype == LIBMTP_FILETYPE_FOLDER && strcmp(f->filename, name) == 0)
            id = f->item_id;
        LIBMTP_destroy_file_t(f);
        f = next;
    }
    return id;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "/DCIM/Camera";
    LIBMTP_raw_device_t *raw = NULL;
    int nraw = 0;

    LIBMTP_Init();
    if (LIBMTP_Detect_Raw_Devices(&raw, &nraw) != LIBMTP_ERROR_NONE || nraw < 1) {
        fprintf(stderr, "no MTP device\n");
        return 1;
    }
    printf("device flags from libmtp table: 0x%08x\n", raw[0].device_entry.device_flags);

    LIBMTP_mtpdevice_t *dev = LIBMTP_Open_Raw_Device_Uncached(&raw[0]);
    if (!dev) {
        fprintf(stderr, "cannot open device\n");
        return 1;
    }
    uint32_t storage = dev->storage ? dev->storage->id : 0;
    PTPParams *params = (PTPParams *) dev->params;

    /* walk the path */
    uint32_t folder = LIBMTP_FILES_AND_FOLDERS_ROOT;
    char *copy = strdup(path), *save = NULL;
    for (char *tok = strtok_r(copy, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
        folder = find_child_folder(dev, storage, folder, tok);
        if (!folder) {
            fprintf(stderr, "folder '%s' not found\n", tok);
            return 1;
        }
    }
    free(copy);
    printf("folder %s = handle 0x%08x\n\n", path, folder);

    /* A) per-object path */
    double t0 = now_ms();
    int na = 0;
    LIBMTP_file_t *f = LIBMTP_Get_Files_And_Folders(dev, storage, folder);
    while (f) {
        LIBMTP_file_t *next = f->next;
        na++;
        LIBMTP_destroy_file_t(f);
        f = next;
    }
    double ta = now_ms() - t0;
    printf("A) LIBMTP_Get_Files_And_Folders : %4d objects in %8.1f ms (%.1f ms/object)\n",
           na, ta, na ? ta / na : 0);

    /* B) one GetObjectPropList transaction, depth 1 = children of the folder */
    MTPProperties *props = NULL;
    int nprops = 0;
    t0 = now_ms();
    uint16_t rc = ptp_mtp_getobjectproplist_level(params, folder, 1, &props, &nprops);
    double tb = now_ms() - t0;
    if (rc != PTP_RC_OK) {
        printf("B) GetObjectPropList depth 1    : FAILED, response code 0x%04x after %.1f ms\n", rc, tb);
    } else {
        /* count distinct objects and show a sample */
        int nobj = 0, shown = 0;
        uint32_t last = 0xFFFFFFFF;
        for (int i = 0; i < nprops; i++) {
            if (props[i].ObjectHandle != last) {
                nobj++;
                last = props[i].ObjectHandle;
            }
        }
        printf("B) GetObjectPropList depth 1    : %4d objects (%d properties) in %8.1f ms\n",
               nobj, nprops, tb);
        printf("   sample:\n");
        for (int i = 0; i < nprops && shown < 3; i++) {
            if (props[i].property == PTP_OPC_ObjectFileName) {
                uint32_t h = props[i].ObjectHandle;
                uint64_t size = 0;
                const char *mod = "";
                for (int j = 0; j < nprops; j++) {
                    if (props[j].ObjectHandle != h)
                        continue;
                    if (props[j].property == PTP_OPC_ObjectSize)
                        size = props[j].datatype == PTP_DTC_UINT64 ? props[j].propval.u64 : props[j].propval.u32;
                    if (props[j].property == PTP_OPC_DateModified && props[j].propval.str)
                        mod = props[j].propval.str;
                }
                printf("   0x%08x %-40s %10llu  %s\n", h, props[i].propval.str ? props[i].propval.str : "?",
                       (unsigned long long) size, mod);
                shown++;
            }
        }
        ptp_destroy_object_prop_list(props, nprops);
    }


    /* C) one property at a time for all children (depth 1), and the handle list alone */
    {
        struct { uint32_t prop; const char *name; } v[] = {
            { PTP_OPC_ObjectFileName, "ObjectFileName" },
            { PTP_OPC_ObjectSize, "ObjectSize" },
            { PTP_OPC_DateModified, "DateModified" },
            { PTP_OPC_ObjectFormat, "ObjectFormat" },
        };
        double total = 0;
        for (unsigned k = 0; k < sizeof(v) / sizeof(v[0]); k++) {
            MTPProperties *pp = NULL;
            int np = 0;
            double s0 = now_ms();
            uint16_t r = ptp_mtp_getobjectproplist_generic(params, folder, 0, v[k].prop, 0, 1, &pp, &np);
            double dt = now_ms() - s0;
            total += dt;
            if (r == PTP_RC_OK) {
                printf("C) %-15s depth 1         : %4d values in %8.1f ms\n", v[k].name, np, dt);
                if (k == 0 && np > 0)
                    printf("   first: 0x%08x %s\n", pp[0].ObjectHandle, pp[0].propval.str ? pp[0].propval.str : "?");
                ptp_destroy_object_prop_list(pp, np);
            } else {
                printf("C) %-15s depth 1         : FAILED 0x%04x after %.1f ms\n", v[k].name, r, dt);
            }
        }
        printf("   C total: %.1f ms\n", total);

        PTPObjectHandles h;
        double s0 = now_ms();
        uint16_t r = ptp_getobjecthandles(params, storage, 0, folder, &h);
        double dt = now_ms() - s0;
        if (r == PTP_RC_OK) {
            printf("D) GetObjectHandles only        : %4u handles in %8.1f ms\n", h.n, dt);
            free(h.Handler);
        } else {
            printf("D) GetObjectHandles only        : FAILED 0x%04x\n", r);
        }
    }

    LIBMTP_Release_Device(dev);
    free(raw);
    return 0;
}
