/* ***** BEGIN LICENSE BLOCK *****
*   Copyright (C) 2012-2016, Peter Hatina <phatina@gmail.com>
*
*   This program is free software; you can redistribute it and/or
*   modify it under the terms of the GNU General Public License as
*   published by the Free Software Foundation; either version 2 of
*   the License, or (at your option) any later version.
*
*   This program is distributed in the hope that it will be useful,
*   but WITHOUT ANY WARRANTY; without even the implied warranty of
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
*   GNU General Public License for more details.
*
*   You should have received a copy of the GNU General Public License
*   along with this program. If not, see <http://www.gnu.org/licenses/>.
* ***** END LICENSE BLOCK ***** */

#include <config.h>
#include <libproc.h>
#include <sys/param.h>
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <sstream>
#include <vector>
#include <cstring>
#include <cstdint>
#include <cstdlib>
extern "C" {
#  include <unistd.h>
#  include <sys/types.h>
#  define _DARWIN_USE_64_BIT_INODE
#  include <sys/stat.h>
}
#include "simple-mtpfs-fuse.h"
#include "simple-mtpfs-libmtp.h"
#include "simple-mtpfs-batch.h"
#include "simple-mtpfs-log.h"
#include "simple-mtpfs-mtp-device.h"
#include "simple-mtpfs-util.h"

uint32_t MTPDevice::s_root_node = ~0;

// Name of the process behind the current FUSE request (for diagnostics).
static std::string smtpfs_requester()
{
    struct fuse_context *ctx = fuse_get_context();
    if (!ctx || ctx->pid <= 0)
        return "pid?";
    char name[2 * MAXCOMLEN + 1] = {0};
    if (proc_name(ctx->pid, name, sizeof(name)) <= 0)
        return "pid " + std::to_string(ctx->pid);
    return std::string(name) + "/" + std::to_string(ctx->pid);
}

MTPDevice::MTPDevice():
    m_device(nullptr),
    m_capabilities(),
    m_device_lock(),
    m_root_dir(),
    m_move_enabled(false)
{
    StreamHelper::off();
    LIBMTP_Init();
    StreamHelper::on();
}

MTPDevice::~MTPDevice()
{
    disconnect();
}

bool MTPDevice::connect(LIBMTP_raw_device_t *dev)
{
    if (m_device) {
        logerr("Already connected.\n");
        return true;
    }

    // Do not output LIBMTP debug stuff
    StreamHelper::off();
    m_device = LIBMTP_Open_Raw_Device_Uncached(dev);
    StreamHelper::on();

    if (!m_device) {
        LIBMTP_Dump_Errorstack(m_device);
        return false;
    }

    if (!enumStorages())
        return false;

    // Retrieve capabilities.
    m_capabilities = MTPDevice::getCapabilities(*this);

    logmsg("Connected.\n");
    return true;
}

bool MTPDevice::connect_priv(int dev_no, const std::string &dev_file)
{
    if (m_device) {
        logerr("Already connected.\n");
        return true;
    }

    int raw_devices_cnt;
    LIBMTP_raw_device_t *raw_devices;

    // Do not output LIBMTP debug stuff
    StreamHelper::off();
    LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(
        &raw_devices, &raw_devices_cnt);
    StreamHelper::on();

    if (err != LIBMTP_ERROR_NONE) {
        switch(err) {
        case LIBMTP_ERROR_NO_DEVICE_ATTACHED:
            logerr("No raw devices found.\n");
            break;
        case LIBMTP_ERROR_CONNECTING:
            logerr("There has been an error connecting. Exiting.\n");
            break;
        case LIBMTP_ERROR_MEMORY_ALLOCATION:
            logerr("Encountered a Memory Allocation Error. Exiting.\n");
            break;
        case LIBMTP_ERROR_GENERAL:
            logerr("General error occurred. Exiting.\n");
            break;
        case LIBMTP_ERROR_USB_LAYER:
            logerr("USB Layer error occurred. Exiting.\n");
            break;
        default:
            break;
        }
        return false;
    }

#ifndef HAVE_LIBUSB1
    if (!dev_file.empty()) {
        uint8_t bnum, dnum;
        dev_no = raw_devices_cnt;

        if (smtpfs_usb_devpath(dev_file, &bnum, &dnum))
            for (dev_no = 0; dev_no < raw_devices_cnt; ++dev_no)
                if (bnum == raw_devices[dev_no].bus_location &&
                    dnum == raw_devices[dev_no].devnum)
                    break;

        if (dev_no == raw_devices_cnt) {
            logerr("Can not open such device '", dev_file, "'.\n");
            free(static_cast<void*>(raw_devices));
            return false;
        }
    }
#endif // !HAVE_LIBUSB1

    if (dev_no < 0 || dev_no >= raw_devices_cnt) {
        logerr("Can not connect to device no. ", dev_no + 1, ".\n");
        free(static_cast<void*>(raw_devices));
        return false;
    }

    LIBMTP_raw_device_t *raw_device = &raw_devices[dev_no];

    // Do not output LIBMTP debug stuff
    StreamHelper::off();
    m_device = LIBMTP_Open_Raw_Device_Uncached(raw_device);
    StreamHelper::on();
    free(static_cast<void*>(raw_devices));

    if (!m_device) {
        LIBMTP_Dump_Errorstack(m_device);
        return false;
    }

    if (!enumStorages())
        return false;

    // Retrieve capabilities.
    m_capabilities = MTPDevice::getCapabilities(*this);

    logmsg("Connected.\n");
    return true;
}

