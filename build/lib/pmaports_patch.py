#!/usr/bin/env python3
"""Edit a pmaports checkout so it can build an LG Watch Urbane (lg-bass) image.

Subcommands
  device  DIR [--offsets JSON]   adapt a copy of device-lg-lenok in DIR
  kernel  DIR DTS                 teach linux-postmarketos-qcom-msm8226 to build the bass DTB
  offsets BOOTIMG                 print mkbootimg addresses of an Android boot image as JSON

Every edit is idempotent: running it twice gives the same result.
"""
import json
import re
import shutil
import struct
import sys
from pathlib import Path

MARK_BEGIN = "# --- lg-bass (LG Watch Urbane) support, added by SmartLinux kit ---"
MARK_END = "# --- end lg-bass ---"

# Fallback boot image layout used by msm8226 LG watches with the stock LK.
# Replaced by the real values read from a known-good bass boot image when available.
DEFAULT_OFFSETS = {
    "base": 0x00000000,
    "kernel": 0x00008000,
    "ramdisk": 0x02000000,
    "second": 0x00F00000,
    "tags": 0x01E00000,
    "pagesize": 2048,
}

KERNEL_CONFIG_WANT = {
    # Bluetooth keyboards: classic (HIDP) and BLE (UHID via bluez)
    "CONFIG_BT_HIDP": "m",
    "CONFIG_UHID": "m",
    # Compressed swap in RAM - the watch only has 512 MB
    "CONFIG_ZRAM": "m",
    # sysfs knob for the CPU rail voltage (watch-cpu volt); built in because a
    # RAM-booted test kernel cannot load modules from the older rootfs
    "CONFIG_REGULATOR_VIRTUAL_CONSUMER": "y",
    # a rebuilt test kernel must still load the modules already on the watch:
    # without this, any config change makes their BTF "invalid" (ipv6, wifi, ...)
    "CONFIG_MODULE_ALLOW_BTF_MISMATCH": "y",
    # "reboot bootloader" -> fastboot: writes LK's magic (0x77665500) to IMEM 0x65c
    # (qcom-msm8226.dtsi already has the syscon-reboot-mode node)
    "CONFIG_POWER_RESET": "y",
    "CONFIG_REBOOT_MODE": "y",
    "CONFIG_SYSCON_REBOOT_MODE": "y",
}


def die(msg):
    print(f"error: {msg}", file=sys.stderr)
    sys.exit(1)


# ---------------------------------------------------------------- helpers
def set_kv(text, key, value):
    """Set key="value" in a shell-style file, replacing or appending."""
    line = f'{key}="{value}"'
    pat = re.compile(rf"^{re.escape(key)}=.*$", re.M)
    if pat.search(text):
        return pat.sub(lambda _m: line, text, count=1)
    if not text.endswith("\n"):
        text += "\n"
    return text + line + "\n"


def del_kv(text, key):
    return re.sub(rf"^{re.escape(key)}=.*\n?", "", text, flags=re.M)


def find_var_block(text, var):
    """Return (start, end) of the value of var="..." (multi-line aware)."""
    m = re.search(rf'^{var}="', text, re.M)
    if not m:
        return None
    start = m.end()
    end = text.index('"', start)
    return start, end


def remove_token(text, var, token):
    blk = find_var_block(text, var)
    if not blk:
        return text
    s, e = blk
    val = text[s:e]
    new = re.sub(rf"(?m)^[ \t]*{re.escape(token)}[ \t]*\n", "", val)
    new = re.sub(rf"(?<![\w.-]){re.escape(token)}(?![\w.-])[ \t]*", "", new)
    return text[:s] + new + text[e:]


