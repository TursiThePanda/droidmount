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

#include <config.h>
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#include <libproc.h>
#include <sys/param.h>
#include <cstdio>
#include <cstdlib>
#include <iostream>
extern "C" {
#  include <errno.h>
#  include <fuse.h>
#  include <unistd.h>
#  include <stddef.h>
}
#include "simple-mtpfs-fuse.h"
#include "simple-mtpfs-log.h"
#include "simple-mtpfs-util.h"

// libfuse3 port (2026-11-08): wrappers adapted to the FUSE 3 operation
// signatures, built with FUSE_DARWIN_ENABLE_EXTENSIONS=0 so that macFUSE's
// vanilla libfuse3 API is used. This allows the FSKit backend (-o backend=fskit).

int wrap_getattr(const char *path, struct stat *statbuf, struct fuse_file_info *)
{
    return SMTPFileSystem::instance()->getattr(path, statbuf);
}

static int wrap_setxattr(const char *path, const char *name, const char *, size_t, int)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] setxattr %s %s\n", path, name);
    return 0;
}

static int wrap_getxattr(const char *, const char *, char *, size_t)
{
    return -ENOATTR;
}

static int wrap_listxattr(const char *, char *, size_t)
{
    return 0;
}

static int wrap_removexattr(const char *, const char *)
{
    return -ENOATTR;
}

int wrap_mknod(const char *path, mode_t mode, dev_t dev)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_mknod %s\n", path);
    return SMTPFileSystem::instance()->mknod(path, mode, dev);
}

int wrap_mkdir(const char *path, mode_t mode)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_mkdir %s\n", path);
    return SMTPFileSystem::instance()->mkdir(path, mode);
}

int wrap_unlink(const char *path)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_unlink %s\n", path);
    return SMTPFileSystem::instance()->unlink(path);
}

int wrap_rmdir(const char *path)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_rmdir %s\n", path);
    return SMTPFileSystem::instance()->rmdir(path);
}

int wrap_rename(const char *path, const char *newpath, unsigned int flags)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_rename %s\n", (std::string(path)+" -> "+newpath).c_str());
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[trace] rename flags 0x%x\n", flags);
    if (flags & RENAME_EXCHANGE)
        return -EINVAL; // atomic swap is impossible over MTP
    if (flags & RENAME_NOREPLACE) {
        struct stat st;
        if (SMTPFileSystem::instance()->getattr(newpath, &st) == 0)
            return -EEXIST;
    }
    return SMTPFileSystem::instance()->rename(path, newpath);
}

int wrap_chmod(const char *path, mode_t mode, struct fuse_file_info *)
{
    return SMTPFileSystem::instance()->chmod(path, mode);
}

int wrap_chown(const char *path, uid_t uid, gid_t gid, struct fuse_file_info *)
{
    return SMTPFileSystem::instance()->chown(path, uid, gid);
}

int wrap_truncate(const char *path, off_t new_size, struct fuse_file_info *fi)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_truncate %s\n", path);
    if (fi)
        return SMTPFileSystem::instance()->ftruncate(path, new_size, fi);
    return SMTPFileSystem::instance()->truncate(path, new_size);
}

int wrap_utimens(const char *path, const struct timespec tv[2], struct fuse_file_info *)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_utimens %s\n", path);
    time_t mtime = time(nullptr);
    if (tv && tv[1].tv_nsec != UTIME_NOW && tv[1].tv_nsec != UTIME_OMIT)
        mtime = tv[1].tv_sec;
    if (tv && tv[1].tv_nsec == UTIME_OMIT)
        return 0;
    return SMTPFileSystem::instance()->utime(path, mtime);
}

int wrap_open(const char *path, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->open(path, file_info);
}

int wrap_read(const char *path, char *buf, size_t size, off_t offset,
    struct fuse_file_info *file_info)
{
    {
        // Loop even with direct_io: read() returns at most one device chunk, and a short
        // read would be passed to the application as-is.
        size_t bytes_read = 0;
        while (bytes_read < size) {
            int rval = SMTPFileSystem::instance()->read(path, buf + bytes_read, size - bytes_read, offset + bytes_read, file_info);
            if (rval < 0) {
                return rval;
            } else if (rval == 0) {
                break;
            }
            bytes_read += rval;
        }
        return static_cast<int>(bytes_read);
    }
}

int wrap_write(const char *path, const char *buf, size_t size, off_t offset,
    struct fuse_file_info *file_info)
{
    if (file_info->direct_io) {
        return SMTPFileSystem::instance()->write(path, buf, size, offset, file_info);
    } else {
        size_t bytes_written = 0;
        while (bytes_written < size) {
            int rval = SMTPFileSystem::instance()->write(path, buf + bytes_written, size - bytes_written, offset + bytes_written, file_info);
            if (rval < 0) {
                return rval;
            } else if (rval == 0) {
                break;
            }
            bytes_written += rval;
        }
        return static_cast<int>(bytes_written);
    }
}