bool MTPDevice::connect(int dev_no)
{
    return connect_priv(dev_no, std::string());
}

#ifdef HAVE_LIBUSB1
bool MTPDevice::connect(const std::string &dev_file)
{
    if (m_device) {
        logerr("Already connected.\n");
        return true;
    }

    LIBMTP_raw_device_t *raw_device = smtpfs_raw_device_new(dev_file);
    if (!raw_device) {
        logerr("Can not open such device '", dev_file, "'.\n");
        return false;
    }

    bool rval = connect(raw_device);

    // TODO:  Smart pointer with alloc, free hooks.
    smtpfs_raw_device_free(raw_device);

    return rval;
}
#else
bool MTPDevice::connect(const std::string &dev_file)
{
    return connect_priv(-1, dev_file);
}
#endif

void MTPDevice::disconnect()
{
    if (!m_device)
        return;

    LIBMTP_Release_Device(m_device);
    m_device = nullptr;
    logmsg("Disconnected.\n");
}

uint64_t MTPDevice::storageTotalSize() const
{
    uint64_t total = 0;
    for (LIBMTP_devicestorage_t *s = m_device->storage; s; s = s->next)
        total += s->MaxCapacity;
    return total;
}

uint64_t MTPDevice::storageFreeSize() const
{
    uint64_t free = 0;
    for (LIBMTP_devicestorage_t *s = m_device->storage; s; s = s->next)
        free += s->FreeSpaceInBytes;
    return free;
}

bool MTPDevice::enumStorages()
{
    criticalEnter();
    LIBMTP_Clear_Errorstack(m_device);
    if (LIBMTP_Get_Storage(m_device, LIBMTP_STORAGE_SORTBY_NOTSORTED) < 0) {
        std::cerr << "Could not retrieve device storage.\n";
        std::cerr << "For android phones make sure the screen is unlocked.\n";
        logerr("Could not retrieve device storage. Exiting.\n");
        LIBMTP_Dump_Errorstack(m_device);
        LIBMTP_Clear_Errorstack(m_device);
        return false;
    }
    criticalLeave();
    return true;
}

const TypeDir *MTPDevice::dirFetchContent(std::string path)
{
    initRoot();

    if (m_root_dir.dirCount() == 1)
        path = '/' + m_root_dir.firstDirName() + path;

    if (path == "/")
        return &m_root_dir;

    std::string member;
    std::istringstream ss(path);
    TypeDir *dir = &m_root_dir;
    while (std::getline(ss, member, '/')) {
        if (member.empty())
            continue;

        const TypeDir *tmp = dir->dir(member);
        if (!tmp && !dir->isFetched()) {
            fetchDirContent(dir, path);
            tmp = dir->dir(member);
        }

        if (!tmp)
            return nullptr;
        dir = const_cast<TypeDir*>(tmp);
    }

    if (dir->isFetched())
        return dir;

    fetchDirContent(dir, path);
    return dir;
}

void MTPDevice::initRoot()
{
    if (m_root_dir.isFetched())
        return;
    PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
    if (m_root_dir.isFetched())
        return;
    for (LIBMTP_devicestorage_t *s = m_device->storage; s; s = s->next) {
        m_root_dir.addDir(TypeDir(s_root_node, 0, s->id,
            std::string(s->StorageDescription ? s->StorageDescription : "Storage")));
    }
    m_root_dir.setFetched(); // only after every storage is in
}