def add_token(text, var, token):
    blk = find_var_block(text, var)
    if not blk:
        return set_kv(text, var, token)
    s, e = blk
    val = text[s:e]
    if re.search(rf"(?<![\w.-]){re.escape(token)}(?![\w.-])", val):
        return text
    if "\n" in val:
        # multi-line list: keep the closing quote on its own (indented) line
        stripped = val.rstrip(" \t")
        tail = val[len(stripped):]
        new = stripped + ("" if stripped.endswith("\n") else "\n") + f"\t{token}\n" + tail
    else:
        new = (val + " " + token).strip()
    return text[:s] + new + text[e:]


def bump_pkgrel(text):
    m = re.search(r"^pkgrel=(\d+)", text, re.M)
    if not m:
        die("APKBUILD has no pkgrel")
    return text[: m.start(1)] + str(int(m.group(1)) + 1) + text[m.end(1):]


# ---------------------------------------------------------------- device
def cmd_device(d, offsets_json=None):
    d = Path(d)
    apk = d / "APKBUILD"
    info = d / "deviceinfo"
    if not apk.exists() or not info.exists():
        die(f"{d} does not look like a device package")

    off = dict(DEFAULT_OFFSETS)
    if offsets_json:
        off.update(json.loads(offsets_json))

    # APKBUILD -------------------------------------------------------
    t = apk.read_text()
    t = re.sub(r'^pkgdesc=.*$', 'pkgdesc="LG Watch Urbane"', t, count=1, flags=re.M)
    # lk2nd does not know the Urbane yet: boot straight from the stock LG bootloader
    for var in ("depends", "makedepends"):
        t = remove_token(t, var, "lk2nd-msm8226")
    apk.write_text(t)
    left = [l for l in t.splitlines() if "lk2nd" in l]
    if left:
        print("WARNING: APKBUILD still mentions lk2nd, check these lines by hand:")
        for l in left:
            print("   ", l)

    # deviceinfo -------------------------------------------------------
    t = info.read_text()
    for k in ("deviceinfo_generate_extlinux_config",):
        t = del_kv(t, k)
    hx = lambda v: f"0x{v:08x}"
    wanted = {
        "deviceinfo_name": "LG Watch Urbane",
        "deviceinfo_codename": "lg-bass",
        "deviceinfo_year": "2015",
        "deviceinfo_chassis": "watch",
        "deviceinfo_screen_width": "320",
        "deviceinfo_screen_height": "320",
        "deviceinfo_flash_method": "fastboot",
        "deviceinfo_generate_bootimg": "true",
        "deviceinfo_append_dtb": "true",
        "deviceinfo_bootimg_qcdt": "false",
        "deviceinfo_flash_offset_base": hx(off["base"]),
        "deviceinfo_flash_offset_kernel": hx(off["kernel"]),
        "deviceinfo_flash_offset_ramdisk": hx(off["ramdisk"]),
        "deviceinfo_flash_offset_second": hx(off["second"]),
        "deviceinfo_flash_offset_tags": hx(off["tags"]),
        "deviceinfo_flash_pagesize": str(off["pagesize"]),
        "deviceinfo_flash_fastboot_partition_kernel": "boot",
        "deviceinfo_flash_fastboot_partition_rootfs": "userdata",
        "deviceinfo_flash_sparse": "true",
        # LG's bootloader always loads the initramfs at 0x02900000 and protected firmware
        # memory starts at 0x02f00000: the initramfs MUST stay below 6 MiB. Keep only the
        # minimal first stage in boot.img; everything else goes to initramfs-extra on pmOS_boot.
        "deviceinfo_create_initfs_extra": "true",
        # stage 1 always unpacks initramfs-extra with "gzip -d" (init_functions.sh)
        "deviceinfo_initfs_extra_compression": "gzip",
        # Google's 18D1:D001 makes Windows bind the ADB driver instead of the USB network
        # driver. Use the Linux Foundation composite-gadget ID so the NCM class driver binds.
        "deviceinfo_usb_idVendor": "0x1D6B",
        "deviceinfo_usb_idProduct": "0x0104",
    }
    for k, v in wanted.items():
        t = set_kv(t, k, v)
    if "deviceinfo_dtb=" not in t:
        t = set_kv(t, "deviceinfo_dtb", "qcom/qcom-apq8026-lg-bass")
    t = re.sub(r'(?m)^(deviceinfo_dtb=")([^"]*)lenok', r"\1\2bass", t)
    if "SmartLinux kit" not in t:
        t = ("# Derived from device-lg-lenok by the SmartLinux kit (build/02-add-watch-port.sh).\n"
             "# Boots via the stock LG bootloader with an appended DTB (no lk2nd).\n") + t
    t = del_kv(t, "deviceinfo_modules_initfs")
    info.write_text(t)
    # no kernel modules in the first-stage initramfs (display/touch load later from the rootfs)
    mi = d / "modules-initfs"
    if mi.exists():
        mi.write_text("# kept empty: LG bootloader limits the initramfs to 6 MiB\n")
    print(f"device package ready: {d}")


