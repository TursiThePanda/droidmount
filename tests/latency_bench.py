#!/usr/bin/env python3
"""Folder-listing latency while big files are being read (read-only).

Mounts the phone with the given simple-mtpfs binary at /Volumes/MTPBench, starts N
readers on the largest files in --big-dir, then times `os.listdir` on folders the
filesystem has not listed yet. Unmounts at the end. Nothing is written to the phone.

usage: latency_bench.py <simple-mtpfs binary> [--readers 4] [--big-dir Movies]
"""
import argparse
import os
import subprocess
import sys
import threading
import time

MP = "/Volumes/MTPBench"


def mounted():
    return MP in subprocess.run(["mount"], capture_output=True, text=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--readers", type=int, default=4)
    ap.add_argument("--big-dir", default="Movies")
    ap.add_argument("--folders", type=int, default=8)
    a = ap.parse_args()

    fs = subprocess.Popen([a.binary, "-f", MP, "-o", "ro,backend=fskit,local,volname=MTPBench"],
                          stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(30):
            if mounted():
                break
            time.sleep(0.5)
        else:
            sys.exit("mount did not appear")

        root = sorted(os.listdir(MP))
        big = os.path.join(MP, a.big_dir)
        files = [os.path.join(big, f) for f in os.listdir(big)]
        files = [f for f in files if os.path.isfile(f)]
        files.sort(key=os.path.getsize, reverse=True)
        files = files[: a.readers]
        print(f"readers on {len(files)} files: " +
              ", ".join(f"{os.path.getsize(f) // (1024 * 1024)} MB" for f in files))

        stop = threading.Event()
        read_bytes = [0] * len(files)

        def reader(i, path):
            with open(path, "rb", buffering=0) as fh:
                while not stop.is_set():
                    chunk = fh.read(1024 * 1024)
                    if not chunk:
                        fh.seek(0)
                        continue
                    read_bytes[i] += len(chunk)

        threads = [threading.Thread(target=reader, args=(i, f), daemon=True)
                   for i, f in enumerate(files)]
        t_start = time.monotonic()
        for t in threads:
            t.start()
        time.sleep(1.5)

        # folders not yet listed: skip the big dir, walk root in reverse order
        candidates = [d for d in reversed(root)
                      if d != a.big_dir and os.path.isdir(os.path.join(MP, d))]
        lat = []
        for d in candidates[: a.folders]:
            t0 = time.monotonic()
            n = len(os.listdir(os.path.join(MP, d)))
            dt = (time.monotonic() - t0) * 1000
            lat.append(dt)
            print(f"  ls {d:<28} {n:5d} entries  {dt:8.1f} ms")
        stop.set()
        elapsed = time.monotonic() - t_start
        for t in threads:
            t.join(timeout=10)
        mb = sum(read_bytes) / (1024 * 1024)
        lat.sort()
        print(f"listing latency: median {lat[len(lat) // 2]:.0f} ms, max {lat[-1]:.0f} ms; "
              f"readers moved {mb:.0f} MB in {elapsed:.1f} s ({mb / elapsed:.1f} MB/s)")
    finally:
        subprocess.run(["/sbin/umount", "-f", MP], capture_output=True)
        try:
            fs.wait(timeout=5)
        except subprocess.TimeoutExpired:
            fs.kill()


if __name__ == "__main__":
    main()
