// SPDX-License-Identifier: MIT
/*
 * watch-bootlogo - keep the bootloader's logo turned the same way as the screen.
 *
 *   watch-bootlogo                status: which picture is the Mini Linux logo, its angle
 *   watch-bootlogo apply          turn it to the screen's ROTATE setting (watch set ROTATE)
 *   watch-bootlogo rotate ANGLE   turn it to ANGLE degrees clockwise (0 90 180 270)
 *   watch-bootlogo mark NAME      remember NAME as the Mini Linux logo, upright (step 08)
 *   watch-bootlogo forget         the logo is LG's again (08 --restore): never touch it
 *
 * The bootloader (LK) draws its pictures from the "imgdata" partition (IMGDATA!
 * format: 40-byte entries, pixels as 4-byte runs count,R,G,B, pictures 512-byte
 * aligned from 0x400). Only a picture marked by step 08 is changed; the state is in
 * /var/lib/urbane/bootlogo ("NAME ANGLE"). The whole partition is rebuilt in memory,
 * checked, written and read back. Needs root.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <unistd.h>

#define STATE "/var/lib/urbane/bootlogo"
#define SCREEN 320
#define HDR 0x400
#define MAXE 64

struct ent { char name[17]; uint32_t w, h, x, y, off, size; };

static void die(const char *m) { fprintf(stderr, "watch-bootlogo: %s\n", m); exit(1); }

static uint32_t rd32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static int find_part(char *dev, size_t n)
{
	char path[256], line[128];
	DIR *d;
	struct dirent *e;
	ssize_t l = readlink("/dev/disk/by-partlabel/imgdata", path, sizeof(path) - 1);

	if (l > 0) {
		path[l] = 0;
		snprintf(dev, n, "/dev/%s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
		return 0;
	}
	if (!(d = opendir("/sys/class/block")))
		return -1;
	while ((e = readdir(d))) {
		FILE *f;
		int hit = 0;
		snprintf(path, sizeof(path), "/sys/class/block/%s/uevent", e->d_name);
		if (!(f = fopen(path, "r"))) continue;
		while (fgets(line, sizeof(line), f))
			if (!strcmp(line, "PARTNAME=imgdata\n")) hit = 1;
		fclose(f);
		if (hit) { snprintf(dev, n, "/dev/%s", e->d_name); closedir(d); return 0; }
	}
	closedir(d);
	return -1;
}

static int parse(const uint8_t *b, size_t len, struct ent *e, uint32_t *n)
{
	if (len < HDR || memcmp(b, "IMGDATA!", 8)) return -1;
	*n = rd32(b + 12);
	if (*n == 0 || *n > MAXE || 0x18 + 40 * *n > HDR) return -1;
	for (uint32_t i = 0; i < *n; i++) {
		const uint8_t *p = b + 0x18 + 40 * i;
		memcpy(e[i].name, p, 16); e[i].name[16] = 0;
		e[i].w = rd32(p + 16); e[i].h = rd32(p + 20); e[i].x = rd32(p + 24);
		e[i].y = rd32(p + 28); e[i].off = rd32(p + 32); e[i].size = rd32(p + 36);
		if (e[i].off < HDR || e[i].off + (uint64_t)e[i].size > len || e[i].w > 4096 || e[i].h > 4096)
			return -1;
	}
	return 0;
}

static uint8_t *decode(const uint8_t *b, const struct ent *e)
{
	size_t need = (size_t)e->w * e->h, k = 0;
	uint8_t *px = calloc(need ? need : 1, 3);
	if (!px) die("out of memory");
	for (uint32_t i = 0; i + 4 <= e->size && k < need; i += 4) {
		const uint8_t *r = b + e->off + i;
		for (int c = 0; c < r[0] && k < need; c++, k++)
			memcpy(px + 3 * k, r + 1, 3);
	}
	return px;
}

static uint8_t *encode(const uint8_t *px, size_t n, uint32_t *out_len)
{
	uint8_t *o = malloc(n * 4 + 4);
	size_t i = 0, l = 0;
	if (!o) die("out of memory");
	while (i < n) {
		size_t c = 1;
		while (i + c < n && c < 255 && !memcmp(px + 3 * i, px + 3 * (i + c), 3)) c++;
		o[l++] = (uint8_t)c;
		memcpy(o + l, px + 3 * i, 3);
		l += 3;
		i += c;
	}
	*out_len = (uint32_t)l;
	return o;
}

/* turn w x h pixels clockwise by 90 * q degrees */
static uint8_t *turn(const uint8_t *px, uint32_t w, uint32_t h, int q, uint32_t *nw, uint32_t *nh)
{
	uint8_t *o = malloc((size_t)w * h * 3 + 1);
	if (!o) die("out of memory");
	*nw = q % 2 ? h : w; *nh = q % 2 ? w : h;
	for (uint32_t y = 0; y < h; y++)
		for (uint32_t x = 0; x < w; x++) {
			uint32_t X, Y;
			switch (q) {
			case 1: X = h - 1 - y; Y = x; break;          /* 90 cw */
			case 2: X = w - 1 - x; Y = h - 1 - y; break;  /* 180 */
			case 3: X = y; Y = w - 1 - x; break;          /* 270 cw */
			default: X = x; Y = y;
			}
			memcpy(o + 3 * ((size_t)Y * *nw + X), px + 3 * ((size_t)y * w + x), 3);
		}
	return o;
}

