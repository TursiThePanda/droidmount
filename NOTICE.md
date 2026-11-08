# Third-party software in Droidmount

## simple-mtpfs
The filesystem (`fs/`) is a modified version of simple-mtpfs.
Copyright (C) 2012-2016 Peter Hatina; macOS changes (2023) by Su Z
(https://github.com/su-z/simple-mtpfs-mac). GPL-2.0-or-later, see `LICENSE`.

## libmtp
Statically linked. Copyright the libmtp authors, https://github.com/libmtp/libmtp.
GNU Lesser General Public License 2.1 or later. Source: libmtp 1.1.23 from
https://sourceforge.net/projects/libmtp/ (built unmodified by `scripts/build.sh`).

## libusb
Statically linked. Copyright the libusb authors, https://libusb.info.
GNU Lesser General Public License 2.1 or later.

## macFUSE
The macFUSE installer (`Install macFUSE.pkg`, unmodified, signed by its author) is
included in the app for convenience. macFUSE is Copyright (c) 2011-2026 Benjamin
Fleischer and is NOT covered by Droidmount's licence; its own licence is in
`Droidmount.app/Contents/Resources/macFUSE-LICENSE.txt`. Note its clause 4:
redistribution bundled with commercial software requires the author's written
permission. https://macfuse.github.io/
