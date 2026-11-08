/*
 * android-automount — part of Droidmount. Mounts an Android phone (any vendor) as a
 * Finder disk while it is plugged in and in "File transfer" (MTP) mode.
 *
 * Detection: any USB interface whose name contains "MTP". Android announces its MTP
 * function that way on every vendor (class 6/1/1 on Pixels, 0xFF vendor-specific on
 * others); it is the same heuristic libmtp uses for devices it does not know.
 *
 * When such an interface appears, simple-mtpfs is started in the foreground on macFUSE's
 * FSKit backend, mounted at /Volumes/<product name>. When it disappears (unplug, or the
 * phone switched back to "Charging only"), the volume is unmounted. If the user ejects
 * the volume (Finder or SIGUSR1), it stays unmounted until the phone is plugged in again.
 *
 * Machine-readable events go to stdout, one per line, for the Droidmount app:
 *   @watching
 *   @plugged <name>                     an MTP phone appeared
 *   @mounting <name>\t<mountpoint>
 *   @mounted <name>\t<mountpoint>
 *   @unmounted <name>                   (unplugged, ejected or failed)
 *   @ejected <name>                     user ejected; no remount until replug
 *   @gone <name>                        the MTP phone disappeared
 *   @hint filetransfer <name>           a phone is on USB but not in File transfer mode
 *   @hint unlock <name>                 MTP is there but mounting keeps failing
 *   @giveup <name>
 * Human-readable log lines go to stderr.
 *
 * One phone at a time.
 *
 *   android-automount <path to simple-mtpfs> [extra mount options, e.g. "ro"]
 * Signals: SIGUSR1 = eject the current phone; SIGTERM/SIGINT/SIGHUP = unmount and quit.
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#define MAX_ATTEMPTS 12      /* per plug-in */
#define UNLOCK_HINT_AFTER 2  /* failed attempts before suggesting to unlock the phone */
#define RETRY_SECONDS 5.0
#define SETTLE_SECONDS 2.0   /* let the device finish enumerating */
#define FILETRANSFER_HINT_SECONDS 5.0
#define REENUM_SECONDS 5.0   /* a phone resetting its USB right after an eject */

static const char *g_fs_bin;
static const char *g_extra_opts = ""; /* read-write unless "ro" is passed */

static uint64_t g_intf_id = 0;     /* registry id of the MTP interface we serve, 0 = none */
static char g_name[96];            /* volume name, from the USB product string */
static char g_mountpoint[160];
static pid_t g_child = 0;          /* running simple-mtpfs */
static int g_attempts = 0;
static int g_mounted = 0;
static int g_ejected = 0;          /* user ejected: no remount until the phone comes back */
static int g_unlock_hinted = 0;
static uint64_t g_loc = 0;          /* USB locationID (port) of the phone we serve */
/* Closing the MTP session makes some phones (Pixel) drop off USB and come straight back.
 * After an eject that must not count as plugging in again, or the phone is remounted. */
static CFAbsoluteTime g_eject_time = 0;
static uint64_t g_reenum_loc = 0;
static CFAbsoluteTime g_reenum_until = 0;
static CFAbsoluteTime g_next_try = 0;
static volatile sig_atomic_t g_eject_requested = 0;
static volatile sig_atomic_t g_quit_requested = 0;

/* A phone of a known Android vendor that showed up on USB: if no MTP interface follows,
 * it is probably in "Charging only" mode. */
static uint64_t g_pending_dev = 0;
static char g_pending_name[96];
static CFAbsoluteTime g_pending_since = 0;

static void logf_(const char *fmt, ...)
{
    char ts[32];
    time_t t = time(NULL);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", localtime(&t));
    fprintf(stderr, "%s ", ts);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}