int wrap_statfs(const char *path, struct statvfs *stat_info)
{
    return SMTPFileSystem::instance()->statfs(path, stat_info);
}

int wrap_flush(const char *path, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->flush(path, file_info);
}

int wrap_release(const char *path, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->release(path, file_info);
}

int wrap_fsync(const char *path, int datasync, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->fsync(path, datasync, file_info);
}

int wrap_opendir(const char *path, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->opendir(path, file_info);
}

int wrap_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *file_info, enum fuse_readdir_flags)
{
    return SMTPFileSystem::instance()->readdir(path, buf, filler,
        offset, file_info);
}

int wrap_releasedir(const char *path, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->releasedir(path, file_info);
}

int wrap_fsyncdir(const char *path, int datasync, struct fuse_file_info *file_info)
{
    return SMTPFileSystem::instance()->fsyncdir(path, datasync, file_info);
}

void* wrap_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    return SMTPFileSystem::instance()->init(conn, cfg);
}

int wrap_create(const char *path, mode_t mode, fuse_file_info *file_info)
{
    if (smtpfs_trace_enabled()) fprintf(stderr, "[trace] wrap_create %s\n", path);
    return SMTPFileSystem::instance()->create(path, mode, file_info);
}


// Files and folders macOS creates on every writable volume. They must not end up on
// the phone (a test phone got a .fseventsd folder from the very first mount).
static bool is_macos_metadata_name(const std::string &name)
{
    static const char *const names[] = {
        ".DS_Store", ".fseventsd", ".Spotlight-V100", ".Trashes",
        ".VolumeIcon.icns", ".metadata_never_index", ".metadata_never_index_unless_rootfs",
        ".apdisk", ".com.apple.timemachine.donotpresent", ".DocumentRevisions-V100",
    };
    for (const char *n : names)
        if (name == n)
            return true;
    return name.size() > 2 && name.compare(0, 2, "._") == 0; // AppleDouble
}

// Any component counts: a .fseventsd folder that already exists on the phone (from a
// mount before this filter existed) must not get fseventsd-uuid written into it.
static bool is_macos_metadata(const std::string &path)
{
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find('/', start);
        if (end == std::string::npos)
            end = path.size();
        if (end > start && is_macos_metadata_name(path.substr(start, end - start)))
            return true;
        start = end + 1;
    }
    return false;
}

// MTPDevice returns negative errno values; a few old call sites negated them again.
static int as_fuse_error(int rval)
{
    return rval > 0 ? -rval : rval;
}

// -----------------------------------------------------------------------------

SMTPFileSystem::SMTPFileSystemOptions::SMTPFileSystemOptions()
    : m_good(false)
    , m_help(false)
    , m_version(false)
    , m_verbose(false)
    , m_enable_move(false)
    , m_list_devices(false)
    , m_device_no(1)
    , m_device_file(nullptr)
    , m_mount_point(nullptr)
{
}

SMTPFileSystem::SMTPFileSystemOptions::~SMTPFileSystemOptions()
{
    free(static_cast<void*>(m_device_file));
    free(static_cast<void*>(m_mount_point));
}

// -----------------------------------------------------------------------------

int SMTPFileSystem::SMTPFileSystemOptions::opt_proc(void *data, const char *arg, int key,
    struct fuse_args *outargs)
{
    SMTPFileSystemOptions *options = static_cast<SMTPFileSystemOptions*>(data);

    if (key == FUSE_OPT_KEY_NONOPT) {
        if (options->m_mount_point && options->m_device_file) {
            // Unknown positional argument supplied
            return -1;
        }
        if (options->m_device_file) {
            fuse_opt_add_opt(&options->m_mount_point, arg);
            return 0;
        }

        fuse_opt_add_opt(&options->m_device_file, arg);
        return 0;
    }
    return 1;
}

std::unique_ptr<SMTPFileSystem> SMTPFileSystem::s_instance;

SMTPFileSystem *SMTPFileSystem::instance()
{
    if (!s_instance.get())
        s_instance.reset(new SMTPFileSystem());
    return s_instance.get();
}

