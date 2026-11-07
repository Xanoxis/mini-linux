#!/usr/bin/env python3
"""Copy the rootfs UUIDs (pmos_boot_uuid / pmos_root_uuid) from the kernel command
line of one Android boot image into another.

Every "pmbootstrap install" creates a new root filesystem with NEW UUIDs and puts
them into boot.img. A kernel that should boot the rootfs ALREADY on the watch
needs the old UUIDs, otherwise the initramfs stops in the PMOS_LOGS debug shell
("failed to mount subpartitions").

  bootimg_uuid.py REFERENCE.img NEW.img OUT.img
"""
import re
import sys

KEYS = ("pmos_boot_uuid", "pmos_root_uuid")


def cmdline(buf):
    return bytes(buf[64:576]).split(b"\0")[0].decode()


def main(ref, new, out):
    r = open(ref, "rb").read(4096)
    n = bytearray(open(new, "rb").read())
    for b, name in ((r, ref), (n, new)):
        if b[:8] != b"ANDROID!":
            sys.exit(f"{name}: not an Android boot image")
    rc, nc = cmdline(r), cmdline(n)
    for k in KEYS:
        m = re.search(k + r"=(\S+)", rc)
        if not m:
            sys.exit(f"{ref}: no {k} in its command line")
        nc = re.sub(k + r"=\S+", f"{k}={m.group(1)}", nc)
    if len(nc) >= 512:
        sys.exit("command line too long")
    n[64:576] = nc.encode().ljust(512, b"\0")
    open(out, "wb").write(n)
    print(f"{out}: {nc}")


if __name__ == "__main__":
    if len(sys.argv) != 4:
        sys.exit(__doc__)
    main(*sys.argv[1:])