void MTPDevice::dumpErrorstack()
{
    PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
    LIBMTP_Dump_Errorstack(m_device);
    LIBMTP_Clear_Errorstack(m_device);
}

void MTPDevice::fetchDirContent(TypeDir *dir, const std::string &path)
{
    const auto t0 = std::chrono::steady_clock::now();
    bool batched = false;
    int count = 0;

    {
    PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
    // Another thread may have fetched this folder while we waited for the device.
    if (dir->isFetched())
        return;
    // The storage root has no object handle of its own; keep libmtp's path there.
    if (dir->id() != s_root_node) {
        smtpfs_entry_t *entries = nullptr;
        if (smtpfs_list_children_batched(m_device, dir->storageid(), dir->id(),
                &entries, &count) == 0) {
            for (int i = 0; i < count; ++i) {
                const smtpfs_entry_t &e = entries[i];
                if (e.is_folder) {
                    TypeDir d(e.handle, dir->id(), dir->storageid(), std::string(e.name));
                    d.setModificationDate(e.mtime);
                    dir->addDir(d);
                } else {
                    dir->addFile(TypeFile(e.handle, dir->id(), dir->storageid(),
                        std::string(e.name), e.size, e.mtime));
                }
            }
            smtpfs_free_entries(entries, count);
            batched = true;
        }
    }
    if (!batched) {
        count = 0;
        LIBMTP_file_t *content = LIBMTP_Get_Files_And_Folders(
            m_device, dir->storageid(), dir->id());
        for (LIBMTP_file_t *f = content; f; f = f->next) {
            if (f->filetype == LIBMTP_FILETYPE_FOLDER)
                dir->addDir(TypeDir(f));
            else
                dir->addFile(TypeFile(f));
            ++count;
        }
        LIBMTP_Free_Files_And_Folders(&content);
    }
    dir->setFetched();
    }

    const double ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - t0).count();
    if (smtpfs_trace_enabled()) fprintf(stderr, "[list] %s: %d entries in %.1f ms (%s) for %s\n", path.c_str(), count, ms,
        batched ? "batched" : "per-object", smtpfs_requester().c_str());
}

int MTPDevice::dirCreateNew(const std::string &path)
{
    const std::string tmp_basename(smtpfs_basename(path));
    const std::string tmp_dirname(smtpfs_dirname(path));
    const TypeDir *dir_parent = dirFetchContent(tmp_dirname);
    if (!dir_parent || dir_parent->id() == 0) {
        logerr("Can not remove directory '", path, "'.\n");
        return -EINVAL;
    }
    char *c_name = strdup(tmp_basename.c_str());
    criticalEnter();
    uint32_t new_id = LIBMTP_Create_Folder(m_device, c_name,
        dir_parent->id(), dir_parent->storageid());
    criticalLeave();
    if (new_id == 0) {
        logerr("Could not create directory '", path, "'.\n");
        dumpErrorstack();
    } else {
        const_cast<TypeDir*>(dir_parent)->addDir(TypeDir(new_id, dir_parent->id(),
            dir_parent->storageid(), tmp_basename));
        logmsg("Directory '", path, "' created.\n");
    }
    free(static_cast<void*>(c_name));
    return new_id != 0 ? 0 : -EINVAL;
}

int MTPDevice::dirRemove(const std::string &path)
{
    const std::string tmp_basename(smtpfs_basename(path));
    const std::string tmp_dirname(smtpfs_dirname(path));
    const TypeDir *dir_parent = dirFetchContent(tmp_dirname);
    const TypeDir *dir_to_remove = dir_parent ? dir_parent->dir(tmp_basename) : nullptr;
    if (!dir_parent || !dir_to_remove || dir_parent->id() == 0) {
        logerr("No such directory '", path, "' to remove.\n");
        return -ENOENT;
    }
    if (!dir_to_remove->isEmpty())
        return -ENOTEMPTY;
    criticalEnter();
    int rval = LIBMTP_Delete_Object(m_device, dir_to_remove->id());
    criticalLeave();
    if (rval != 0){
        logerr("Could not remove the directory '", path, "'.\n");
        dumpErrorstack();
        return -EINVAL;
    }
    const_cast<TypeDir*>(dir_parent)->removeDir(*dir_to_remove);
    logmsg("Folder '", path, "' removed.\n");
    return 0;
}