SMTPFileSystem::SMTPFileSystem():
    m_args(),
    m_tmp_files_pool(),
    m_options(),
    m_device()
{
    memset(&m_fuse_operations, 0, sizeof(m_fuse_operations));
    m_fuse_operations.getattr = wrap_getattr;
    m_fuse_operations.mknod = wrap_mknod;
    m_fuse_operations.mkdir = wrap_mkdir;
    m_fuse_operations.unlink = wrap_unlink;
    m_fuse_operations.rmdir = wrap_rmdir;
    m_fuse_operations.rename = wrap_rename;
    m_fuse_operations.chmod = wrap_chmod;
    m_fuse_operations.chown = wrap_chown;
    m_fuse_operations.truncate = wrap_truncate;
    m_fuse_operations.utimens = wrap_utimens;
    m_fuse_operations.open = wrap_open;
    m_fuse_operations.read = wrap_read;
    m_fuse_operations.write = wrap_write;
    m_fuse_operations.statfs = wrap_statfs;
    m_fuse_operations.flush = wrap_flush;
    m_fuse_operations.release = wrap_release;
    m_fuse_operations.fsync = wrap_fsync;
    m_fuse_operations.setxattr = wrap_setxattr;
    m_fuse_operations.getxattr = wrap_getxattr;
    m_fuse_operations.listxattr = wrap_listxattr;
    m_fuse_operations.removexattr = wrap_removexattr;
    m_fuse_operations.opendir = wrap_opendir;
    m_fuse_operations.readdir = wrap_readdir;
    m_fuse_operations.releasedir = wrap_releasedir;
    m_fuse_operations.fsyncdir = wrap_fsyncdir;
    m_fuse_operations.init = wrap_init;
    m_fuse_operations.create = wrap_create;
}

SMTPFileSystem::~SMTPFileSystem()
{
    fuse_opt_free_args(&m_args);
}

bool SMTPFileSystem::parseOptions(int argc, char **argv)
{
    #define SMTPFS_OPT_KEY(t, p, v) { t, offsetof(SMTPFileSystemOptions, p), v }

    static struct fuse_opt smtpfs_opts[] = {
        SMTPFS_OPT_KEY("enable-move", m_enable_move, 1),
        SMTPFS_OPT_KEY("--device %i", m_device_no, 0),
        SMTPFS_OPT_KEY("-l", m_list_devices, 1),
        SMTPFS_OPT_KEY("--list-devices", m_list_devices, 1),
        SMTPFS_OPT_KEY("-v", m_verbose, 1),
        SMTPFS_OPT_KEY("--verbose", m_verbose, 1),
        SMTPFS_OPT_KEY("-V", m_version, 1),
        SMTPFS_OPT_KEY("--version", m_version, 1),
        SMTPFS_OPT_KEY("-h", m_help, 1),
        SMTPFS_OPT_KEY("--help", m_help, 1),
        FUSE_OPT_END
    };

    if (argc < 2) {
        logerr("Wrong usage.\n");
        m_options.m_good = false;
        return false;
    }

    fuse_opt_free_args(&m_args);
    m_args = FUSE_ARGS_INIT(argc, argv);
    if (fuse_opt_parse(&m_args, &m_options, smtpfs_opts, SMTPFileSystemOptions::opt_proc) == -1) {
        m_options.m_good = false;
        return false;
    }

    if (m_options.m_version || m_options.m_help || m_options.m_list_devices) {
        m_options.m_good = true;
        return true;
    }

    if (m_options.m_device_file && !m_options.m_mount_point) {
        m_options.m_mount_point = m_options.m_device_file;
        m_options.m_device_file = nullptr;
    }

    if (!m_options.m_mount_point) {
        logerr("Mount point missing.\n");
        m_options.m_good = false;
        return false;
    }

    fuse_opt_add_arg(&m_args, m_options.m_mount_point);
    // Multi-threaded: the device itself is serialised by MTPDevice's priority lock.

    if (m_options.m_verbose) {
        Logger::setGlobalVerbose();
        fuse_opt_add_arg(&m_args, "-f");
    }

    --m_options.m_device_no;

    // device file and -- device are mutually exclusive, fail if both set
    if (m_options.m_device_no && m_options.m_device_file) {
        m_options.m_good = false;
        return false;
    }

    m_options.m_good = true;
    return true;
}

void SMTPFileSystem::printHelp() const
{
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    struct fuse_operations tmp_operations;
    memset(&tmp_operations, 0, sizeof(tmp_operations));
    std::cerr << "usage: " << smtpfs_basename(m_args.argv[0])
              << " <source> mountpoint [options]\n\n"
        << "general options:\n"
        << "    -o opt,[opt...]        mount options\n"
        << "    -h   --help            print help\n"
        << "    -V   --version         print version\n\n"
        << "simple-mtpfs options:\n"
        << "    -v   --verbose         verbose output, implies -f\n"
        << "    -l   --list-devices    print available devices. Supports <source> option\n"
        << "         --device          select a device number to mount\n"
        << "    -o enable-move         enable the move operations\n\n";
    fuse_opt_add_arg(&args, m_args.argv[0]);
    fuse_opt_add_arg(&args, "--help");
    
    fuse_main(args.argc, args.argv, &tmp_operations, nullptr);
    fuse_opt_free_args(&args);
    std::cerr << "\nReport bugs to <" << PACKAGE_BUGREPORT << ">.\n";
}