# ---------------------------------------------------------------- kernel
SNIPPET = r'''	{begin}
	_lenok_dts=$(find "$builddir"/arch/arm/boot/dts -name qcom-apq8026-lg-lenok.dts | head -n 1)
	[ -n "$_lenok_dts" ] || die "lenok dts not found in kernel tree"
	_dtsdir=$(dirname "$_lenok_dts")
	if [ ! -e "$_dtsdir"/qcom-apq8026-lg-bass.dts ]; then
		cp "$srcdir"/qcom-apq8026-lg-bass.dts "$_dtsdir"/
		sed -i 's/^\([[:space:]]*\)qcom-apq8026-lg-lenok\.dtb/\1qcom-apq8026-lg-bass.dtb \\\n&/' "$_dtsdir"/Makefile
		grep -q qcom-apq8026-lg-bass.dtb "$_dtsdir"/Makefile || die "could not add bass dtb to Makefile"
	fi
	{end}
'''.format(begin=MARK_BEGIN, end=MARK_END)

SMBB_MARK = "# --- smbb disable-btc v2 (SmartLinux kit) ---"
SMBB_SNIPPET = r"""	{mark}
	_smbb="$builddir"/drivers/power/supply/qcom_smbb.c
	if ! grep -q "qcom,disable-btc" "$_smbb"; then
		# must run AFTER the smbb_charger_setup[] table, which re-enables the comparator:
		# insert just before the final "platform_set_drvdata(pdev, chg);" of the probe
		_ln=$(grep -n 'platform_set_drvdata(pdev, chg);' "$_smbb" | tail -n 1 | cut -d: -f1)
		[ -n "$_ln" ] || die "qcom_smbb.c: anchor not found"
		_ln=$((_ln - 1))
		sed -i "${{_ln}}r $srcdir/smbb-disable-btc.c" "$_smbb"
		grep -q "qcom,disable-btc" "$_smbb" || die "qcom_smbb.c patch failed"
	fi
	# --- end smbb ---
""".format(mark=SMBB_MARK)

SMBB_C = """
	/* SmartLinux kit: some boards (LG Watch Urbane) leave the PMIC battery
	 * thermistor unconnected; the vendor kernel disables the comparator. */
	if (of_property_read_bool(pdev->dev.of_node, "qcom,disable-btc")) {
		unsigned int val = 0;

		regmap_write(chg->regmap, chg->addr + 0x2d0, SEC_ACCESS_MAGIC);
		regmap_update_bits(chg->regmap, chg->addr + SMBB_BAT_BTC_CTRL,
				   BTC_CTRL_COMP_EN, 0);
		regmap_read(chg->regmap, chg->addr + SMBB_BAT_BTC_CTRL, &val);
		dev_info(&pdev->dev, "battery temperature comparator disabled (BTC_CTRL=0x%02x)\\n", val);
	}
"""