int MTPDevice::dirRename(const std::string &oldpath, const std::string &newpath)
{
    const std::string tmp_old_basename(smtpfs_basename(oldpath));
    const std::string tmp_old_dirname(smtpfs_dirname(oldpath));
    const std::string tmp_new_basename(smtpfs_basename(newpath));
    const std::string tmp_new_dirname(smtpfs_dirname(newpath));
    const TypeDir *dir_parent = dirFetchContent(tmp_old_dirname);
    const TypeDir *dir_to_rename = dir_parent ? dir_parent->dir(tmp_old_basename) : nullptr;
    if (!dir_parent || !dir_to_rename || dir_parent->id() == 0) {
        logerr("Can not rename '", tmp_old_basename, "' to '",
            tmp_new_basename, "'.\n");
        return -EINVAL;
    }
    if (tmp_old_dirname != tmp_new_dirname) {
        logerr("Can not move '", oldpath, "' to '", newpath, "'.\n");
        return -EINVAL;
    }

    LIBMTP_folder_t *folder = dir_to_rename->toLIBMTPFolder();
    criticalEnter();
    int ret = LIBMTP_Set_Folder_Name(m_device, folder, tmp_new_basename.c_str());
    criticalLeave();
    free(static_cast<void*>(folder->name));
    free(static_cast<void*>(folder));
    if (ret != 0) {
        logerr("Could not rename '", oldpath, "' to '",  tmp_new_basename, "'.\n");
        dumpErrorstack();
        return -EINVAL;
    }
    const_cast<TypeDir*>(dir_parent)->renameDir(*dir_to_rename, tmp_new_basename);
    logmsg("Directory '", oldpath, "' renamed to '", tmp_new_basename, "'.\n");
    return 0;
}

int MTPDevice::rename(const std::string &oldpath, const std::string &newpath)
{
#ifndef SMTPFS_MOVE_BY_SET_OBJECT_PROPERTY
    const std::string tmp_old_basename(smtpfs_basename(oldpath));
    const std::string tmp_old_dirname(smtpfs_dirname(oldpath));
    const std::string tmp_new_dirname(smtpfs_dirname(newpath));
    if (tmp_old_dirname != tmp_new_dirname)
        return moveObject(oldpath, newpath);

    const TypeDir *dir_parent = dirFetchContent(tmp_old_dirname);
    if (!dir_parent || dir_parent->id() == 0)
        return -EINVAL;
    const TypeDir *dir_to_rename = dir_parent->dir(tmp_old_basename);
    if (dir_to_rename)
        return dirRename(oldpath, newpath);
    else
        return fileRename(oldpath, newpath);
#else
    const std::string tmp_old_basename(smtpfs_basename(oldpath));
    const std::string tmp_old_dirname(smtpfs_dirname(oldpath));
    const std::string tmp_new_basename(smtpfs_basename(newpath));
    const std::string tmp_new_dirname(smtpfs_dirname(newpath));
    const TypeDir *dir_old_parent = dirFetchContent(tmp_old_dirname);
    const TypeDir *dir_new_parent = dirFetchContent(tmp_new_dirname);
    const TypeDir *dir_to_rename = dir_old_parent ? dir_old_parent->dir(tmp_old_basename) : nullptr;
    const TypeFile *file_to_rename = dir_old_parent ? dir_old_parent->file(tmp_old_basename) : nullptr;

    logdebug("dir_to_rename:    ", dir_to_rename, "\n");
    logdebug("file_to_rename:   ", file_to_rename, "\n");

    if (!dir_old_parent || !dir_new_parent || dir_old_parent->id() == 0)
        return -EINVAL;

    const TypeBasic *object_to_rename =  dir_to_rename ?
        static_cast<const TypeBasic*>(dir_to_rename) :
        static_cast<const TypeBasic*>(file_to_rename);

    logdebug("object_to_rename: ", object_to_rename, "\n");
    logdebug("object_to_rename->id(): ", object_to_rename->id(), "\n");

    if (!object_to_rename) {
        logerr("No such file or directory to rename/move!\n");
        return -ENOENT;
    }

    if (tmp_old_dirname != tmp_new_dirname) {
        criticalEnter();
        int rval = LIBMTP_Set_Object_u32(m_device, object_to_rename->id(),
            LIBMTP_PROPERTY_ParentObject, dir_new_parent->id());
        criticalLeave();
        if (rval != 0) {
            logerr("Could not move '", oldpath, "' to '", newpath, "'.\n");
            dumpErrorstack();
            return -EINVAL;
        }
        const_cast<TypeBasic*>(object_to_rename)->setParent(dir_new_parent->id());
    }
    if (tmp_old_basename != tmp_new_basename) {
        criticalEnter();
        int rval = LIBMTP_Set_Object_String(m_device, object_to_rename->id(),
            LIBMTP_PROPERTY_Name, tmp_new_basename.c_str());
        criticalLeave();
        if (rval != 0) {
            logerr("Could not rename '", oldpath, "' to '", newpath, "'.\n");
            dumpErrorstack();
            return -EINVAL;
        }
    }
    return 0;
#endif
}