static void event(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputc('@', stdout);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

static int run_wait(char *const argv[])
{
    pid_t pid;
    int status = -1;
    if (posix_spawn(&pid, argv[0], NULL, NULL, argv, environ) != 0)
        return -1;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return status;
}

/* Is something mounted at mp? Read from the kernel's mount table (MNT_NOWAIT: no request
 * reaches any file system). A stat() of the mount point would be a request to the phone's
 * file system, queued in the FSKit module behind Finder's and Quick Look's requests. */
static int is_mounted(const char *mp)
{
    struct statfs *m;
    if (!mp[0])
        return 0;
    int n = getmntinfo(&m, MNT_NOWAIT);
    for (int i = 0; i < n; i++)
        if (strcmp(m[i].f_mntonname, mp) == 0)
            return 1;
    return 0;
}

/* Force-unmount without asking the volume anything first. /sbin/umount resolves the path
 * with realpath() before it unmounts; that request waits in the FSKit module behind
 * Finder's and Quick Look's requests until umount gives up after ~10 s ("pthread_cond_
 * timeout failed; continuing with unmount") - measured 11-17 s with a folder open in
 * Finder, plugged in or not. unmount(2) itself takes a few milliseconds. */
static void force_unmount(void)
{
    if (!is_mounted(g_mountpoint))
        return;
    if (unmount(g_mountpoint, MNT_FORCE) == 0)
        return;
    const int err = errno;
    if (!is_mounted(g_mountpoint))
        return;
    logf_("unmount(%s) failed: %s; trying umount -f", g_mountpoint, strerror(err));
    char *um[] = { "/sbin/umount", "-f", g_mountpoint, NULL };
    run_wait(um);
}

static int get_string(io_registry_entry_t e, CFStringRef key, char *buf, size_t len)
{
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, NULL, 0);
    int ok = 0;
    if (v && CFGetTypeID(v) == CFStringGetTypeID())
        ok = CFStringGetCString((CFStringRef) v, buf, (CFIndex) len, kCFStringEncodingUTF8);
    if (v)
        CFRelease(v);
    return ok;
}

static uint64_t get_u64(io_registry_entry_t e, CFStringRef key)
{
    int64_t n = 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, NULL, 0);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef) v, kCFNumberSInt64Type, &n);
    if (v)
        CFRelease(v);
    return (uint64_t) n;
}

static int get_int(io_registry_entry_t e, CFStringRef key)
{
    int n = -1;
    CFTypeRef v = IORegistryEntryCreateCFProperty(e, key, NULL, 0);
    if (v && CFGetTypeID(v) == CFNumberGetTypeID())
        CFNumberGetValue((CFNumberRef) v, kCFNumberIntType, &n);
    if (v)
        CFRelease(v);
    return n;
}

/* Volume names go into a comma-separated -o list and a path: keep it plain. */
static void sanitize(char *s)
{
    for (char *p = s; *p; p++)
        if (!(isalnum((unsigned char) *p) || *p == ' ' || *p == '-' || *p == '_' || *p == '.'))
            *p = '_';
    size_t n = strlen(s);
    while (n > 0 && s[n - 1] == ' ')
        s[--n] = 0;
    if (!*s || strcmp(s, ".") == 0 || strcmp(s, "..") == 0)
        strcpy(s, "Android");
}

static void start_fs(void)
{
    if (g_child > 0 || !g_intf_id || g_ejected)
        return;
    if (is_mounted(g_mountpoint)) {
        logf_("%s is already mounted by something else; not starting", g_mountpoint);
        return;
    }
    char opts[256];
    snprintf(opts, sizeof(opts), "%s%sbackend=fskit,local,volname=%s", g_extra_opts,
             g_extra_opts[0] ? "," : "", g_name);
    char *argv[] = { (char *) g_fs_bin, "-f", g_mountpoint, "-o", opts, NULL };
    g_attempts++;
    if (posix_spawn(&g_child, g_fs_bin, NULL, NULL, argv, environ) != 0) {
        logf_("cannot start %s: %s", g_fs_bin, strerror(errno));
        g_child = 0;
        return;
    }
    logf_("mounting %s at %s (simple-mtpfs pid %d, attempt %d)", g_name, g_mountpoint,
          g_child, g_attempts);
    event("mounting %s\t%s", g_name, g_mountpoint);
}