static int read_state(char *name, int *ang)
{
	FILE *f = fopen(STATE, "r");
	int ok;
	if (!f) return -1;
	ok = fscanf(f, "%16s %d", name, ang) == 2;
	fclose(f);
	return ok ? 0 : -1;
}

static void write_state(const char *name, int ang)
{
	FILE *f;
	mkdir("/var/lib/urbane", 0755);
	if (!(f = fopen(STATE, "w"))) die("cannot write " STATE);
	fprintf(f, "%s %d\n", name, ang);
	fclose(f);
}

static int conf_rotate(void)
{
	/* ROTATE from the system settings (package defaults, then the local file) */
	const char *files[] = { "/etc/urbane/watch.conf", "/etc/urbane/watch.local.conf" };
	int r = 0;
	for (int i = 0; i < 2; i++) {
		FILE *f = fopen(files[i], "r");
		char line[128];
		if (!f) continue;
		while (fgets(line, sizeof(line), f)) {
			int v;
			if (sscanf(line, "ROTATE=\"%d\"", &v) == 1 || sscanf(line, "ROTATE=%d", &v) == 1)
				r = v;
		}
		fclose(f);
	}
	return r;
}

int main(int argc, char **argv)
{
	const char *cmd = argc > 1 ? argv[1] : "status";
	char dev[300], name[17] = "";
	int fd, ang = 0, target, q;
	uint64_t len = 0;
	uint8_t *b, *nb;
	struct ent e[MAXE];
	uint32_t n, t = MAXE;

	if (!strcmp(cmd, "-h") || !strcmp(cmd, "--help") || !strcmp(cmd, "help")) {
		puts("usage: watch-bootlogo [status|apply|rotate ANGLE|mark NAME|forget]");
		return 0;
	}
	if (!strcmp(cmd, "forget")) {
		if (unlink(STATE) && errno != ENOENT) die("cannot remove " STATE);
		puts("boot logo: LG's own, left alone");
		return 0;
	}
	if (!strcmp(cmd, "mark")) {
		if (argc < 3 || strlen(argv[2]) > 16) die("mark NAME");
		write_state(argv[2], 0);
		printf("boot logo: picture '%s' is the Mini Linux logo, upright\n", argv[2]);
		return 0;
	}
	if (read_state(name, &ang)) {
		puts("boot logo: not installed by Mini Linux (build/08-boot-logo.sh) - nothing to turn");
		return 0;
	}
	if (!strcmp(cmd, "status")) {
		printf("boot logo: picture '%s', turned %d degrees; screen ROTATE=%d\n", name, ang, conf_rotate());
		return 0;
	}
	if (!strcmp(cmd, "apply")) target = conf_rotate();
	else if (!strcmp(cmd, "rotate") && argc > 2) target = atoi(argv[2]);
	else die("unknown command (try --help)");
	if (target % 90 || target < 0 || target > 270) die("angle is 0, 90, 180 or 270");
	q = ((target - ang) / 90 + 4) % 4;
	if (q == 0) { printf("boot logo already at %d degrees\n", target); return 0; }

	if (geteuid()) die("needs root");
	if (getenv("WATCH_BOOTLOGO_DEV"))           /* tests: an image file instead */
		snprintf(dev, sizeof(dev), "%s", getenv("WATCH_BOOTLOGO_DEV"));
	else if (find_part(dev, sizeof(dev))) die("no imgdata partition");
	if ((fd = open(dev, O_RDWR | O_SYNC)) < 0) die("cannot open the imgdata partition");
	{
		struct stat st;
		if (fstat(fd, &st)) die("stat failed");
		if (S_ISREG(st.st_mode)) len = (uint64_t)st.st_size;
		else if (ioctl(fd, BLKGETSIZE64, &len)) die("cannot get the partition size");
	}
	if (len < HDR || len > (64u << 20)) die("odd partition size");
	b = malloc(len); nb = calloc(1, len);
	if (!b || !nb) die("out of memory");
	if (pread(fd, b, len, 0) != (ssize_t)len) die("read failed");
	if (parse(b, len, e, &n)) die("partition is not in IMGDATA! format - not touching it");
	for (uint32_t i = 0; i < n; i++)
		if (!strncmp(e[i].name, name, 16)) t = i;
	if (t == MAXE) die("the marked picture is not in the partition any more");

	/* the marked picture, turned */
	{
		uint8_t *px = decode(b, &e[t]), *tp, *enc;
		uint32_t nw, nh, elen, off = HDR;
		uint32_t ox = e[t].x, oy = e[t].y, w = e[t].w, h = e[t].h;
		tp = turn(px, w, h, q, &nw, &nh);
		enc = encode(tp, (size_t)nw * nh, &elen);
		/* new box: the old one turned around the screen centre */
		switch (q) {
		case 1: e[t].x = SCREEN - (oy + h); e[t].y = ox; break;
		case 2: e[t].x = SCREEN - (ox + w); e[t].y = SCREEN - (oy + h); break;
		case 3: e[t].x = oy; e[t].y = SCREEN - (ox + w); break;
		}
		if ((int32_t)e[t].x < 0 || (int32_t)e[t].y < 0) { e[t].x = 0; e[t].y = 0; }
		e[t].w = nw; e[t].h = nh;

		/* rebuild: same header, pictures packed again from 0x400, 512-byte aligned */
		memcpy(nb, b, HDR);
		for (uint32_t i = 0; i < n; i++) {
			const uint8_t *src = i == t ? enc : b + e[i].off;
			uint32_t sz = i == t ? elen : e[i].size;
			uint8_t *p = nb + 0x18 + 40 * i;
			if (off + (uint64_t)sz > len) die("turned logo does not fit in the partition - not written");
			memcpy(nb + off, src, sz);
			wr32(p + 16, e[i].w); wr32(p + 20, e[i].h); wr32(p + 24, e[i].x);
			wr32(p + 28, e[i].y); wr32(p + 32, off); wr32(p + 36, sz);
			e[i].off = off; e[i].size = sz;
			off += (sz + 511) & ~511u;
		}
		/* check what is about to be written: parses, and the logo decodes to tp */
		{
			struct ent c[MAXE];
			uint32_t cn;
			uint8_t *back;
			if (parse(nb, len, c, &cn) || cn != n) die("self-check failed (header) - not written");
			back = decode(nb, &c[t]);
			if (memcmp(back, tp, (size_t)nw * nh * 3)) die("self-check failed (pixels) - not written");
			free(back);
		}
		free(px); free(tp); free(enc);
	}
	if (pwrite(fd, nb, len, 0) != (ssize_t)len || fsync(fd)) die("WRITE FAILED - restore with build/08-boot-logo.sh");
	if (pread(fd, b, len, 0) != (ssize_t)len || memcmp(b, nb, len)) die("READ-BACK DIFFERS - restore with build/08-boot-logo.sh");
	close(fd);
	write_state(name, target);
	printf("boot logo turned to %d degrees (shows at the next start)\n", target);
	return 0;
}