FTS_MARK = "# --- ftsmps range split v1 (SmartLinux kit) ---"
FTS_SNIPPET = r"""	{mark}
	# PM8226 S2 (CPU rail) is an FTS buck that LG's bootloader leaves in range 1
	# (10 mV steps) at 1.15 V. Mainline only accepts range 1 from 1.28 V, so the
	# voltage reads back as "Invalid argument" and setting it would switch ranges
	# (risking a dip). Split the ranges at 1.15 V instead.
	_spmi="$builddir"/drivers/regulator/qcom_spmi-regulator.c
	if ! grep -q "SmartLinux: FTS range split" "$_spmi"; then
		sed -i -E \
			-e 's/SPMI_VOLTAGE_RANGE\(0, +0, +350000, +1275000, +1275000, +5000\),/SPMI_VOLTAGE_RANGE(0,       0,  350000, 1145000, 1275000,  5000), \/* SmartLinux: FTS range split *\//' \
			-e 's/SPMI_VOLTAGE_RANGE\(1, +0, +1280000, +2040000, +2040000, +10000\),/SPMI_VOLTAGE_RANGE(1,       0, 1150000, 2040000, 2040000, 10000),/' \
			"$_spmi"
		grep -q "SmartLinux: FTS range split" "$_spmi" && grep -q "0, 1150000, 2040000" "$_spmi" ||
			die "qcom_spmi-regulator.c patch failed"
	fi
	# --- end ftsmps ---
""".format(mark=FTS_MARK)


def cmd_kernel(d, dts):
    d = Path(d)
    apk = d / "APKBUILD"
    if not apk.exists():
        die(f"{apk} missing")
    shutil.copy(dts, d / "qcom-apq8026-lg-bass.dts")
    t = apk.read_text()
    changed = False

    if MARK_BEGIN not in t:
        t = add_token(t, "source", "qcom-apq8026-lg-bass.dts")
        m = re.search(r"(?m)^prepare\(\)\s*\{\s*\n", t)
        if m:
            close = re.search(r"(?m)^\}\s*$", t[m.end():])
            if not close:
                die("could not find end of prepare()")
            pos = m.end() + close.start()
            t = t[:pos] + SNIPPET + t[pos:]
        else:
            # no prepare(): add one before build()
            b = re.search(r"(?m)^build\(\)", t)
            if not b:
                die("kernel APKBUILD has neither prepare() nor build()")
            t = t[:b.start()] + "prepare() {\n\tdefault_prepare\n" + SNIPPET + "}\n\n" + t[b.start():]
        changed = True

    # kernel config: make sure BT keyboards and zram are possible
    cfgs = [p for p in d.iterdir() if p.name.startswith("config-") and "armv7" in p.name]
    for cfg in cfgs:
        c = cfg.read_text()
        add = []
        for opt, val in KERNEL_CONFIG_WANT.items():
            if re.search(rf"(?m)^{opt}=[ym]$", c):
                continue
            c = re.sub(rf"(?m)^# {opt} is not set\n", "", c)
            add.append(f"{opt}={val}")
        if add:
            cfg.write_text(c.rstrip("\n") + "\n" + "\n".join(add) + "\n")
            print(f"{cfg.name}: enabled {', '.join(add)}")
            changed = True

    # smbb disable-btc patch (added later; independent marker)
    (d / "smbb-disable-btc.c").write_text(SMBB_C)
    if SMBB_MARK not in t:
        # drop an older version of the block
        t = re.sub(r"(?ms)^\t# --- smbb disable-btc.*?# --- end smbb ---\n", "", t)
        t = add_token(t, "source", "smbb-disable-btc.c")
        m = re.search(r"(?m)^prepare\(\)\s*\{\s*\n", t)
        close = re.search(r"(?m)^\}\s*$", t[m.end():])
        pos = m.end() + close.start()
        t = t[:pos] + SMBB_SNIPPET + t[pos:]
        changed = True

    # FTS range split for the CPU rail (added later; independent marker)
    if FTS_MARK not in t:
        t = re.sub(r"(?ms)^\t# --- ftsmps range split.*?# --- end ftsmps ---\n", "", t)
        m = re.search(r"(?m)^prepare\(\)\s*\{\s*\n", t)
        close = re.search(r"(?m)^\}\s*$", t[m.end():])
        pos = m.end() + close.start()
        t = t[:pos] + FTS_SNIPPET + t[pos:]
        changed = True

    # rebuild whenever the bass DTS content changes
    import hashlib
    rev = hashlib.sha1(Path(dts).read_bytes() + SMBB_C.encode() + FTS_SNIPPET.encode()).hexdigest()[:12]
    m = re.search(r"(?m)^# smartlinux-rev: (\w+)$", t)
    if not m or m.group(1) != rev:
        t = re.sub(r"(?m)^# smartlinux-rev: \w+\n", "", t)
        t = t.rstrip("\n") + f"\n# smartlinux-rev: {rev}\n"
        changed = True

    if changed:
        t = bump_pkgrel(t)
        apk.write_text(t)
        print(f"kernel package patched (pkgrel bumped): {d}")
    else:
        print(f"kernel package already patched: {d}")
    update_local_sums(d)
    print("kernel checksums updated for local files (no download needed)")