static int wait_child(int tenths)
{
    for (int i = 0; i < tenths && g_child > 0; i++) {
        if (waitpid(g_child, NULL, WNOHANG) == g_child)
            g_child = 0;
        else
            usleep(100 * 1000);
    }
    return g_child == 0;
}

static void stop_fs(const char *why)
{
    if (g_child > 0)
        logf_("%s: unmounting %s (simple-mtpfs pid %d)", why, g_mountpoint, g_child);
    /* Under the FSKit backend libfuse does not unmount when the daemon exits, so unmount
     * first, while the daemon can still answer; the session loop then ends by itself. */
    force_unmount();
    if (g_child > 0 && !wait_child(30)) {
        kill(g_child, SIGTERM);
        if (!wait_child(30)) {
            kill(g_child, SIGKILL);
            waitpid(g_child, NULL, 0);
            g_child = 0;
        }
    }
    force_unmount();
    if (g_mounted) {
        g_mounted = 0;
        event("unmounted %s", g_name);
    }
}

static void on_timer(CFRunLoopTimerRef timer, void *info)
{
    (void) timer;
    (void) info;
    const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();

    if (g_quit_requested) {
        stop_fs("agent stopping");
        exit(0);
    }
    if (g_eject_requested) {
        g_eject_requested = 0;
        if (g_intf_id && !g_ejected) {
            g_ejected = 1;
            g_eject_time = now;
            g_next_try = 0;
            stop_fs("ejected by the user");
            event("ejected %s", g_name);
        }
    }

    if (g_child > 0) {
        int status;
        if (waitpid(g_child, &status, WNOHANG) == g_child) {
            logf_("simple-mtpfs pid %d exited (%s %d)", g_child,
                  WIFEXITED(status) ? "code" : "signal",
                  WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status));
            g_child = 0;
            const int clean_exit = WIFEXITED(status) && WEXITSTATUS(status) == 0;
            if (g_mounted) {
                g_mounted = 0;
                event("unmounted %s", g_name);
                if (clean_exit && !g_ejected) {
                    /* the volume was unmounted (Finder eject) and the daemon ended
                     * normally: respect it until the phone is plugged in again */
                    g_ejected = 1;
                    g_eject_time = now;
                    g_next_try = 0;
                    logf_("%s ejected in Finder; not remounting until replug", g_mountpoint);
                    event("ejected %s", g_name);
                }
            }
            if (g_intf_id && !g_ejected) {
                if (g_attempts >= UNLOCK_HINT_AFTER && !g_unlock_hinted) {
                    g_unlock_hinted = 1;
                    event("hint unlock %s", g_name);
                }
                if (g_attempts < MAX_ATTEMPTS) {
                    g_next_try = now + RETRY_SECONDS;
                } else {
                    logf_("giving up until the phone is plugged in again "
                          "(is it unlocked and set to File transfer?)");
                    event("giveup %s", g_name);
                }
            }
        }
    }
    /* mount state as the user sees it */
    if (g_child > 0 && !g_mounted && is_mounted(g_mountpoint)) {
        g_mounted = 1;
        g_unlock_hinted = 0;
        event("mounted %s\t%s", g_name, g_mountpoint);
    } else if (g_mounted && !is_mounted(g_mountpoint)) {
        /* ejected in Finder (or umount): respect it until the phone is replugged */
        logf_("%s was unmounted outside Droidmount; not remounting until replug", g_mountpoint);
        g_mounted = 0;
        g_ejected = 1;
        g_eject_time = now;
        g_next_try = 0;
        event("unmounted %s", g_name);
        event("ejected %s", g_name);
        stop_fs("ejected in Finder");
    }

    if (g_intf_id && g_child == 0 && !g_ejected && g_next_try > 0 && now >= g_next_try) {
        g_next_try = 0;
        start_fs();
    }

    if (g_pending_dev && !g_intf_id && now - g_pending_since >= FILETRANSFER_HINT_SECONDS) {
        event("hint filetransfer %s", g_pending_name);
        g_pending_dev = 0;
    }
}