void SMTPFileSystem::printVersion() const
{
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    struct fuse_operations tmp_operations;
    memset(&tmp_operations, 0, sizeof(tmp_operations));
    fuse_opt_add_arg(&args, m_args.argv[0]);
    fuse_opt_add_arg(&args, "--version");
    std::cout << "simple-mtpfs version " << VERSION << "\n";

    fuse_main(args.argc, args.argv, &tmp_operations, nullptr);
    fuse_opt_free_args(&args);
}

bool SMTPFileSystem::listDevices() const
{
    const std::string dev_file = m_options.m_device_file ? m_options.m_device_file : "";

    return MTPDevice::listDevices(m_options.m_verbose, dev_file);
}

bool SMTPFileSystem::exec()
{
    if (!m_options.m_good)
        return false;

    if (m_options.m_version || m_options.m_help)
        return true;

    // With macFUSE's FSKit backend the mount point lives under /Volumes and is
    // created by the system (/Volumes is root-owned), so it need not exist yet.
    const bool under_volumes =
        std::string(m_options.m_mount_point).rfind("/Volumes/", 0) == 0;
    if (!under_volumes && !smtpfs_check_dir(m_options.m_mount_point)) {
        logerr("Can not mount the device to '", m_options.m_mount_point, "'.\n");
        return false;
    }

    if (!m_tmp_files_pool.createTmpDir()) {
        logerr("Can not create a temporary directory.\n");
        return false;
    }

    if (m_options.m_device_file) {
        // Try to use device file first, if provided
        if (!m_device.connect(m_options.m_device_file))
            return false;
    } else {
        // Connect to MTP device by order number, if no device file supplied
        if (!m_device.connect(m_options.m_device_no))
            return false;
    }
    m_device.enableMove(m_options.m_enable_move);
    if (fuse_main(m_args.argc, m_args.argv, &m_fuse_operations, nullptr) > 0) {
        return false;
    }
    m_device.disconnect();

    if (!m_tmp_files_pool.removeTmpDir()) {
        logerr("Can not remove a temporary directory.\n");
        return false;
    }

    return true;
}

void* SMTPFileSystem::init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void) conn;
    // MTP object ids are stable; expose them as inode numbers.
    cfg->use_ino = 1;
    return nullptr;
}

bool SMTPFileSystem::pendingAttr(const std::string &path, struct stat *buf)
{
    // caller holds m_pool_mutex
    std::string tmp;
    auto it = m_pushing.find(path);
    if (it != m_pushing.end()) {
        tmp = it->second;
    } else if (m_content_paths.count(path)) {
        const TypeTmpFile *t = m_tmp_files_pool.getFile(path);
        if (t)
            tmp = t->pathTmp();
    }
    struct stat st;
    if (tmp.empty() || ::stat(tmp.c_str(), &st) != 0)
        return false;
    memset(buf, 0, sizeof(struct stat));
    struct fuse_context *fc = fuse_get_context();
    buf->st_uid = fc ? fc->uid : getuid();
    buf->st_gid = fc ? fc->gid : getgid();
    buf->st_mode = S_IFREG | 0644;
    buf->st_nlink = 1;
    buf->st_size = st.st_size;
    buf->st_blocks = st.st_blocks;
    buf->st_mtime = buf->st_ctime = buf->st_atime = st.st_mtime;
    return true;
}

std::vector<std::string> SMTPFileSystem::pendingNamesIn(const std::string &dir)
{
    // caller holds m_pool_mutex
    std::vector<std::string> out;
    auto consider = [&](const std::string &p) {
        if (smtpfs_dirname(p) == dir || (dir == "/" && smtpfs_dirname(p) == ""))
            out.push_back(smtpfs_basename(p));
    };
    for (const auto &kv : m_pushing)
        consider(kv.first);
    for (const std::string &p : m_content_paths)
        consider(p);
    return out;
}



// close() cannot report a failed upload under FSKit, so tell the user directly.
static void notifyUser(const std::string &title, const std::string &text)
{
    auto quote = [](std::string v) {
        std::string out;
        for (char c : v)
            if (c != '"' && c != '\\')
                out += c;
        return out;
    };
    const std::string script = "display notification \"" + quote(text) +
        "\" with title \"" + quote(title) + "\"";
    char *argv[] = { (char *) "/usr/bin/osascript", (char *) "-e",
                     (char *) script.c_str(), nullptr };
    pid_t pid;
    if (posix_spawn(&pid, argv[0], nullptr, nullptr, argv, environ) == 0)
        waitpid(pid, nullptr, 0); // osascript returns at once; avoids a zombie
}

