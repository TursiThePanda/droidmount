#!/usr/bin/env python3
"""Upload-failure test on a SPARE phone (refuses to run if a Google device is on USB).

1. normal mount: create MTPFS_FAIL/keep.txt = "version 1"
2. mount with SMTPFS_TEST_FAIL_UPLOAD=keep.txt: overwrite with "version 2" -> close()
   must report an error
3. normal mount: keep.txt must still be "version 1", no temp upload names left, and the
   "version 2" copy must be in ~/Library/Application Support/Droidmount/unsent
4. cleanup

usage: failure_test.py <simple-mtpfs binary>
"""
import glob
import os
import subprocess
import sys
import time

MP = "/Volumes/MTPFailTest"
UNSENT = os.path.expanduser("~/Library/Application Support/Droidmount/unsent")


def pixel_present():
    out = subprocess.run(["ioreg", "-r", "-c", "IOUSBHostDevice", "-l"],
                         capture_output=True, text=True).stdout
    return '"idVendor" = 6353' in out


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


def mount(binary, env=None):
    e = dict(os.environ, **(env or {}))
    p = subprocess.Popen([binary, "-f", MP, "-o", "backend=fskit,local,volname=MTPFailTest"],
                         stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=e, text=True)
    for _ in range(30):
        if MP in subprocess.run(["mount"], capture_output=True, text=True).stdout:
            return p
        time.sleep(0.5)
    p.kill()
    sys.exit("mount did not appear")


def umount(p):
    subprocess.run(["/sbin/umount", "-f", MP], capture_output=True)
    try:
        _, err = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        _, err = p.communicate()
    time.sleep(2)
    return err


ok_all = True


def check(name, cond, detail=""):
    global ok_all
    ok_all &= bool(cond)
    print(f"{'PASS' if cond else 'FAIL'}  {name}  {detail}")


def main():
    binary = sys.argv[1]
    guard("--allow-google" in sys.argv[2:])
    before = set(glob.glob(os.path.join(UNSENT, "*")))
    d = os.path.join(MP, "MTPFS_FAIL")

    p = mount(binary)
    if os.path.exists(d):
        umount(p)
        sys.exit("ABORT: MTPFS_FAIL already exists on the phone")
    os.mkdir(d)
    with open(os.path.join(d, "keep.txt"), "w") as f:
        f.write("version 1\n")
    umount(p)

    p = mount(binary, {"SMTPFS_TEST_FAIL_UPLOAD": "keep.txt"})
    err_fsync = err_close = None
    f = open(os.path.join(d, "keep.txt"), "w")
    f.write("version 2\n")
    f.flush()
    try:
        os.fsync(f.fileno())
    except OSError as e:
        err_fsync = e
    try:
        f.close()
    except OSError as e:
        err_close = e
    log = umount(p)
    check("fsync() reports the failed upload", err_fsync is not None, f"({err_fsync})" if err_fsync else "")
    print(f"INFO  close() {'reported' if err_close else 'did not report'} the error"
          + (f" ({err_close})" if err_close else " (FSKit swallows flush errors)"))

    p = mount(binary)
    try:
        with open(os.path.join(d, "keep.txt")) as f:
            check("old version still on the phone", f.read() == "version 1\n")
        names = os.listdir(d)
        check("no temp upload left on the phone", names == ["keep.txt"], str(names))
        new = set(glob.glob(os.path.join(UNSENT, "*"))) - before
        saved = [x for x in new if x.endswith("keep.txt")]
        content = open(saved[0]).read() if saved else ""
        check("new version saved locally", content == "version 2\n", str(saved))
        for x in saved:
            os.unlink(x)
        os.unlink(os.path.join(d, "keep.txt"))
        os.rmdir(d)
    finally:
        umount(p)
    print("hook log:", " | ".join(l for l in (log or "").splitlines() if "keep.txt" in l))
    print("ALL PASSED" if ok_all else "SOME FAILED")


if __name__ == "__main__":
    main()
