#!/usr/bin/env python3
"""Read-write test on a SPARE phone. Refuses to run if a Google (Pixel) device is on USB.

Everything happens inside <root>/MTPFS_TEST on the phone, which is created and removed.
Checks: mkdir, small and 8 MB copies, overwrite, append, rename, macOS metadata refusal,
copy of a file with extended attributes, unlink, rmdir; then remounts and verifies what
is really on the phone (fresh listing from the device).

usage: write_test.py <simple-mtpfs binary>
"""
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time

MP = "/Volumes/MTPWriteTest"
GOOGLE_VID = 0x18D1


def pixel_present():
    out = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l"],
                         capture_output=True, text=True).stdout
    return f'"idVendor" = {GOOGLE_VID}' in out


def mtp_phones():
    """(vendor, product) of every USB interface named MTP."""
    out = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostInterface", "-l"],
                         capture_output=True, text=True).stdout
    phones, cur = [], {}
    for line in out.splitlines():
        if "+-o " in line:
            cur = {}
        for key in ("idVendor", "idProduct", "kUSBString"):
            if f'"{key}" = ' in line:
                cur[key] = line.split(" = ", 1)[1].strip().strip('"')
        if cur.get("kUSBString") == "MTP" and "idVendor" in cur and "idProduct" in cur:
            t = (int(cur["idVendor"]), int(cur["idProduct"]))
            if t not in phones:
                phones.append(t)
    return phones


def guard(allow_google):
    phones = mtp_phones()
    if len(phones) != 1:
        sys.exit(f"ABORT: need exactly one phone in File transfer mode, found {phones}")
    if phones[0][0] == 0x18D1 and not allow_google:
        sys.exit("ABORT: a Google device (Pixel) is connected. Pass --allow-google to test it.")
    print(f"testing phone {phones[0][0]:04x}:{phones[0][1]:04x}")