int MTPDevice::fileRead(const std::string &path, char *buf, size_t size,
    off_t offset)
{
    const std::string path_basename(smtpfs_basename(path));
    const std::string path_dirname(smtpfs_dirname(path));
    const TypeDir *dir_parent = dirFetchContent(path_dirname);
    const TypeFile *file_to_fetch = dir_parent ?
        dir_parent->file(path_basename) : nullptr;
    if (!dir_parent) {
        logerr("Can not fetch '", path, "'.\n");
        return -EINVAL;
    }
    if (!file_to_fetch) {
        logerr("No such file '", path, "'.\n");
        return -ENOENT;
    }

    // all systems clear
    unsigned char *tmp_buf = nullptr;
    unsigned int tmp_size = 0;
    const uint32_t id = file_to_fetch->id();
    int rval;
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::Low);
        rval = LIBMTP_GetPartialObject(m_device, id, offset, size, &tmp_buf, &tmp_size);
    }
    if (tmp_size > size)
        tmp_size = static_cast<unsigned int>(size);
    if (tmp_buf) {
        if (tmp_size > 0)
            memcpy(buf, tmp_buf, tmp_size);
        free(tmp_buf);
    }

    if (rval != 0)
        return -EIO;
    return tmp_size;
}

int MTPDevice::fileWrite(const std::string &path, const char *buf, size_t size,
    off_t offset)
{
    const std::string path_basename(smtpfs_basename(path));
    const std::string path_dirname(smtpfs_dirname(path));
    const TypeDir *dir_parent = dirFetchContent(path_dirname);
    const TypeFile *file_to_fetch = dir_parent ?
        dir_parent->file(path_basename) : nullptr;
    if (!dir_parent) {
        logerr("Can not fetch '", path, "'.\n");
        return -EINVAL;
    }
    if (!file_to_fetch) {
        logerr("No such file '", path, "'.\n");
        return -ENOENT;
    }

    // all systems clear
    const uint32_t id = file_to_fetch->id();
    int rval;
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::Low);
        rval = LIBMTP_SendPartialObject(m_device, id, offset, (unsigned char *) buf, size);
    }

    if (rval < 0)
        return -EIO;
    return size;
}

int MTPDevice::filePull(const std::string &src, const std::string &dst)
{
    const std::string src_basename(smtpfs_basename(src));
    const std::string src_dirname(smtpfs_dirname(src));
    const TypeDir *dir_parent = dirFetchContent(src_dirname);
    const TypeFile *file_to_fetch = dir_parent ? dir_parent->file(src_basename) : nullptr;
    if (!dir_parent) {
        logerr("Can not fetch '", src, "'.\n");
        return -EINVAL;
    }
    if (!file_to_fetch) {
        logerr("No such file '", src, "'.\n");
        return -ENOENT;
    }
    if (file_to_fetch->size() == 0) {
        int fd = ::creat(dst.c_str(), S_IRUSR | S_IWUSR);
        ::close(fd);
    } else {
        logmsg("Started fetching '", src, "'.\n");
        criticalEnter();
        int rval = LIBMTP_Get_File_To_File(m_device, file_to_fetch->id(),
            dst.c_str(), nullptr, nullptr);
        criticalLeave();
        if (rval != 0) {
            logerr("Could not fetch file '", src, "'.\n");
            dumpErrorstack();
            return -ENOENT;
        }
    }
    logmsg("File fetched '", src, "'.\n");
    return 0;
}