int SMTPFileSystem::pushNow(const std::string &path)
{
    std::string tmp;
    {
        std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
        m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(path); });
        TypeTmpFile *t = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(path));
        if (!t || !t->isModified())
            return 0;
        tmp = t->pathTmp();
        t->setModified(false);
        m_pushing[path] = tmp;
    }
    const int rval = m_device.filePush(tmp, path);
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        m_pushing.erase(path);
        if (rval != 0) {
            TypeTmpFile *t = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(path));
            if (t)
                t->setModified(); // try again on the next flush/release
        }
    }
    m_push_cv.notify_all();
    return as_fuse_error(rval);
}

std::string SMTPFileSystem::rescueTmp(const std::string &tmp, const std::string &path)
{
    const char *home = getenv("HOME");
    std::string dir = std::string(home ? home : "/tmp") + "/Library/Application Support/Droidmount";
    ::mkdir(dir.c_str(), 0700);
    dir += "/unsent";
    ::mkdir(dir.c_str(), 0700);
    char stamp[32];
    time_t now = time(nullptr);
    strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", localtime(&now));
    const std::string target = dir + "/" + stamp + "-" + smtpfs_basename(path);
    if (::rename(tmp.c_str(), target.c_str()) != 0) {
        fprintf(stderr, "simple-mtpfs: upload of %s failed; local copy kept at %s\n",
            path.c_str(), tmp.c_str());
        return tmp;
    }
    fprintf(stderr, "simple-mtpfs: upload of %s failed; local copy saved as %s\n",
        path.c_str(), target.c_str());
    if (getenv("DROIDMOUNT_APP")) {
        // Running under the Droidmount app: it shows the notification under its own name.
        printf("@unsent %s\t%s\n", smtpfs_basename(path).c_str(), target.c_str());
        fflush(stdout);
    } else {
        notifyUser("Upload to the phone failed",
            smtpfs_basename(path) + " was not copied. A copy is in Application Support/Droidmount/unsent.");
    }
    return target;
}

void SMTPFileSystem::rekeyPending(const std::string &from, const std::string &to)
{
    const TypeTmpFile *t = m_tmp_files_pool.getFile(from);
    if (t) {
        const std::set<int> fds = t->fileDescriptors();
        TypeTmpFile moved(to, t->pathTmp(), fds.empty() ? -1 : *fds.begin(), t->isModified());
        for (int fd : fds)
            moved.addFileDescriptor(fd);
        if (fds.empty())
            moved.removeFileDescriptor(-1);
        m_tmp_files_pool.removeFile(from);
        m_tmp_files_pool.addFile(moved);
    }
    if (m_content_paths.erase(from))
        m_content_paths.insert(to);
}

int SMTPFileSystem::getattr(const char *path, struct stat *buf)
{
    // A file open for writing or being uploaded is described by its local copy: the
    // phone still has the old size (or nothing) until the upload has finished.
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        if (pendingAttr(std::string(path), buf))
            return 0;
    }
    return getattrTree(path, buf);
}

int SMTPFileSystem::getattrTree(const char *path, struct stat *buf)
{
    memset(buf, 0, sizeof(struct stat));
    struct fuse_context *fc = fuse_get_context();
    buf->st_uid = fc->uid;
    buf->st_gid = fc->gid;
    if (path == std::string("/")) {
        buf->st_mode = S_IFDIR | 0775;
        buf->st_nlink = 2;
        return 0;
    } else {
        std::string tmp_path(smtpfs_dirname(path));
        std::string tmp_file(smtpfs_basename(path));
        const TypeDir *content = m_device.dirFetchContent(tmp_path);
        if (!content) {
            return -ENOENT;
        }

        if (content->dir(tmp_file)) {
            const TypeDir *dir = content->dir(tmp_file);
            buf->st_ino = dir->id();
            buf->st_mode = S_IFDIR | 0775;
            buf->st_nlink = 2;
            buf->st_mtime = dir->modificationDate();
        } else if (content->file(tmp_file)) {
            const TypeFile *file = content->file(tmp_file);
            buf->st_ino = file->id();
            buf->st_size = file->size();
            buf->st_blocks = (file->size() / 512) + (file->size() % 512 > 0 ? 1 : 0);
            buf->st_nlink = 1;
            buf->st_mode = S_IFREG | 0644;
            buf->st_mtime = file->modificationDate();
            buf->st_ctime = buf->st_mtime;
            buf->st_atime = buf->st_mtime;
        } else {
            return -ENOENT;
        }
    }

    return 0;
}

