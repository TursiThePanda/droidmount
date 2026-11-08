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

// Notification of change to source code by Su Z (https://github.com/su-z)
// Some modifications to this file are done on 29 Nov 2023, in particular adding the implementation of four functions related to extended attributes, so that the program works better on macOS

#ifndef SMTPFS_FUSE_H
#define SMTPFS_FUSE_H

#include <config.h>
#include <memory>
#include <condition_variable>
#include <map>
#include <mutex>
#include <set>
#include <vector>
#include <string>
#include <cstdlib>
extern "C" {
#  include <fuse.h>
}
#include "simple-mtpfs-mtp-device.h"
#include "simple-mtpfs-tmp-files-pool.h"
#include "simple-mtpfs-type-tmp-file.h"


class SMTPFileSystem
{
private:
    struct SMTPFileSystemOptions {
    public:
        int m_good;
        int m_help;
        int m_version;
        int m_verbose;
        int m_enable_move;
        int m_list_devices;
        int m_device_no;
        char *m_device_file;
        char *m_mount_point;

        SMTPFileSystemOptions();
        ~SMTPFileSystemOptions();

        static int opt_proc(void *data, const char *arg, int key,
            struct fuse_args *outargs);
    };

    SMTPFileSystem();

    enum {
        KEY_ENABLE_MOVE,
        KEY_DEVICE_NO,
        KEY_LIST_DEVICES,
        KEY_VERBOSE,
        KEY_VERSION,
        KEY_HELP
    };

public:
    ~SMTPFileSystem();

    static SMTPFileSystem *instance();

    bool parseOptions(int argc, char **argv);
    void printHelp() const;
    void printVersion() const;
    bool listDevices() const;

    bool exec();
    bool isGood() const { return m_options.m_good; }
    bool isHelp() const { return m_options.m_help; }
    bool isVersion() const { return m_options.m_version; }
    bool isListDevices() const { return m_options.m_list_devices; }

    int getattr(const char *path, struct stat *buf);
    int getattrTree(const char *path, struct stat *buf);
    int mknod(const char *path, mode_t mode, dev_t dev);
    int mkdir(const char *path, mode_t mode);
    int unlink(const char *path);
    int rmdir(const char *path);
    int rename(const char *path, const char *newpath);
    int chmod(const char *path, mode_t mode);
    int chown(const char *path, uid_t uid, gid_t gid);
    int truncate(const char *path, off_t new_size);
    int utime(const char *path, time_t modtime);
    int open(const char *path, struct fuse_file_info *file_info);
    int read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *file_info);
    int write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *file_info);
    int statfs(const char *path, struct statvfs *stat_info);
    int flush(const char *path, struct fuse_file_info *file_info);
    int release(const char *path, struct fuse_file_info *file_info);
    int fsync(const char *path, int datasync, struct fuse_file_info *fi);
    int opendir(const char *path, struct fuse_file_info *file_info);
    int readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *file_info);
    int releasedir(const char *path, struct fuse_file_info *file_info);
    int fsyncdir(const char *path, int datasync, struct fuse_file_info *file_info);
    int ftruncate(const char *path, off_t offset, struct fuse_file_info *file_info);
    void* init(struct fuse_conn_info *conn, struct fuse_config *cfg);
    int create(const char *path, mode_t mode, fuse_file_info *file_info);

private:
    static constexpr size_t kMaxReadChunk = 512 * 1024;

    bool hasPartialObjectSupport();

    static std::unique_ptr<SMTPFileSystem> s_instance;
    struct fuse_args m_args;
    struct fuse_operations m_fuse_operations;
    TmpFilesPool m_tmp_files_pool;
    // Serialises everything that touches m_tmp_files_pool (open/release/create/...).
    std::mutex m_pool_mutex;
    // File handles opened for writing: their data lives in the local temp file and is
    // sent to the phone in one piece when the last handle is released.
    std::set<uint64_t> m_write_fhs;
    // Device paths whose temp file holds the real content (not just a placeholder).
    std::set<std::string> m_content_paths;
    // Uploads in flight: device path -> temp file. A path is uploaded by one thread at a
    // time; open()/truncate() of that path wait on m_push_cv until it is done.
    std::map<std::string, std::string> m_pushing;
    std::condition_variable m_push_cv;

    // Attributes of a file that exists only locally (being written or uploaded).
    bool pendingAttr(const std::string &path, struct stat *buf);
    // Upload the local copy of path if it changed; waits for an upload already running.
    int pushNow(const std::string &path);
    // Keep a local copy whose upload failed, so nothing is lost; returns where it went.
    std::string rescueTmp(const std::string &tmp, const std::string &path);
    // Move pool bookkeeping of an open/pending file to a new path (caller holds the lock).
    void rekeyPending(const std::string &from, const std::string &to);
    std::vector<std::string> pendingNamesIn(const std::string &dir);
    SMTPFileSystemOptions m_options;
    MTPDevice m_device;
};

#endif // SMTPFS_FUSE_H