// Upload src as dst without ever losing the old dst: send under a hidden temporary name
// first, delete the old object only after the upload succeeded, then rename. If the final
// rename fails, the data stays on the phone under the temporary name (logged).
int MTPDevice::filePush(const std::string &src, const std::string &dst)
{
    const std::string dst_basename(smtpfs_basename(dst));
    const std::string dst_dirname(smtpfs_dirname(dst));
    const TypeDir *dir_parent = dirFetchContent(dst_dirname);
    if (!dir_parent || dir_parent->id() == 0) {
        logerr("Can not upload '", src, "' to '", dst, "': no parent folder.\n");
        return -ENOENT;
    }
    if (dir_parent->dir(dst_basename))
        return -EISDIR;

    struct stat file_stat;
    if (::stat(src.c_str(), &file_stat) != 0)
        return -errno;

    char suffix[32];
    snprintf(suffix, sizeof(suffix), ".mtpfs-%08x", arc4random());
    const std::string tmp_name = "." + dst_basename + suffix;

    // Test hook: make the upload of one file name fail, to exercise the failure path.
    const char *fail_name = getenv("SMTPFS_TEST_FAIL_UPLOAD");
    if (fail_name && dst_basename == fail_name) {
        logerr("Test hook: failing upload of '", dst, "'.\n");
        return -EIO;
    }

    // 1) send the new content under the temporary name
    TypeFile file_to_upload(0, dir_parent->id(), dir_parent->storageid(),
        tmp_name, static_cast<uint64_t>(file_stat.st_size), file_stat.st_mtime);
    LIBMTP_file_t *f = file_to_upload.toLIBMTPFile();
    int rval;
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
        rval = LIBMTP_Send_File_From_File(m_device, src.c_str(), f, nullptr, nullptr);
    }
    const uint32_t new_id = f->item_id;
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[push] %s: sent as %s, id %u, rval %d\n", dst.c_str(),
            tmp_name.c_str(), new_id, rval);
    if (rval != 0) {
        logerr("Could not upload file '", src, "' to '", dst, "'; old file untouched.\n");
        dumpErrorstack();
        free(static_cast<void*>(f->filename));
        free(static_cast<void*>(f));
        return -EIO;
    }

    // 2) delete the old object, now that the new one is safely on the phone
    const TypeFile *old_file = dir_parent->file(dst_basename);
    if (old_file) {
        const uint32_t old_id = old_file->id();
        {
            PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
            rval = LIBMTP_Delete_Object(m_device, old_id);
        }
        if (rval != 0) {
            logerr("Could not replace '", dst, "': old object not deleted; removing the upload.\n");
            dumpErrorstack();
            PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
            LIBMTP_Delete_Object(m_device, new_id);
            free(static_cast<void*>(f->filename));
            free(static_cast<void*>(f));
            return -EIO;
        }
        const_cast<TypeDir*>(dir_parent)->removeFile(*old_file);
    }

    // 3) give the new object its real name
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
        rval = LIBMTP_Set_File_Name(m_device, f, dst_basename.c_str());
    }
    file_to_upload.setId(new_id);
    file_to_upload.setParent(dir_parent->id());
    file_to_upload.setStorage(dir_parent->storageid());
    if (rval != 0) {
        logerr("Uploaded '", dst, "' but could not rename it; it is on the phone as '",
            tmp_name, "'.\n");
        fprintf(stderr, "simple-mtpfs: %s is on the phone as %s/%s\n", dst.c_str(),
            dst_dirname.c_str(), tmp_name.c_str());
        dumpErrorstack();
        const_cast<TypeDir*>(dir_parent)->addFile(file_to_upload); // visible under tmp name
        rval = -EIO;
    } else {
        file_to_upload.setName(dst_basename);
        const_cast<TypeDir*>(dir_parent)->addFile(file_to_upload);
    }
    free(static_cast<void*>(f->filename));
    free(static_cast<void*>(f));
    return rval;
}

int MTPDevice::fileRemove(const std::string &path)
{
    const std::string tmp_basename(smtpfs_basename(path));
    const std::string tmp_dirname(smtpfs_dirname(path));
    const TypeDir *dir_parent = dirFetchContent(tmp_dirname);
    const TypeFile *file_to_remove = dir_parent ? dir_parent->file(tmp_basename) : nullptr;
    if (!dir_parent || !file_to_remove) {
        logerr("No such file '", path, "' to remove.\n");
        return -ENOENT;
    }
    criticalEnter();
    int rval = LIBMTP_Delete_Object(m_device, file_to_remove->id());
    criticalLeave();
    if (rval != 0) {
        logerr("Could not remove the directory '", path, "'.\n");
        return -EINVAL;
    }
    const_cast<TypeDir*>(dir_parent)->removeFile(*file_to_remove);
    logmsg("File '", path, "' removed.\n");
    return 0;
}