int SMTPFileSystem::mknod(const char *path, mode_t mode, dev_t dev)
{
    if (is_macos_metadata(path))
        return -EPERM;
    std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
    if (!S_ISREG(mode))
        return -EINVAL;

    std::string tmp_path = m_tmp_files_pool.makeTmpPath(std::string(path));
    int rval = ::open(tmp_path.c_str(), O_CREAT | O_WRONLY, mode);
    if (rval < 0)
        return -errno;

    rval = ::close(rval);
    if (rval < 0)
        return -errno;

    rval = m_device.filePush(tmp_path, std::string(path));
    ::unlink(tmp_path.c_str());
    return as_fuse_error(rval);
}

int SMTPFileSystem::mkdir(const char *path, mode_t mode)
{
    if (is_macos_metadata(path))
        return -EPERM;
    return as_fuse_error(m_device.dirCreateNew(std::string(path)));
}

int SMTPFileSystem::unlink(const char *path)
{
    const std::string std_path(path);
    std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
    m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(std_path); });
    TypeTmpFile *t = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path));
    const bool pending = t || m_content_paths.count(std_path);
    if (t)
        t->setModified(false); // do not upload a file that was deleted
    m_content_paths.erase(std_path);
    struct stat st;
    if (getattrTree(path, &st) != 0)
        return pending ? 0 : -ENOENT;
    return as_fuse_error(m_device.fileRemove(std_path));
}

int SMTPFileSystem::rmdir(const char *path)
{
    return as_fuse_error(m_device.dirRemove(std::string(path)));
}

int SMTPFileSystem::rename(const char *path, const char *newpath)
{
    if (is_macos_metadata(newpath))
        return -EPERM;
    const std::string from(path), to(newpath);
    std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
    m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(from) && !m_pushing.count(to); });
    if (m_tmp_files_pool.getFile(to) || m_content_paths.count(to))
        return -EBUSY; // the target is open for writing right now

    const bool pending = m_tmp_files_pool.getFile(from) || m_content_paths.count(from);
    struct stat st;
    const bool on_phone = getattrTree(path, &st) == 0;
    if (!on_phone && !pending)
        return -ENOENT;
    if (on_phone) {
        // rename/move on the phone; an existing target is replaced (POSIX), and a
        // pending upload simply follows to the new name
        const int rval = as_fuse_error(m_device.rename(from, to));
        if (rval != 0)
            return rval;
    }
    // Only local so far (being written): the upload will go to the new name and replace
    // whatever is there.
    rekeyPending(from, to);
    return 0;
}

int SMTPFileSystem::chmod(const char *path, mode_t mode)
{
    return 0;
}

int SMTPFileSystem::chown(const char *path, uid_t uid, gid_t gid)
{
    return 0;
}

int SMTPFileSystem::truncate(const char *path, off_t new_size)
{
    const std::string std_path(path);
    std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
    m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(std_path); });

    // Open for writing somewhere: change the local copy, it is sent on release.
    TypeTmpFile *open_file = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path));
    if (open_file && m_content_paths.count(std_path)) {
        if (::truncate(open_file->pathTmp().c_str(), new_size) != 0)
            return -errno;
        open_file->setModified();
        return 0;
    }

    const std::string tmp_path = m_tmp_files_pool.makeTmpPath(std_path);
    int rval = m_device.filePull(std_path, tmp_path);
    if (rval == 0 && ::truncate(tmp_path.c_str(), new_size) != 0)
        rval = -errno;
    if (rval == 0)
        rval = m_device.filePush(tmp_path, std_path); // replaces the old object
    ::unlink(tmp_path.c_str());
    return as_fuse_error(rval);
}

int SMTPFileSystem::utime(const char *path, time_t modtime)
{
    std::string tmp_basename(smtpfs_basename(std::string(path)));
    std::string tmp_dirname(smtpfs_dirname(std::string(path)));

    const TypeDir *parent = m_device.dirFetchContent(tmp_dirname);
    if (!parent)
        return -ENOENT;

    const TypeFile *file = parent->file(tmp_basename);
    if (!file)
        return -ENOENT;

    const_cast<TypeFile*>(file)->setModificationDate(modtime);

    return 0;
}

int SMTPFileSystem::create(const char *path, mode_t mode, fuse_file_info *file_info)
{
    if (is_macos_metadata(path))
        return -EPERM;
    const std::string std_path(path);
    std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
    m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(std_path); });
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[create] %s\n", path);

    // The file exists only locally until the last handle is released; getattr() and
    // readdir() report it from the temp file meanwhile.
    TypeTmpFile *existing = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path));
    const std::string tmp_path = existing ? existing->pathTmp()
                                          : m_tmp_files_pool.makeTmpPath(std_path);
    int fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_RDWR, S_IRUSR | S_IWUSR);
    if (fd < 0)
        return -errno;

    file_info->fh = fd;
    if (existing) {
        existing->addFileDescriptor(fd);
        existing->setModified();
    } else {
        // modified = true: an empty new file must still be created on the phone
        m_tmp_files_pool.addFile(TypeTmpFile(std_path, tmp_path, fd, true));
    }
    m_content_paths.insert(std_path);
    m_write_fhs.insert(fd);
    return 0;
}

