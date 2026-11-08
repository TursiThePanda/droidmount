#!/usr/bin/env python3
"""Read the same files through two simple-mtpfs builds and compare SHA-256 (read-only).

Reads each file with several threads in parallel (different offsets) on the second build
to exercise chunked, concurrent reads.

usage: checksum_compare.py <binary A> <binary B> <path inside phone>...
"""
import hashlib
import os
import subprocess
import sys
import threading
import time

MP = "/Volumes/MTPCheck"


def mount(binary):
    p = subprocess.Popen([binary, "-f", MP, "-o", "ro,backend=fskit,local,volname=MTPCheck"],
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
        p.wait(timeout=5)
    except subprocess.TimeoutExpired:
        p.kill()
    time.sleep(2)


def sha_seq(path):
    h = hashlib.sha256()
    with open(path, "rb", buffering=0) as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                return h.hexdigest()
            h.update(b)


def sha_parallel(path, parts=4):
    size = os.path.getsize(path)
    step = (size + parts - 1) // parts or 1
    out = [b""] * parts

    def work(i):
        with open(path, "rb", buffering=0) as f:
            f.seek(i * step)
            want, buf = min(step, max(0, size - i * step)), bytearray()
            while len(buf) < want:
                b = f.read(min(1 << 20, want - len(buf)))
                if not b:
                    break
                buf += b
            out[i] = bytes(buf)

    ts = [threading.Thread(target=work, args=(i,)) for i in range(parts)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return hashlib.sha256(b"".join(out)).hexdigest()


def main():
    a, b, paths = sys.argv[1], sys.argv[2], sys.argv[3:]
    p = mount(a)
    ref = {x: sha_seq(os.path.join(MP, x)) for x in paths}
    umount(p)
    p = mount(b)
    ok = True
    for x in paths:
        full = os.path.join(MP, x)
        s1, s2 = sha_seq(full), sha_parallel(full)
        same = s1 == ref[x] and s2 == ref[x]
        ok &= same
        print(f"{'OK ' if same else 'BAD'} {os.path.getsize(full) // 1024:>8} KB  {x}")
    umount(p)
    print("all identical" if ok else "MISMATCH")


if __name__ == "__main__":
    main()