int MTPDevice::fileRename(const std::string &oldpath, const std::string &newpath)
{
    const std::string tmp_old_basename(smtpfs_basename(oldpath));
    const std::string tmp_old_dirname(smtpfs_dirname(oldpath));
    const std::string tmp_new_basename(smtpfs_basename(newpath));
    const std::string tmp_new_dirname(smtpfs_dirname(newpath));
    const TypeDir *dir_parent = dirFetchContent(tmp_old_dirname);
    const TypeFile *file_to_rename = dir_parent ? dir_parent->file(tmp_old_basename) : nullptr;
    if (!dir_parent || !file_to_rename || tmp_old_dirname != tmp_new_dirname) {
        logerr("Can not rename '", oldpath, "' to '", tmp_new_basename, "'.\n");
        return -ENOENT;
    }
    if (dir_parent->dir(tmp_new_basename))
        return -EISDIR;

    // POSIX rename replaces an existing target; MTP refuses duplicate names, so delete it
    // first. The source keeps its data whatever happens next.
    const TypeFile *target = dir_parent->file(tmp_new_basename);
    if (target && target != file_to_rename) {
        const uint32_t target_id = target->id();
        int r;
        {
            PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
            r = LIBMTP_Delete_Object(m_device, target_id);
        }
        if (r != 0) {
            logerr("Could not replace '", newpath, "'.\n");
            dumpErrorstack();
            return -EIO;
        }
        const_cast<TypeDir*>(dir_parent)->removeFile(*target);
    }

    LIBMTP_file_t *file = file_to_rename->toLIBMTPFile();
    int rval;
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
        rval = LIBMTP_Set_File_Name(m_device, file, tmp_new_basename.c_str());
    }
    free(static_cast<void*>(file->filename));
    free(static_cast<void*>(file));
    if (rval != 0) { // libmtp returns -1 on failure
        logerr("Could not rename '", oldpath, "' to '", newpath, "'.\n");
        dumpErrorstack();
        return -EIO;
    }
    const_cast<TypeDir*>(dir_parent)->renameFile(*file_to_rename, tmp_new_basename);
    logmsg("File '", oldpath, "' renamed to '", tmp_new_basename, "'.\n");
    return 0;
}

// Move to another folder with MTP MoveObject (no data transfer). Returns -EXDEV when the
// phone cannot move, so that the caller (mv, Finder) falls back to copy + delete.
int MTPDevice::moveObject(const std::string &oldpath, const std::string &newpath)
{
    const std::string old_base(smtpfs_basename(oldpath)), new_base(smtpfs_basename(newpath));
    const TypeDir *old_parent = dirFetchContent(smtpfs_dirname(oldpath));
    const TypeDir *new_parent = dirFetchContent(smtpfs_dirname(newpath));
    if (!old_parent || !new_parent || old_parent->id() == 0 || new_parent->id() == 0)
        return -ENOENT;
    if (old_parent->storageid() != new_parent->storageid())
        return -EXDEV;
    const TypeDir *dir = old_parent->dir(old_base);
    const TypeFile *file = dir ? nullptr : old_parent->file(old_base);
    if (!dir && !file)
        return -ENOENT;
    if (new_parent->dir(new_base))
        return -EEXIST;
    const TypeFile *target = new_parent->file(new_base);
    if (target && dir)
        return -ENOTDIR;

    const uint32_t id = dir ? dir->id() : file->id();
    // MTP: parent 0 = storage root
    const uint32_t dest_parent = new_parent->id() == s_root_node ? 0 : new_parent->id();
    if (target) {
        const uint32_t target_id = target->id();
        int r;
        {
            PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
            r = LIBMTP_Delete_Object(m_device, target_id);
        }
        if (r != 0) {
            dumpErrorstack();
            return -EIO;
        }
        const_cast<TypeDir*>(new_parent)->removeFile(*target);
    }
    int rval;
    {
        PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
        rval = LIBMTP_Move_Object(m_device, id, new_parent->storageid(), dest_parent);
    }
    if (rval != 0) {
        logerr("Phone refused to move '", oldpath, "'; letting the caller copy it.\n");
        dumpErrorstack();
        return -EXDEV;
    }
    if (old_base != new_base) {
        int r;
        {
            PriorityLockGuard guard(m_device_lock, PriorityLockGuard::High);
            r = LIBMTP_Set_Object_String(m_device, id, LIBMTP_PROPERTY_ObjectFileName,
                new_base.c_str());
        }
        if (r != 0) {
            logerr("Moved '", oldpath, "' but could not rename it to '", new_base, "'.\n");
            dumpErrorstack();
        }
    }
    // tree: gone from the old folder; in the new one as a fresh node
    if (dir) {
        TypeDir moved(id, new_parent->id(), new_parent->storageid(), new_base);
        moved.setModificationDate(dir->modificationDate());
        const_cast<TypeDir*>(old_parent)->removeDir(*dir);
        if (new_parent->isFetched())
            const_cast<TypeDir*>(new_parent)->addDir(moved);
    } else {
        TypeFile moved(id, new_parent->id(), new_parent->storageid(), new_base,
            file->size(), file->modificationDate());
        const_cast<TypeDir*>(old_parent)->removeFile(*file);
        if (new_parent->isFetched())
            const_cast<TypeDir*>(new_parent)->addFile(moved);
    }
    return 0;
}