int SMTPFileSystem::open(const char *path, struct fuse_file_info *file_info)
{
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[open] %s flags 0x%x\n", path, file_info->flags);

    const std::string std_path(path);
    const bool for_write = (file_info->flags & O_ACCMODE) != O_RDONLY;
    const bool truncate = for_write && (file_info->flags & O_TRUNC);
    if (for_write && is_macos_metadata(std_path))
        return -EPERM;

    std::unique_lock<std::mutex> pool_lock(m_pool_mutex);
    m_push_cv.wait(pool_lock, [&] { return !m_pushing.count(std_path); });
    TypeTmpFile *tmp_file = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path));
    const std::string tmp_path = tmp_file ? tmp_file->pathTmp()
                                          : m_tmp_files_pool.makeTmpPath(std_path);
    const bool new_entry = tmp_file == nullptr;

    if (!m_content_paths.count(std_path) && (for_write || !hasPartialObjectSupport())) {
        // The real bytes are needed locally: we are going to change them, or the phone
        // cannot serve partial reads.
        if (truncate) {
            int fd = ::open(tmp_path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, S_IRUSR | S_IWUSR);
            if (fd < 0)
                return -errno;
            ::close(fd);
        } else {
            int rval = m_device.filePull(std_path, tmp_path);
            if (rval != 0) {
                if (new_entry)
                    ::unlink(tmp_path.c_str());
                return as_fuse_error(rval);
            }
        }
        m_content_paths.insert(std_path);
    } else if (new_entry) {
        // Placeholder for a valid descriptor; data is read from the phone in chunks.
        int fd = ::creat(tmp_path.c_str(), S_IRUSR | S_IWUSR);
        if (fd < 0)
            return -errno;
        ::close(fd);
    }

    int fd = ::open(tmp_path.c_str(), file_info->flags & (O_ACCMODE | O_TRUNC | O_APPEND));
    if (fd < 0) {
        const int err = errno;
        if (new_entry) {
            ::unlink(tmp_path.c_str());
            m_content_paths.erase(std_path);
        }
        return -err;
    }

    file_info->fh = fd;
    if (tmp_file)
        tmp_file->addFileDescriptor(fd);
    else
        m_tmp_files_pool.addFile(TypeTmpFile(std_path, tmp_path, fd));
    if (for_write) {
        m_write_fhs.insert(fd);
        if (truncate)
            const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path))->setModified();
    }
    return 0;
}

int SMTPFileSystem::read(const char *path, char *buf, size_t size,
    off_t offset, struct fuse_file_info *file_info)
{
    bool local;
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        local = m_write_fhs.count(file_info->fh) || m_content_paths.count(std::string(path))
            || !hasPartialObjectSupport();
    }
    if (local) {
        ssize_t rval = ::pread(file_info->fh, buf, size, offset);
        return rval < 0 ? -errno : static_cast<int>(rval);
    }
    // Keep each device transaction short; wrap_read() loops for the rest.
    const size_t chunk = size < kMaxReadChunk ? size : kMaxReadChunk;
    return m_device.fileRead(std::string(path), buf, chunk, offset);
}

int SMTPFileSystem::write(const char *path, const char *buf, size_t size,
    off_t offset, struct fuse_file_info *file_info)
{
    // Writes only touch the local temp file; nothing goes to the phone until release.
    std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
    if (!m_write_fhs.count(file_info->fh))
        return -EBADF;
    TypeTmpFile *tmp_file = const_cast<TypeTmpFile*>(
        m_tmp_files_pool.getFile(std::string(path)));
    if (!tmp_file)
        return -EBADF;
    ssize_t rval = ::pwrite(file_info->fh, buf, size, offset);
    if (rval < 0)
        return -errno;
    tmp_file->setModified();
    return static_cast<int>(rval);
}

