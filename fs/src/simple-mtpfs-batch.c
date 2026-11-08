/* Batched folder listing over MTP GetObjectPropList (depth 1). See simple-mtpfs-batch.h. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ptp.h"
#include "simple-mtpfs-batch.h"

static int cmp_entry(const void *a, const void *b)
{
    uint32_t x = ((const smtpfs_entry_t *) a)->handle;
    uint32_t y = ((const smtpfs_entry_t *) b)->handle;
    return x < y ? -1 : x > y;
}

static smtpfs_entry_t *find_entry(smtpfs_entry_t *e, int n, uint32_t handle)
{
    smtpfs_entry_t key = { .handle = handle };
    return bsearch(&key, e, (size_t) n, sizeof(*e), cmp_entry);
}

/* MTP DateTime "YYYYMMDDThhmmss[.s][Z|+hhmm]"; interpreted like libmtp does
 * (local time unless it ends in Z). */
static time_t parse_mtp_time(const char *s)
{
    struct tm tm;
    if (!s)
        return 0;
    memset(&tm, 0, sizeof(tm));
    if (sscanf(s, "%4d%2d%2dT%2d%2d%2d", &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
               &tm.tm_hour, &tm.tm_min, &tm.tm_sec) != 6)
        return 0;
    tm.tm_year -= 1900;
    tm.tm_mon -= 1;
    tm.tm_isdst = -1;
    if (strchr(s, 'Z'))
        return timegm(&tm);
    return mktime(&tm);
}

static uint16_t get_prop(PTPParams *params, uint32_t parent, uint32_t prop,
                         MTPProperties **props, int *n)
{
    *props = NULL;
    *n = 0;
    return ptp_mtp_getobjectproplist_generic(params, parent, 0, prop, 0, 1, props, n);
}

int smtpfs_list_children_batched(LIBMTP_mtpdevice_t *device, uint32_t storage,
    uint32_t parent, smtpfs_entry_t **out, int *count)
{
    PTPParams *params = (PTPParams *) device->params;
    MTPProperties *p = NULL;
    int np = 0, n = 0;
    smtpfs_entry_t *e = NULL;
    PTPObjectHandles handles = { 0, NULL };

    *out = NULL;
    *count = 0;

    /* Authoritative child list, used to check the batched answers are complete. */
    if (ptp_getobjecthandles(params, storage, 0x000000, parent, &handles) != PTP_RC_OK)
        return -1;

    /* 1) names define the entries; depth 1 also returns the parent itself, skip it */
    if (get_prop(params, parent, PTP_OPC_ObjectFileName, &p, &np) != PTP_RC_OK)
        goto fail;
    e = calloc((size_t) (np > 0 ? np : 1), sizeof(*e));
    if (!e)
        goto fail;
    for (int i = 0; i < np; i++) {
        if (p[i].ObjectHandle == parent || !p[i].propval.str)
            continue;
        e[n].handle = p[i].ObjectHandle;
        e[n].name = strdup(p[i].propval.str);
        n++;
    }
    ptp_destroy_object_prop_list(p, np);
    p = NULL;
    qsort(e, (size_t) n, sizeof(*e), cmp_entry);

    if ((uint32_t) n != handles.n)
        goto fail; /* incomplete answer: let libmtp do it the slow, safe way */
    for (uint32_t i = 0; i < handles.n; i++)
        if (!find_entry(e, n, handles.Handler[i]))
            goto fail;

    /* 2) format -> folder or file */
    if (get_prop(params, parent, PTP_OPC_ObjectFormat, &p, &np) != PTP_RC_OK)
        goto fail;
    for (int i = 0; i < np; i++) {
        smtpfs_entry_t *x = find_entry(e, n, p[i].ObjectHandle);
        if (x)
            x->is_folder = (p[i].propval.u16 == PTP_OFC_Association);
    }
    ptp_destroy_object_prop_list(p, np);
    p = NULL;

    /* 3) size (UINT64 on MTP, be lenient) */
    if (get_prop(params, parent, PTP_OPC_ObjectSize, &p, &np) != PTP_RC_OK)
        goto fail;
    for (int i = 0; i < np; i++) {
        smtpfs_entry_t *x = find_entry(e, n, p[i].ObjectHandle);
        if (!x)
            continue;
        x->size = p[i].datatype == PTP_DTC_UINT64 ? p[i].propval.u64 : p[i].propval.u32;
    }
    ptp_destroy_object_prop_list(p, np);
    p = NULL;

    /* 4) modification date */
    if (get_prop(params, parent, PTP_OPC_DateModified, &p, &np) != PTP_RC_OK)
        goto fail;
    for (int i = 0; i < np; i++) {
        smtpfs_entry_t *x = find_entry(e, n, p[i].ObjectHandle);
        if (x && p[i].datatype == PTP_DTC_STR)
            x->mtime = parse_mtp_time(p[i].propval.str);
    }
    ptp_destroy_object_prop_list(p, np);
    p = NULL;

    free(handles.Handler);
    *out = e;
    *count = n;
    return 0;

fail:
    if (p)
        ptp_destroy_object_prop_list(p, np);
    smtpfs_free_entries(e, n);
    free(handles.Handler);
    return -1;
}

void smtpfs_free_entries(smtpfs_entry_t *entries, int count)
{
    if (!entries)
        return;
    for (int i = 0; i < count; i++)
        free(entries[i].name);
    free(entries);
}
