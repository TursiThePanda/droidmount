# Droidmount

**Your Android phone in Finder, like a USB drive.** Plug it in, choose *File transfer* on
the phone, and it appears in Finder and on the desktop — browse your photos with
thumbnails, copy files both ways, eject, unplug. No extra file-transfer app.

Works with any Android phone (Pixel, Samsung, …) over MTP. macOS 26 or later, Apple
Silicon.

> Droidmount is free software (GPL-2.0-or-later). It is not affiliated with or endorsed
> by Google or Apple.

## Install

1. Download `Droidmount-<version>.dmg` from [Releases](../../releases) and drag
   **Droidmount** to **Applications**.
2. Open it. Droidmount is not signed with a paid Apple developer certificate, so the
   first time macOS blocks it: open **System Settings → Privacy & Security**, scroll down
   to *"Droidmount was blocked…"*, click **Open Anyway** and confirm. Only once.
3. Droidmount's setup window takes you through the rest and ticks each step off by
   itself:
   - **Install macFUSE** — the installer comes with Droidmount; macOS asks for your
     password once.
   - **Allow the macFUSE file system extension** — Droidmount opens the right page of
     System Settings. Switch the list to **By Category**, open **File System
     Extensions** ⓘ and turn on **macFUSE** and **macFUSE (local)**. (The switches in the
     default view do not respond — [macfuse#1194](https://github.com/macfuse/macfuse/issues/1194).)
   - **Start at login** — recommended.
4. Plug in your phone, **unlock it**, and choose **File transfer** in its USB
   notification.

Droidmount lives in the menu bar: phone status, *Open in Finder*, *Eject*, a *Read-only*
switch, *Start at login*, *Check for Updates…*.

**Updates** come by themselves (from 0.1.1 on): Droidmount asks once whether to check
automatically, then looks once a day and offers each new version with its release notes.
Every update is signed, and a download that does not match the signature is refused.
Version 0.1.0 has no updater yet — install 0.1.1 over it by hand once.

### Or with Homebrew

```sh
brew tap tursithepanda/tap
brew trust --cask tursithepanda/tap/droidmount   # Homebrew asks you to trust third-party casks once
brew install --cask tursithepanda/tap/droidmount
```

This installs macFUSE as well. The *Open Anyway* step and enabling the macFUSE extension
(steps 2 and 3 above) are still needed. Droidmount is not in Homebrew's main repository,
which only accepts apps signed with a paid Apple developer account.

## Good to know

- **Copying to the phone is safe.** A file is sent when the copy finishes, under a
  temporary name; the old version on the phone is replaced only after the new one has
  arrived. If sending fails (phone locked, storage full, cable pulled), nothing is lost:
  the file is kept in **Unsent files** (menu bar) and you get a notification.
- **Unlock the phone.** Android shares its files only while the phone is unlocked;
  GrapheneOS also blocks new USB data connections while locked. Droidmount tells you when
  this is the likely problem.
- **macOS's hidden files stay off the phone** (`.DS_Store`, `._*`, `.fseventsd`,
  `.Spotlight-V100`, …).
- **Speed** is limited by MTP and the cable: a USB 2 cable gives roughly 5–20 MB/s.
  Opening a large folder the first time takes a moment; after that it is cached.
- One phone at a time.

## How it works

| Part | What it is |
|---|---|
| `fs/` | The filesystem: a heavily modified [simple-mtpfs](https://github.com/phatina/simple-mtpfs) (via [Su Z's macOS fork](https://github.com/su-z/simple-mtpfs-mac)), on [libmtp](https://github.com/libmtp/libmtp) and macFUSE's **FSKit** backend (user space, no kernel extension). |
| `watcher/` | Watches USB with IOKit and mounts any phone whose USB interface is called "MTP" — no vendor list. |
| `probe/` | Checks whether the macFUSE FSKit module is enabled by mounting an empty test volume. |
| `app/` | The menu bar app (SwiftUI) and its setup assistant. |

What makes it usable:

- **Batched folder listing.** libmtp asks the phone about every file separately, because
  it marks `GetObjectPropList` as broken on all Android devices. Asking for one property
  of all children at once works on the phones tested and lists a 173-photo camera folder
  in **0.22 s instead of 4.2 s**. If a phone answers incompletely, Droidmount falls back
  to the slow way automatically.
- **Folder listings never wait behind file reads**: a two-level lock lets listings
  overtake reads, which are done in 512 KiB pieces. Listing latency while large files
  are copied: **~40 ms instead of ~2 s**.

## Build from source

Needs Xcode 26 or later (or the Command Line Tools without the layered icon), Homebrew
`cmake` and `pkg-config`, and macFUSE 5.5+ installed on the build machine.

```sh
scripts/build.sh      # -> build/Droidmount.app and build/Droidmount-<version>.dmg
```

The script downloads libusb and libmtp (checksums verified), builds them for macOS 26,
links them statically, compiles the app and the layered icon, bundles the signed macFUSE
installer and makes the DMG.

Releases are built by GitHub Actions (`.github/workflows/release.yml`, macOS 27 /
Xcode 27 runner): bump `VERSION`, add `docs/release-notes/<version>.md`, commit, then
`git tag -a v<version> -m "Droidmount <version>" && git push origin main --follow-tags`.
The workflow builds the DMG, signs it for Sparkle and attaches the DMG, its `.sha256`
and `appcast.xml` (the update feed) to the release; then update `version` and `sha256`
in the [Homebrew cask](https://github.com/TursiThePanda/homebrew-tap).

Updates are signed with an EdDSA key. Its public half is `SUPublicEDKey` in
`app/Info.plist`; the private half lives in the maintainer's login Keychain (Sparkle's
`generate_keys --account droidmount`) and in the repository secret
`SPARKLE_ED_PRIVATE_KEY`. Losing it means existing installs can no longer verify
updates, so keep a backup.

`tests/` holds the benchmarks and the read-write tests. The write tests refuse to run
unless exactly one phone is connected, work only inside their own `MTPFS_TEST` folder,
and need `--allow-google` before they write to a Google phone.

## Credits and licences

- Droidmount: GPL-2.0-or-later, see [LICENSE](LICENSE).
- simple-mtpfs by Peter Hatina, macOS port by Su Z — GPL-2.0-or-later.
- libmtp and libusb — LGPL-2.1-or-later, linked statically.
- macFUSE by Benjamin Fleischer — its own licence, included in the app; its installer is
  bundled unchanged. Note that macFUSE's licence does not allow bundling it with
  *commercial* software without the author's permission.
- Sparkle (automatic updates) — MIT licence, included in the app.
- The Android robot is reproduced or modified from work created and shared by Google and
  used according to terms described in the Creative Commons 3.0 Attribution License.
  Android is a trademark of Google LLC.

Details in [NOTICE.md](NOTICE.md).