int SMTPFileSystem::release(const char *path, struct fuse_file_info *file_info)
{
    const std::string std_path(path);
    std::string tmp_path;
    bool push = false;
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        ::close(file_info->fh);
        m_write_fhs.erase(file_info->fh);
        if (std_path == std::string("-"))
            return 0;
        TypeTmpFile *tmp_file = const_cast<TypeTmpFile*>(m_tmp_files_pool.getFile(std_path));
        if (!tmp_file)
            return 0;
        tmp_file->removeFileDescriptor(file_info->fh);
        if (smtpfs_trace_enabled())
            fprintf(stderr, "[release] %s refs left %d modified %d\n", path,
                tmp_file->refcnt(), tmp_file->isModified());
        if (tmp_file->refcnt() != 0)
            return 0;
        push = tmp_file->isModified();
        tmp_path = tmp_file->pathTmp();
        m_tmp_files_pool.removeFile(std_path);
        m_content_paths.erase(std_path);
        if (push)
            m_pushing[std_path] = tmp_path; // getattr still sees it, open() waits
    }
    // Usually flush() has uploaded already; this catches what is left. Outside the pool
    // lock: a large upload must not stall every other open().
    int rval = push ? m_device.filePush(tmp_path, std_path) : 0;
    if (push) {
        {
            std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
            m_pushing.erase(std_path);
        }
        m_push_cv.notify_all();
    }
    if (rval != 0)
        rescueTmp(tmp_path, std_path); // never throw away data that did not arrive
    else
        ::unlink(tmp_path.c_str()); // only after getattr() stopped looking at it
    return as_fuse_error(rval);
}

int SMTPFileSystem::statfs(const char *path, struct statvfs *stat_info)
{
    uint64_t bs = 1024;
    // XXX: linux coreutils still use bsize member to calculate free space
    stat_info->f_bsize = static_cast<unsigned long>(bs);
    stat_info->f_frsize = static_cast<unsigned long>(bs);
    stat_info->f_blocks = m_device.storageTotalSize() / bs;
    stat_info->f_bavail = m_device.storageFreeSize() / bs;
    stat_info->f_bfree = stat_info->f_bavail;
    return 0;
}


int SMTPFileSystem::flush(const char *path, struct fuse_file_info *file_info)
{
    // close() calls flush(): uploading here lets a failed upload reach the application.
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[flush] %s fh %llu\n", path, (unsigned long long) file_info->fh);
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        if (!m_write_fhs.count(file_info->fh))
            return 0;
    }
    return pushNow(std::string(path));
}

int SMTPFileSystem::fsync(const char *path, int datasync,
    struct fuse_file_info *fi)
{
    bool writer;
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        writer = m_write_fhs.count(fi->fh) > 0;
    }
    if (smtpfs_trace_enabled())
        fprintf(stderr, "[fsync] %s writer %d\n", path, writer);
    if (writer) {
        const int pr = pushNow(std::string(path));
        if (pr != 0)
            return pr;
    }
    int rval = -1;
#ifdef HAVE_FDATASYNC
    if (datasync)
        rval = ::fdatasync(fi->fh);
    else
#else
    rval = ::fsync(fi->fh);
#endif
    if (rval != 0)
        return -errno;
    return 0;
}

int SMTPFileSystem::opendir(const char *path, struct fuse_file_info *file_info)
{
    const TypeDir *content = m_device.dirFetchContent(std::string(path));
    if (!content)
        return -ENOENT;

    return 0;
}

int SMTPFileSystem::readdir(const char *path, void *buf, fuse_fill_dir_t filler,
    off_t offset, struct fuse_file_info *file_info)
{
    const TypeDir *content = m_device.dirFetchContent(std::string(path));
    if (!content)
        return -ENOENT;

    std::vector<TypeDir::Entry> entries = content->entries();
    {
        std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
        for (const std::string &name : pendingNamesIn(std::string(path))) {
            bool known = false;
            for (const TypeDir::Entry &e : entries)
                known |= e.name == name;
            if (!known)
                entries.push_back(TypeDir::Entry{name, 0, false, 0, 0});
        }
    }
    for (const TypeDir::Entry &e : entries) {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = e.id;
        st.st_mode = e.is_dir ? (S_IFDIR | 0775) : (S_IFREG | 0644);
        filler(buf, e.name.c_str(), &st, 0, static_cast<fuse_fill_dir_flags>(0));
    }

    return 0;
}

int SMTPFileSystem::releasedir(const char *path, struct fuse_file_info *file_info)
{
    return 0;
}

int SMTPFileSystem::fsyncdir(const char *path, int datasync,
    struct fuse_file_info *file_info)
{
    return 0;
}

int SMTPFileSystem::ftruncate(const char *path, off_t offset,
    struct fuse_file_info *file_info)
{
    std::lock_guard<std::mutex> pool_lock(m_pool_mutex);
    if (!m_write_fhs.count(file_info->fh))
        return -EBADF;
    TypeTmpFile *tmp_file = const_cast<TypeTmpFile*>(
        m_tmp_files_pool.getFile(std::string(path)));
    if (!tmp_file)
        return -EBADF;
    if (::ftruncate(file_info->fh, offset) != 0)
        return -errno;
    tmp_file->setModified();
    return 0;
}

bool SMTPFileSystem::hasPartialObjectSupport()
{
    MTPDevice::Capabilities caps = m_device.getCapabilities();
    // Writes always go through a local temp file, so only partial GET matters.
    return caps.canGetPartialObject();
}