static int is_mtp_interface(io_service_t intf)
{
    char name[64] = "";
    get_string(intf, CFSTR("kUSBString"), name, sizeof(name));
    for (char *p = name; *p; p++)
        *p = (char) toupper((unsigned char) *p);
    return strstr(name, "MTP") != NULL;
}

static void on_arrival(void *refcon, io_iterator_t it)
{
    (void) refcon;
    io_service_t intf;
    while ((intf = IOIteratorNext(it))) {
        if (!is_mtp_interface(intf)) {
            IOObjectRelease(intf);
            continue;
        }
        uint64_t id = 0;
        IORegistryEntryGetRegistryEntryID(intf, &id);
        int vid = get_int(intf, CFSTR("idVendor")), pid = get_int(intf, CFSTR("idProduct"));

        io_registry_entry_t dev = 0;
        char product[96] = "", vendor[64] = "";
        uint64_t loc = 0;
        if (IORegistryEntryGetParentEntry(intf, kIOServicePlane, &dev) == KERN_SUCCESS) {
            get_string(dev, CFSTR(kUSBProductString), product, sizeof(product));
            get_string(dev, CFSTR(kUSBVendorString), vendor, sizeof(vendor));
            loc = get_u64(dev, CFSTR("locationID"));
            IOObjectRelease(dev);
        }
        IOObjectRelease(intf);

        if (g_intf_id) {
            logf_("another MTP device plugged in (%s %s %04x:%04x) - only one at a time, ignored",
                  vendor, product, vid, pid);
            continue;
        }
        g_intf_id = id;
        g_pending_dev = 0; /* it is in File transfer mode after all */
        snprintf(g_name, sizeof(g_name), "%s", product[0] ? product : "Android");
        sanitize(g_name);
        snprintf(g_mountpoint, sizeof(g_mountpoint), "/Volumes/%s", g_name);
        g_attempts = 0;
        g_ejected = 0;
        g_unlock_hinted = 0;
        g_loc = loc;
        const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
        if (g_reenum_loc && loc == g_reenum_loc && now < g_reenum_until) {
            /* the same port, moments after an eject: the phone reset its USB when the
             * MTP session closed. Still ejected until it is really plugged in again. */
            g_reenum_loc = 0;
            g_ejected = 1;
            logf_("%s came back right after the eject (USB reset); staying ejected until "
                  "it is plugged in again", product);
            event("plugged %s", g_name);
            event("ejected %s", g_name);
            continue;
        }
        g_reenum_loc = 0;
        logf_("MTP device plugged in: %s %s (%04x:%04x)", vendor, product, vid, pid);
        event("plugged %s", g_name);
        g_next_try = now + SETTLE_SECONDS;
    }
}

static void on_removal(void *refcon, io_iterator_t it)
{
    (void) refcon;
    io_service_t intf;
    while ((intf = IOIteratorNext(it))) {
        uint64_t id = 0;
        IORegistryEntryGetRegistryEntryID(intf, &id);
        IOObjectRelease(intf);
        if (id && id == g_intf_id) {
            logf_("MTP device gone: %s", g_name);
            const CFAbsoluteTime now = CFAbsoluteTimeGetCurrent();
            if (g_ejected && g_loc && now - g_eject_time < REENUM_SECONDS) {
                g_reenum_loc = g_loc;
                g_reenum_until = now + REENUM_SECONDS;
            }
            g_next_try = 0;
            stop_fs("phone unplugged or left File transfer mode");
            event("gone %s", g_name);
            g_intf_id = 0;
            g_ejected = 0;
        }
    }
}