def update_local_sums(d):
    """Refresh sha512sums for files that live next to the APKBUILD.

    Remote sources (the kernel tarball) keep their existing checksum, so
    nothing has to be downloaded (avoids GitHub rate limits)."""
    import hashlib
    d = Path(d)
    apk = d / "APKBUILD"
    t = apk.read_text()
    blk = find_var_block(t, "source")
    names = []
    if blk:
        for tok in t[blk[0]:blk[1]].split():
            name = tok.split("::")[0] if "::" in tok else tok
            if "://" in tok:
                continue
            names.append(name)
    m = re.search(r'^sha512sums="', t, re.M)
    if not m:
        die("APKBUILD has no sha512sums")
    s0 = m.end()
    e0 = t.index('"', s0)
    lines = [l for l in t[s0:e0].splitlines() if l.strip()]
    sums = {}
    order = []
    for l in lines:
        h, _, f = l.strip().partition("  ")
        sums[f] = h
        order.append(f)
    for tok in names:
        # resolve shell variables like $_config the simple way: match by suffix
        cands = [p for p in d.iterdir() if p.name == tok or (tok.startswith("$") and p.name in sums)]
        for p in cands:
            if p.name == "APKBUILD":
                continue
            sums[p.name] = hashlib.sha512(p.read_bytes()).hexdigest()
            if p.name not in order:
                order.append(p.name)
    body = "\n" + "".join(f"{sums[f]}  {f}\n" for f in order)
    t = t[:s0] + body + t[e0:]
    apk.write_text(t)


# ---------------------------------------------------------------- offsets
def cmd_offsets(path):
    data = Path(path).read_bytes()[:64]
    if data[:8] != b"ANDROID!":
        die(f"{path} is not an Android boot image")
    (ksz, kaddr, rsz, raddr, ssz, saddr, taddr, page) = struct.unpack("<8I", data[8:40])
    out = {"base": 0, "kernel": kaddr, "ramdisk": raddr, "second": saddr,
           "tags": taddr, "pagesize": page}
    # sanity: LK on msm8226 loads below 256 MiB of RAM start
    if not (0 < kaddr < 0x10000000 and 0 < raddr < 0x10000000 and page in (2048, 4096)):
        die(f"implausible header values: {out}")
    print(json.dumps(out))


def main(argv):
    if len(argv) < 2:
        die(__doc__)
    cmd = argv[1]
    if cmd == "device":
        offs = None
        if "--offsets" in argv:
            offs = argv[argv.index("--offsets") + 1]
        cmd_device(argv[2], offs)
    elif cmd == "kernel":
        cmd_kernel(argv[2], argv[3])
    elif cmd == "offsets":
        cmd_offsets(argv[2])
    else:
        die(__doc__)


if __name__ == "__main__":
    main(sys.argv)
