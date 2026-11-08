/* Batched folder listing over MTP GetObjectPropList (depth 1).
 *
 * libmtp marks every Android device with DEVICE_FLAG_BROKEN_MTPGETOBJPROPLIST and then
 * fetches folder contents one object at a time (one USB round trip per file). Measured on
 * a Pixel 9 Pro, asking for ONE property for ALL children of a folder works
 * and is ~19x faster on a 173-file camera folder. Asking for all properties at once
 * fails on some folders (0xA801), so this asks four times, one property each.
 */
#ifndef SMTPFS_BATCH_H
#define SMTPFS_BATCH_H

#include <stdint.h>
#include <time.h>
#include <libmtp.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t handle;
    char *name;
    uint64_t size;
    time_t mtime;
    int is_folder;
} smtpfs_entry_t;

/* Returns 0 and fills *out / *count on success. Any non-zero return means the caller
 * must fall back to LIBMTP_Get_Files_And_Folders. The caller holds the device lock. */
int smtpfs_list_children_batched(LIBMTP_mtpdevice_t *device, uint32_t storage,
    uint32_t parent, smtpfs_entry_t **out, int *count);

void smtpfs_free_entries(smtpfs_entry_t *entries, int count);

#ifdef __cplusplus
}
#endif

#endif /* SMTPFS_BATCH_H */