MTPDevice::Capabilities MTPDevice::getCapabilities() const
{
    return m_capabilities;
}

MTPDevice::Capabilities MTPDevice::getCapabilities(const MTPDevice &device)
{
    MTPDevice::Capabilities capabilities;

#ifdef HAVE_LIBMTP_CHECK_CAPABILITY
    if (device.m_device) {
        capabilities.setCanGetPartialObject(
            static_cast<bool>(
                LIBMTP_Check_Capability(
                    device.m_device,
                    LIBMTP_DEVICECAP_GetPartialObject)));
        capabilities.setCanSendPartialobject(
            static_cast<bool>(
                LIBMTP_Check_Capability(
                    device.m_device,
                    LIBMTP_DEVICECAP_SendPartialObject)));
        capabilities.setCanEditObjects(
            static_cast<bool>(
                LIBMTP_Check_Capability(
                    device.m_device,
                    LIBMTP_DEVICECAP_EditObjects)));
    }
#endif

    return capabilities;
}

bool MTPDevice::listDevices(bool verbose, const std::string &dev_file)
{
    int raw_devices_cnt;
    LIBMTP_raw_device_t *raw_devices;

    // Do not output LIBMTP debug stuff
    StreamHelper::off();
    LIBMTP_error_number_t err = LIBMTP_Detect_Raw_Devices(
        &raw_devices, &raw_devices_cnt);
    StreamHelper::on();

    if (err != 0) {
        if (err == LIBMTP_ERROR_NO_DEVICE_ATTACHED)
            std::cerr << "No raw devices found.\n";
        return false;
    }

    uint8_t bnum, dnum;
    if (!dev_file.empty() && !smtpfs_usb_devpath(dev_file, &bnum, &dnum)) {
        std::cerr << "Can not open such device '" << dev_file << "'.\n";
        return false;
    }

    for (int i = 0; i < raw_devices_cnt; ++i) {
        if (!dev_file.empty() &&
            !(bnum == raw_devices[i].bus_location && dnum == raw_devices[i].devnum))
            continue;
        std::cout << i + 1 << ": "
            << (raw_devices[i].device_entry.vendor ? raw_devices[i].device_entry.vendor : "Unknown vendor ")
            << ", "
            << (raw_devices[i].device_entry.product ? raw_devices[i].device_entry.product : "Unknown product")
            << std::endl;
#ifdef HAVE_LIBMTP_CHECK_CAPABILITY
            MTPDevice dev;
            if (verbose) {
                if (!dev.connect(&raw_devices[i]))
                    return false;

                const MTPDevice::Capabilities &cap = dev.getCapabilities();
                std::cout << "  - can get  partial object: " << (cap.canGetPartialObject() ? "yes" : "no") << std::endl;
                std::cout << "  - can send partial object: " << (cap.canSendPartialObject() ? "yes" : "no") << std::endl;
                std::cout << "  - can edit objects       : " << (cap.canEditObjects() ? "yes" : "no") << std::endl;
                dev.disconnect();
            }
#endif
    }
    free(static_cast<void*>(raw_devices));

    return true;
}