/* USB vendors of common Android phones; only used for the "choose File transfer" hint. */
static int is_phone_vendor(int vid)
{
    static const int vendors[] = {
        0x18d1, /* Google */   0x04e8, /* Samsung */  0x2717, /* Xiaomi */
        0x2a70, /* OnePlus */  0x22b8, /* Motorola */ 0x12d1, /* Huawei */
        0x0fce, /* Sony */     0x22d9, /* Oppo, Realme */ 0x2b0e, /* Nothing (provisional) */
        0x1004, /* LG */       0x0bb4, /* HTC */      0x2d95, /* vivo */
    };
    for (size_t i = 0; i < sizeof(vendors) / sizeof(vendors[0]); i++)
        if (vendors[i] == vid)
            return 1;
    return 0;
}

static void on_device_arrival(void *refcon, io_iterator_t it)
{
    (void) refcon;
    io_service_t dev;
    while ((dev = IOIteratorNext(it))) {
        int vid = get_int(dev, CFSTR(kUSBVendorID));
        if (is_phone_vendor(vid) && !g_intf_id) {
            IORegistryEntryGetRegistryEntryID(dev, &g_pending_dev);
            g_pending_name[0] = 0;
            get_string(dev, CFSTR(kUSBProductString), g_pending_name, sizeof(g_pending_name));
            if (!g_pending_name[0])
                strcpy(g_pending_name, "phone");
            g_pending_since = CFAbsoluteTimeGetCurrent();
        }
        IOObjectRelease(dev);
    }
}

static void on_device_removal(void *refcon, io_iterator_t it)
{
    (void) refcon;
    io_service_t dev;
    while ((dev = IOIteratorNext(it))) {
        uint64_t id = 0;
        IORegistryEntryGetRegistryEntryID(dev, &id);
        if (id == g_pending_dev)
            g_pending_dev = 0;
        IOObjectRelease(dev);
    }
}

static void on_quit(int sig)
{
    (void) sig;
    g_quit_requested = 1;
}

static void on_eject(int sig)
{
    (void) sig;
    g_eject_requested = 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <simple-mtpfs> [extra mount options]\n", argv[0]);
        return 2;
    }
    g_fs_bin = argv[1];
    if (argc > 2)
        g_extra_opts = argv[2];
    signal(SIGTERM, on_quit);
    signal(SIGINT, on_quit);
    signal(SIGHUP, on_quit);
    signal(SIGUSR1, on_eject);

    IONotificationPortRef port = IONotificationPortCreate(kIOMainPortDefault);
    CFRunLoopAddSource(CFRunLoopGetCurrent(), IONotificationPortGetRunLoopSource(port),
                       kCFRunLoopDefaultMode);

    io_iterator_t arrive, leave, dev_arrive, dev_leave;
    IOServiceAddMatchingNotification(port, kIOFirstMatchNotification,
                                     IOServiceMatching("IOUSBHostInterface"), on_arrival, NULL,
                                     &arrive);
    IOServiceAddMatchingNotification(port, kIOTerminatedNotification,
                                     IOServiceMatching("IOUSBHostInterface"), on_removal, NULL,
                                     &leave);
    IOServiceAddMatchingNotification(port, kIOFirstMatchNotification,
                                     IOServiceMatching("IOUSBHostDevice"), on_device_arrival,
                                     NULL, &dev_arrive);
    IOServiceAddMatchingNotification(port, kIOTerminatedNotification,
                                     IOServiceMatching("IOUSBHostDevice"), on_device_removal,
                                     NULL, &dev_leave);
    logf_("watching for USB interfaces named MTP (any vendor)");
    event("watching");
    /* devices already attached; this also arms the notifications */
    on_device_arrival(NULL, dev_arrive);
    on_device_removal(NULL, dev_leave);
    on_arrival(NULL, arrive);
    on_removal(NULL, leave);

    CFRunLoopTimerRef t = CFRunLoopTimerCreate(NULL, CFAbsoluteTimeGetCurrent() + 0.5, 0.5, 0, 0,
                                               on_timer, NULL);
    CFRunLoopAddTimer(CFRunLoopGetCurrent(), t, kCFRunLoopDefaultMode);
    CFRunLoopRun();
    return 0;
}
