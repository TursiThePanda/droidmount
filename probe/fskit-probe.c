/* fskit-probe: mount an empty read-only filesystem through macFUSE's FSKit backend.
 * If the mount appears, macFUSE is installed and its FSKit module is enabled.
 * usage: fskit-probe <mountpoint under /Volumes>   (runs in the foreground) */
#define FUSE_USE_VERSION 31
#define FUSE_DARWIN_ENABLE_EXTENSIONS 0
#include <fuse.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

static int p_getattr(const char *path, struct stat *st, struct fuse_file_info *fi)
{
    (void) fi;
    memset(st, 0, sizeof(*st));
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    st->st_mode = S_IFDIR | 0555;
    st->st_nlink = 2;
    return 0;
}

static int p_readdir(const char *path, void *buf, fuse_fill_dir_t fill, off_t off,
                     struct fuse_file_info *fi, enum fuse_readdir_flags flags)
{
    (void) off; (void) fi; (void) flags;
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    fill(buf, ".", NULL, 0, 0);
    fill(buf, "..", NULL, 0, 0);
    return 0;
}

static const struct fuse_operations ops = { .getattr = p_getattr, .readdir = p_readdir };

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    char *args[] = { argv[0], "-f", argv[1], "-o",
                     "ro,backend=fskit,local,volname=DroidmountProbe", NULL };
    return fuse_main(5, args, &ops, NULL);
}