def mount(binary):
    p = subprocess.Popen([binary, "-f", MP, "-o", "backend=fskit,local,volname=MTPWriteTest"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(30):
        if MP in subprocess.run(["mount"], capture_output=True, text=True).stdout:
            return p
        time.sleep(0.5)
    p.kill()
    sys.exit("mount did not appear")


def umount(p):
    subprocess.run(["/sbin/umount", "-f", MP], capture_output=True)
    try:
        p.wait(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
    time.sleep(2)


def sha(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for b in iter(lambda: f.read(1 << 20), b""):
            h.update(b)
    return h.hexdigest()


results = []
META_NAMES = (".fseventsd", ".Spotlight-V100", ".Trashes", ".DS_Store")


def meta_snapshot():
    """macOS metadata in the phone root: name -> (mtime, entries)."""
    snap = {}
    for n in META_NAMES:
        full = os.path.join(MP, n)
        if os.path.exists(full):
            inner = sorted(os.listdir(full)) if os.path.isdir(full) else []
            snap[n] = (os.stat(full).st_mtime, inner)
    return snap


def check(name, cond, detail=""):
    results.append((name, bool(cond)))
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {detail}")


def expect_errno(name, fn):
    try:
        fn()
        check(name, False, "(was allowed)")
    except OSError as e:
        check(name, True, f"(refused: {e.strerror})")


def main():
    binary = sys.argv[1]
    guard("--allow-google" in sys.argv[2:])

    work = tempfile.mkdtemp(prefix="mtpwt-")
    small = os.path.join(work, "small.txt")
    big = os.path.join(work, "big.bin")
    big2 = os.path.join(work, "big2.bin")
    with open(small, "w") as f:
        f.write("hello from the Mac\n")
    with open(big, "wb") as f:
        f.write(os.urandom(8 * 1024 * 1024))
    with open(big2, "wb") as f:
        f.write(os.urandom(3 * 1024 * 1024 + 123))
    subprocess.run(["xattr", "-w", "com.example.test", "value", small])

    p = mount(binary)
    try:
        storage = MP  # single-storage phones show their content at the root
        test = os.path.join(storage, "MTPFS_TEST")
        meta_before = meta_snapshot()
        if os.path.exists(test):
            sys.exit(f"ABORT: {test} already exists on the phone, not touching it")
        os.mkdir(test)
        check("mkdir", os.path.isdir(test))

        t0 = time.monotonic()
        shutil.copyfile(small, os.path.join(test, "small.txt"))
        shutil.copyfile(big, os.path.join(test, "big.bin"))
        dt = time.monotonic() - t0
        check("copy small", sha(os.path.join(test, "small.txt")) == sha(small))
        check("copy 8 MB", sha(os.path.join(test, "big.bin")) == sha(big),
              f"({8 / dt:.1f} MB/s incl. small)")

        shutil.copyfile(big2, os.path.join(test, "big.bin"))
        check("overwrite with shorter file", sha(os.path.join(test, "big.bin")) == sha(big2)
              and os.path.getsize(os.path.join(test, "big.bin")) == os.path.getsize(big2))

        with open(os.path.join(test, "small.txt"), "a") as f:
            f.write("appended line\n")
        with open(os.path.join(test, "small.txt")) as f:
            check("append", f.read() == "hello from the Mac\nappended line\n")

        os.rename(os.path.join(test, "small.txt"), os.path.join(test, "renamed.txt"))
        check("rename", os.path.exists(os.path.join(test, "renamed.txt"))
              and not os.path.exists(os.path.join(test, "small.txt")))

        expect_errno("refuse .DS_Store", lambda: open(os.path.join(test, ".DS_Store"), "w").close())
        expect_errno("refuse ._ AppleDouble", lambda: open(os.path.join(test, "._renamed.txt"), "w").close())
        expect_errno("refuse .fseventsd", lambda: os.mkdir(os.path.join(test, ".fseventsd")))

        r = subprocess.run(["cp", small, os.path.join(test, "with_xattr.txt")],
                           capture_output=True, text=True)
        check("cp of a file with xattrs succeeds", r.returncode == 0, r.stderr.strip())
        names = sorted(os.listdir(test))
        check("no ._ files created", not any(n.startswith("._") for n in names), str(names))

        os.unlink(os.path.join(test, "with_xattr.txt"))
        check("unlink", not os.path.exists(os.path.join(test, "with_xattr.txt")))

        # editor-style atomic save: write a temp file, rename it over the target
        with open(os.path.join(test, "doc.txt"), "w") as f:
            f.write("version 1\n")
        with open(os.path.join(test, "doc.txt.tmp"), "w") as f:
            f.write("version 2\n")
        os.rename(os.path.join(test, "doc.txt.tmp"), os.path.join(test, "doc.txt"))
        with open(os.path.join(test, "doc.txt")) as f:
            check("atomic save (rename over existing)", f.read() == "version 2\n")

        # rename while the file is still open for writing
        fh = open(os.path.join(test, "moving.txt"), "w")
        fh.write("part 1\n")
        fh.flush()
        os.rename(os.path.join(test, "moving.txt"), os.path.join(test, "moved.txt"))
        fh.write("part 2\n")
        fh.close()
        with open(os.path.join(test, "moved.txt")) as f:
            check("rename while open for writing", f.read() == "part 1\npart 2\n")

        # a second reader sees what the writer wrote, before close
        w = open(os.path.join(test, "live.txt"), "w")
        w.write("live data\n")
        w.flush()
        with open(os.path.join(test, "live.txt")) as r:
            check("reader of a file being written", r.read() == "live data\n")
        w.close()

        # delete a file before it was ever closed: it must not appear on the phone
        g = open(os.path.join(test, "ghost.txt"), "w")
        g.write("should never arrive\n")
        g.flush()
        os.unlink(os.path.join(test, "ghost.txt"))
        g.close()
        check("unlink while open", not os.path.exists(os.path.join(test, "ghost.txt")))

        # move into a subfolder (MTP MoveObject or EXDEV fallback by the caller)
        os.mkdir(os.path.join(test, "sub"))
        r = subprocess.run(["mv", os.path.join(test, "live.txt"), os.path.join(test, "sub", "live.txt")],
                           capture_output=True, text=True)
        check("move into subfolder", r.returncode == 0 and
              os.path.exists(os.path.join(test, "sub", "live.txt")) and
              not os.path.exists(os.path.join(test, "live.txt")), r.stderr.strip())
    finally:
        umount(p)

    # Fresh mount: everything below comes from the phone, not from our cache.
    p = mount(binary)
    try:
        test = os.path.join(MP, "MTPFS_TEST")
        names = sorted(os.listdir(test))
        check("on phone: expected files",
              names == ["big.bin", "doc.txt", "moved.txt", "renamed.txt", "sub"], str(names))
        check("on phone: no leftover upload temp names",
              not any(".mtpfs-" in n for n in names), str(names))
        check("on phone: big.bin content", sha(os.path.join(test, "big.bin")) == sha(big2))
        with open(os.path.join(test, "renamed.txt")) as f:
            check("on phone: renamed.txt content", f.read() == "hello from the Mac\nappended line\n")
        with open(os.path.join(test, "doc.txt")) as f:
            check("on phone: atomic save content", f.read() == "version 2\n")
        with open(os.path.join(test, "moved.txt")) as f:
            check("on phone: renamed-while-open content", f.read() == "part 1\npart 2\n")
        with open(os.path.join(test, "sub", "live.txt")) as f:
            check("on phone: moved file content", f.read() == "live data\n")
        storage = MP
        meta_after = meta_snapshot()
        # macOS may delete its own leftovers; it must not create anything new
        grown = {n: v for n, v in meta_after.items()
                 if n not in meta_before or not set(v[1]) <= set(meta_before[n][1])}
        check("no macOS metadata created on the phone", not grown,
              f"before {meta_before} after {meta_after}")
        os.unlink(os.path.join(test, "sub", "live.txt"))
        os.rmdir(os.path.join(test, "sub"))
        for n in os.listdir(test):
            os.unlink(os.path.join(test, n))
        os.rmdir(test)
        check("cleanup rmdir", not os.path.exists(test))
    finally:
        umount(p)
        shutil.rmtree(work, ignore_errors=True)

    failed = [n for n, ok in results if not ok]
    print(f"\n{len(results) - len(failed)}/{len(results)} passed" +
          (f"; FAILED: {', '.join(failed)}" if failed else ""))


if __name__ == "__main__":
    main()
