// SPDX-License-Identifier: MIT
/*
 * watch-panel - the panels of the round watch frame (watch-frame).
 *
 *   watch-panel mem    left:   RAM / swap / disk bars + numbers, free RAM
 *   watch-panel cpu    right:  one bar per CPU core + numbers, temperature, clock
 *   watch-panel nav    bottom: buttons; page "main" (menu, windows, keys, Ctrl-C)
 *                      and page "keys" (Esc Tab PgUp PgDn ^C, arrows, Enter) that
 *                      types into the watch screen like a keyboard (also in menus)
 *
 * Runs inside a pane of the outer "frame" tmux. It asks that tmux where its pane
 * sits on the 320x320 panel and fits everything into the visible circle.
 * Clicks/taps arrive as SGR mouse reports. Redraws every 2 s.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/statvfs.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAXCPU 8
#define SCREEN 320.0          /* panel pixels */
#define RADIUS 161.0          /* visible glass radius in pixels */
#define FRAME "tmux -L frame "

static const char *blk[9] = { " ", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█" };
static int W = 4, H = 10;
static volatile sig_atomic_t resized = 1;
static struct termios saved;
static char mode[8], self[16] = "", center[16] = "";

/* pane position on the panel (cells) and cell size (pixels) */
static int cl_w = 39, cl_h = 20, pn_x = 0, pn_y = 0;
static double cw = 8, ch = 16, ox = 4, oy = 4;

/* ------------------------------------------------------------------ helpers */

static void on_winch(int s) { (void)s; resized = 1; }

static void restore(void)
{
	printf("\033[?1006l\033[?1000l\033[?25h\033[0m");
	fflush(stdout);
	tcsetattr(0, TCSANOW, &saved);
}

static void on_term(int s) { (void)s; restore(); _exit(0); }

static long read_long(const char *path)
{
	FILE *f = fopen(path, "r");
	long v = -1;
	if (f) {
		if (fscanf(f, "%ld", &v) != 1)
			v = -1;
		fclose(f);
	}
	return v;
}

static int read_cmd(const char *cmd, char *out, size_t n)
{
	FILE *p = popen(cmd, "r");
	int ok = 0;
	out[0] = 0;
	if (p) {
		if (fgets(out, (int)n, p)) {
			out[strcspn(out, "\n")] = 0;
			ok = 1;
		}
		pclose(p);
	}
	return ok;
}

static void geometry(void)
{
	struct winsize ws;
	char cmd[160], out[96];
	int wh = 0;

	if (ioctl(1, TIOCGWINSZ, &ws) == 0 && ws.ws_col && ws.ws_row) {
		W = ws.ws_col;
		H = ws.ws_row;
	}
	snprintf(cmd, sizeof(cmd), FRAME "display -p -t '%s' "
		 "'#{client_width} #{client_height} #{window_height} #{pane_left} #{pane_top}' 2>/dev/null",
		 self[0] ? self : "");
	if (read_cmd(cmd, out, sizeof(out))) {
		int a, b, c, d, e;
		if (sscanf(out, "%d %d %d %d %d", &a, &b, &c, &d, &e) == 5 && a > 0 && b > 0) {
			cl_w = a; cl_h = b; wh = c; pn_x = d;
			pn_y = (b - c) + e;          /* status lines are on top */
		}
	}
	(void)wh;
	/* foot: 4 px padding, grid centred (watch-viewport "round") */
	cw = (SCREEN - 8) / cl_w;
	ch = (SCREEN - 8) / cl_h;
	if (cw > 7 && cw < 12) cw = floor(cw);
	if (ch > 12 && ch < 24) ch = floor(ch);
	ox = (SCREEN - cw * cl_w) / 2;
	oy = (SCREEN - ch * cl_h) / 2;
	read_cmd(FRAME "show -gv @center 2>/dev/null", center, sizeof(center));
}

/* visible columns [*c0, *c1] of pane line `line` (pane coordinates); 0 = none */
static int span(int line, int *c0, int *c1)
{
	double y0 = oy + (pn_y + line) * ch - SCREEN / 2, y1 = y0 + ch, dy, half;
	int a, b;

	dy = fabs(y0) > fabs(y1) ? fabs(y0) : fabs(y1);
	if (dy >= RADIUS)
		return 0;
	half = sqrt(RADIUS * RADIUS - dy * dy);
	a = (int)ceil((SCREEN / 2 - half - ox) / cw) - pn_x;
	b = (int)floor((SCREEN / 2 + half - ox) / cw) - 1 - pn_x;
	if (a < 0) a = 0;
	if (b > W - 1) b = W - 1;
	if (a > b)
		return 0;
	*c0 = a;
	*c1 = b;
	return 1;
}

static int ulen(const char *s)
{
	int n = 0;
	for (; *s; s++)
		if ((*s & 0xc0) != 0x80) n++;
	return n;
}

static void at(int row, int col) { printf("\033[%d;%dH", row + 1, col + 1); }

static int run(const char *cmd)
{
	pid_t p = fork();
	if (p == 0) {
		int fd;
		setsid();
		fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	return p > 0 ? 0 : -1;
}

/* ------------------------------------------------------------------ side panels */

/* fill colours by load, two shades so neighbouring bars read as separate columns */
static int fillc(int pct, int odd)
{
	if (pct >= 85) return odd ? 167 : 203;
	if (pct >= 60) return odd ? 179 : 221;
	return odd ? 71 : 114;
}

/*
 * Vertical bar in column col, rows top..top+h-1, filled from the bottom. The
 * value (percent, max 99) is written top-down in the lowest rows of the bar:
 * dark on the fill colour where the bar is filled, light on the track above it.
 */
static void vbar(int col, int top, int h, int pct, int odd)
{
	int eighths = pct * h * 8 / 100, fc, tc = odd ? 239 : 235, ll;
	char lab[4];

	if (pct < 0) pct = 0;
	if (pct > 99) pct = 99;
	if (pct > 0 && eighths == 0)
		eighths = 1;
	fc = fillc(pct, odd);
	ll = snprintf(lab, sizeof(lab), "%d", pct);
	if (ll > h) ll = 0;
	/* odd bars carry their number two rows higher, so neighbours never read
	 * as one number ("1354") */
	{
		int lift = odd && h >= ll + 4 ? 2 : 0;
		for (int r = 0; r < h; r++) {
		int e = eighths - (h - 1 - r) * 8, li = r - (h - ll - lift);
		if (li >= ll) li = -1;
		e = e < 0 ? 0 : e > 8 ? 8 : e;
		at(top + r, col);
		if (li >= 0) {
			if (e >= 5)
				printf("\033[1;38;5;232;48;5;%dm%c", fc, lab[li]);
			else
				printf("\033[1;38;5;252;48;5;%dm%c", tc, lab[li]);
		} else if (e) {
			printf("\033[38;5;%d;48;5;%dm%s", fc, tc, blk[e]);
		} else {
			printf("\033[38;5;%dm█", tc);
		}
		printf("\033[0m");
		}
	}
}

/*
 * Layout of a side panel, hugging the middle (right-aligned on the left panel,
 * left-aligned on the right one) and kept inside the glass:
 *     head1        free RAM MB / temperature
 *     head2        "MB" / MHz
 *     bars  (h)    percent written inside each bar
 *     labels       RSD / 0123
 *     info         1-2 lines: disk used/total MB / uptime
 * Each line needs some width; the block goes where all of them fit, as close to
 * the panel's middle as possible. The info text tries its long form first, then
 * shorter ones, then lower bars, then goes away.
 */
static int visw[256];

struct info { const char *line[2]; int n; };

static int fits(int top, int h, int n, int w1, int w2, const struct info *in)
{
	int l = top, need = 3 + h + (in ? in->n : 0);
	if (top < 0 || top + need > H) return 0;
	if (visw[l++] < w1 || visw[l++] < w2) return 0;
	for (int i = 0; i < h + 1; i++)
		if (visw[l++] < n) return 0;
	for (int i = 0; in && i < in->n; i++)
		if (visw[l++] < ulen(in->line[i])) return 0;
	return 1;
}

static void side(const char *head1, const char *h1style, const char *head2,
		 int n, const char *names, const int *pct, const int *on,
		 const struct info *infos, const char *const *istyle, const char *const *stext,
		 const char *sunit)
{
	int left = !strcmp(mode, "mem"), w1 = ulen(head1), w2 = ulen(head2);
	int h = 2, top = -1, mid, x0;
	const struct info *in = NULL;

	for (int l = 0; l < H && l < 256; l++) {
		int c0, c1;
		visw[l] = 0;
		if (span(l, &c0, &c1))
			visw[l] = left ? (c1 == W - 1 ? W - c0 : 0) : (c0 == 0 ? c1 + 1 : 0);
	}
	mid = (int)((SCREEN / 2 - oy) / ch) - pn_y;
	/* tallest bars first; at each height the longest info text that fits; bars of
	 * at least 5 rows win over the text */
	for (int pass = 0; pass < 2 && top < 0; pass++)
		for (h = 10; h >= (pass ? 2 : 5) && top < 0; h--)
			for (int k = 0; top < 0 && (pass ? k < 1 : infos && infos[k].n); k++) {
				const struct info *try = pass ? NULL : &infos[k];
				int need = 3 + h + (try ? try->n : 0);
				for (int d = 0; d < H && top < 0; d++) {
					if (fits(mid - need / 2 - d, h, n, w1, w2, try)) top = mid - need / 2 - d;
					else if (fits(mid - need / 2 + d, h, n, w1, w2, try)) top = mid - need / 2 + d;
				}
				if (top >= 0) in = try;
			}
	h++;                         /* undo the loop's last step */
	if (top < 0) { top = 0; h = H - 3 > 2 ? (H - 3 > 10 ? 10 : H - 3) : 2; in = NULL; }

	printf("\033[2J");
	at(top, left ? W - w1 : 0);
	printf("%s%s\033[0m", h1style, head1);
	at(top + 1, left ? W - w2 : 0);
	printf("\033[38;5;244m%s\033[0m", head2);
	x0 = left ? W - n : 0;
	if (x0 < 0) x0 = 0;
	for (int i = 0; i < n && x0 + i < W; i++) {
		if (on[i])
			vbar(x0 + i, top + 2, h, pct[i], i & 1);
		else
			for (int r = 0; r < h; r++) { at(top + 2 + r, x0 + i); printf("\033[38;5;%dm·\033[0m", i & 1 ? 240 : 238); }
	}
	at(top + 2 + h, x0);
	printf("\033[38;5;244m%.*s\033[0m", n, names);
	for (int i = 0; in && i < in->n; i++) {
		int w = ulen(in->line[i]);
		at(top + 3 + h + i, left ? W - w : 0);
		printf("%s%s\033[0m", istyle[i], in->line[i]);
	}
	(void)stext; (void)sunit;    /* disk + uptime moved to the top bar (watch-status --info) */
}

static void draw_mem(void)
{
	long total = 0, avail = 0, stot = 0, sfree = 0, v;
	char key[64], h1[24], t1[48] = "", t2[24] = "", t3[24] = "";
	FILE *f = fopen("/proc/meminfo", "r");
	struct statvfs st;
	int pct[3] = { 0, 0, 0 }, on[3] = { 1, 0, 1 };

	if (f) {
		while (fscanf(f, "%63s %ld kB", key, &v) == 2) {
			if (!strcmp(key, "MemTotal:")) total = v;
			else if (!strcmp(key, "MemAvailable:")) avail = v;
			else if (!strcmp(key, "SwapTotal:")) stot = v;
			else if (!strcmp(key, "SwapFree:")) sfree = v;
		}
		fclose(f);
	}
	if (total) pct[0] = (int)(100 - avail * 100 / total);
	if (stot) { pct[1] = (int)(100 - sfree * 100 / stot); on[1] = 1; }
	if (statvfs("/", &st) == 0 && st.f_blocks) {
		unsigned long long bs = st.f_frsize ? st.f_frsize : st.f_bsize;
		unsigned long long tot = st.f_blocks * bs >> 20, used = (st.f_blocks - st.f_bfree) * bs >> 20;
		pct[2] = (int)(100 - st.f_bavail * 100 / st.f_blocks);
		/* disk used / total, in MB: one line if it fits, else two */
		snprintf(t1, sizeof(t1), "%llu/%llu", used, tot);
		snprintf(t2, sizeof(t2), "%llu", used);
		snprintf(t3, sizeof(t3), "/%llu", tot);
	}
	if (avail / 1024 >= 10000) snprintf(h1, sizeof(h1), "%ldG", avail / 1048576);
	else snprintf(h1, sizeof(h1), "%ld", avail / 1024);
	{
		struct info infos[] = { { { t1, NULL }, 1 }, { { t2, t3 }, 2 }, { { t2, NULL }, 1 },
					{ { NULL, NULL }, 0 } };
		const char *st[2] = { "\033[38;5;252m", "\033[38;5;245m" };
		const char *stext[3] = { t1, t2, NULL };
		(void)infos;
		side(h1, "\033[1;37m", "MB", 3, "RSD", pct, on, NULL, st, t1[0] ? stext : NULL, "MB");
	}
}

static unsigned long long prev_busy[MAXCPU], prev_all[MAXCPU];

static void draw_cpu(void)
{
	int pct[MAXCPU] = { 0 }, on[MAXCPU] = { 0 }, temp = -1;
	char line[256], h1[24], h2[24];
	FILE *f = fopen("/proc/stat", "r");
	DIR *d;
	struct dirent *e;
	long khz;

	if (f) {
		while (fgets(line, sizeof(line), f)) {
			int n;
			unsigned long long u, ni, s, id, io, irq, sirq, st = 0, busy, all;
			if (strncmp(line, "cpu", 3) || line[3] < '0' || line[3] > '9')
				continue;
			if (sscanf(line + 3, "%d %llu %llu %llu %llu %llu %llu %llu %llu",
				   &n, &u, &ni, &s, &id, &io, &irq, &sirq, &st) < 8 || n < 0 || n >= MAXCPU)
				continue;
			busy = u + ni + s + irq + sirq + st;
			all = busy + id + io;
			if (all > prev_all[n])
				pct[n] = (int)((busy - prev_busy[n]) * 100 / (all - prev_all[n]));
			prev_busy[n] = busy;
			prev_all[n] = all;
			on[n] = 1;
		}
		fclose(f);
	}
	d = opendir("/sys/class/thermal");
	if (d) {
		while ((e = readdir(d))) {
			char p[300], type[64] = "";
			FILE *t;
			long v;
			if (strncmp(e->d_name, "thermal_zone", 12)) continue;
			snprintf(p, sizeof(p), "/sys/class/thermal/%s/type", e->d_name);
			t = fopen(p, "r");
			if (t) { if (!fgets(type, sizeof(type), t)) type[0] = 0; fclose(t); }
			if (!strstr(type, "cpu")) continue;
			snprintf(p, sizeof(p), "/sys/class/thermal/%s/temp", e->d_name);
			v = read_long(p);
			if (v / 1000 > temp) temp = (int)(v / 1000);
		}
		closedir(d);
	}
	khz = read_long("/sys/devices/system/cpu/cpu0/cpufreq/scaling_cur_freq");
	if (temp >= 0) snprintf(h1, sizeof(h1), "%d°", temp);
	else snprintf(h1, sizeof(h1), "--");
	if (khz > 0) snprintf(h2, sizeof(h2), "%ld", khz / 1000);
	else snprintf(h2, sizeof(h2), "MHz");
	{
		/* time since boot, sideways next to the bars */
		double up = 0;
		long m;
		char t1[40], t2[32];
		FILE *u = fopen("/proc/uptime", "r");
		if (u) { if (fscanf(u, "%lf", &up) != 1) up = 0; fclose(u); }
		m = (long)up / 60;
		if (m >= 1440) snprintf(t2, sizeof(t2), "%ldd%02ldh", m / 1440, m / 60 % 24);
		else snprintf(t2, sizeof(t2), "%ldh%02ldm", m / 60, m % 60);
		snprintf(t1, sizeof(t1), "up %s", t2);
		{
			struct info infos[] = { { { t1, NULL }, 1 }, { { "up", t2 }, 2 }, { { NULL, NULL }, 0 } };
			const char *st1[2] = { "\033[38;5;245m", "\033[38;5;252m" };
			const char *stext[3] = { t1, t2, NULL };
			(void)infos;
			side(h1, temp >= 70 ? "\033[1;38;5;203m" : temp >= 60 ? "\033[1;38;5;221m" : "\033[1;37m",
			     h2, 4, "0123", pct, on, NULL, st1, stext, NULL);
		}
	}
}

/* ------------------------------------------------------------------ nav */

/*
 * Buttons. A button runs a shell command (inner tmux), types keys into the middle
 * pane (sent to the outer tmux, so the watch's own tmux sees them like a real
 * keyboard - menus too), switches page (@keys @pad @main), or is a phone-pad key.
 *
 * Look: flat dark button with cut corners, drawn with block elements that foot
 * renders itself (pixel exact, no font needed):
 *     ▟ label ▙       label line, background = button colour
 *     ▝▀▀▀▀▀▀▀▘       lower half line = rounded bottom + half-line gap
 */
struct button {
	const char *label, *fg, *cmd, *keys;
	int row;                 /* button row */
	const char *shortl;      /* label when the button is narrow (NULL = same) */
	const char *pad;         /* phone-pad letters (E.161 multi-tap) */
	int x0, x1, y0, y1;      /* filled in by the layout */
};

#define FG_W  "\033[1;38;5;255m"
#define FG_AC "\033[1;38;5;80m"
#define FG_BL "\033[1;38;5;117m"
#define FG_YE "\033[1;38;5;221m"
#define FG_GR "\033[1;38;5;114m"
#define FG_DI "\033[38;5;250m"

static struct button main_page[] = {
	{ "≡",    FG_AC, "watch-menu", NULL, 0, NULL, NULL, 0, 0, 0, 0 },
	{ "◀",    FG_W,  "tmux previous-window", NULL, 0, NULL, NULL, 0, 0, 0, 0 },
	{ "▶",    FG_W,  "tmux next-window", NULL, 0, NULL, NULL, 0, 0, 0, 0 },
	{ "Keys", FG_BL, "@keys", NULL, 0, "K", NULL, 0, 0, 0, 0 },
	{ "^C",   FG_YE, NULL, "C-c", 0, NULL, NULL, 0, 0, 0, 0 },
};
static struct button keys_page[] = {
	{ "Esc",  FG_W,  NULL, "Escape", 0, "E", NULL, 0, 0, 0, 0 },
	{ "Tab",  FG_W,  NULL, "Tab", 0, "⇥", NULL, 0, 0, 0, 0 },
	{ "Pg↑",  FG_GR, "tmux copy-mode -eu", NULL, 0, "⇞", NULL, 0, 0, 0, 0 },
	{ "Pg↓",  FG_GR, "tmux send-keys -X page-down", NULL, 0, "⇟", NULL, 0, 0, 0, 0 },
	{ "abc",  FG_BL, "@pad", NULL, 0, "#", NULL, 0, 0, 0, 0 },
	{ "Back", FG_BL, "@main", NULL, 1, "◁", NULL, 0, 0, 0, 0 },
	{ "↑",    FG_W,  NULL, "Up", 1, NULL, NULL, 0, 0, 0, 0 },
	{ "⏎",    FG_AC, NULL, "Enter", 1, NULL, NULL, 0, 0, 0, 0 },
	{ "←",    FG_W,  NULL, "Left", 2, NULL, NULL, 0, 0, 0, 0 },
	{ "↓",    FG_W,  NULL, "Down", 2, NULL, NULL, 0, 0, 0, 0 },
	{ "→",    FG_W,  NULL, "Right", 2, NULL, NULL, 0, 0, 0, 0 },
};
/* ITU-T E.161 phone keypad, multi-tap: tap a key again within 1 s for the next letter */
static struct button pad_page[] = {
	{ "1 .,?", FG_W, NULL, NULL, 0, "1", ".,?!'-1", 0, 0, 0, 0 },
	{ "2 abc", FG_W, NULL, NULL, 0, "2", "abc2", 0, 0, 0, 0 },
	{ "3 def", FG_W, NULL, NULL, 0, "3", "def3", 0, 0, 0, 0 },
	{ "4 ghi", FG_W, NULL, NULL, 1, "4", "ghi4", 0, 0, 0, 0 },
	{ "5 jkl", FG_W, NULL, NULL, 1, "5", "jkl5", 0, 0, 0, 0 },
	{ "6 mno", FG_W, NULL, NULL, 1, "6", "mno6", 0, 0, 0, 0 },
	{ "7 pqrs", FG_W, NULL, NULL, 2, "7", "pqrs7", 0, 0, 0, 0 },
	{ "8 tuv", FG_W, NULL, NULL, 2, "8", "tuv8", 0, 0, 0, 0 },
	{ "9 wxyz", FG_W, NULL, NULL, 2, "9", "wxyz9", 0, 0, 0, 0 },
	{ "* abc", FG_BL, "@case", NULL, 3, "*", NULL, 0, 0, 0, 0 },
	{ "0 ␣",   FG_W, NULL, NULL, 3, "0", " 0", 0, 0, 0, 0 },
	{ "# ⌫",   FG_YE, NULL, "BSpace", 3, "⌫", NULL, 0, 0, 0, 0 },
	{ "Back",  FG_BL, "@keys", NULL, 4, "◁", NULL, 0, 0, 0, 0 },
	{ "⏎",     FG_AC, NULL, "Enter", 4, NULL, NULL, 0, 0, 0, 0 },
};
#define N(a) ((int)(sizeof(a) / sizeof((a)[0])))
static struct button *page = main_page;
static int npage = N(main_page), pressed = -1, pressfade = 0, pagenum = 0;

/* multi-tap state */
static int casemode = 0;               /* 0 abc, 1 Abc (one capital), 2 ABC, 3 123 */
static const char *caselab[4] = { "* abc", "* Abc", "* ABC", "* 123" };
static int pend = -1, pendidx = 0;     /* key being cycled, letter index */
static long long pendt = 0;            /* when it was tapped (ms) */
#define MULTITAP_MS 1000

static long long now_ms(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (long long)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

/* place the buttons of row(s) [ra, rb] (same columns for all of them) on lines y.. */
static void place(int ra, int rb, int y, int lh)
{
	int a = 0, b = W - 1, n = 0, gap = 1, bw, used, start;

	for (int l = y; l < y + (rb - ra + 1) * lh; l++) {
		int c0, c1;
		if (!span(l, &c0, &c1)) { c0 = 0; c1 = -1; }
		if (c0 > a) a = c0;
		if (c1 < b) b = c1;
	}
	for (int i = 0; i < npage; i++)
		if (page[i].row == ra) n++;
	if (b < a || n == 0) { a = 0; b = W - 1; }
	bw = ((b - a + 1) - (n - 1) * gap) / n;
	if (bw < 3) { gap = 0; bw = (b - a + 1) / n; }
	if (bw > 11) bw = 11;
	if (bw < 1) bw = 1;
	used = n * bw + (n - 1) * gap;
	start = a + ((b - a + 1) - used) / 2;
	for (int r = ra; r <= rb; r++) {
		int k = 0;
		for (int i = 0; i < npage; i++) {
			if (page[i].row != r) continue;
			page[i].x0 = start + k * (bw + gap);
			page[i].x1 = page[i].x0 + bw - 1;
			page[i].y0 = y + (r - ra) * lh;
			page[i].y1 = page[i].y0 + lh - 1;
			k++;
		}
	}
}

static void draw_button(struct button *b, int i)
{
	const char *lab = b->label;
	int bw = b->x1 - b->x0 + 1, lw, bg, inner, pad, round;
	char tmp[48];

	if (b->cmd && !strcmp(b->cmd, "@case"))
		lab = caselab[casemode];
	if (ulen(lab) > bw && b->shortl) lab = b->shortl;
	lw = ulen(lab);
	bg = i == pressed ? (pressfade ? 24 : 31) : (i == pend ? 238 : 236);
	round = bw >= 4 && lw <= bw - 2;        /* corner cells eat one column each side */
	inner = round ? bw - 2 : bw;
	pad = (inner - lw) / 2;
	if (pad < 0) pad = 0;

	for (int r = b->y0; r <= b->y1 && r < H; r++) {
		at(r, b->x0);
		if (r == b->y1 && b->y1 > b->y0) {
			/* lower half line: rounded bottom edge, the other half is the gap */
			printf("\033[38;5;%dm", bg);
			if (round) {
				printf("▝");
				for (int k = 0; k < bw - 2; k++) printf("▀");
				printf("▘");
			} else {
				for (int k = 0; k < bw; k++) printf("▀");
			}
		} else {
			if (round) printf("\033[38;5;%dm▟", bg);
			printf("\033[48;5;%dm", bg);
			if (r == b->y0 && lw <= inner) {
				printf("%*s", pad, "");
				if (b->pad && i == pend) {
					/* the letter that the next tap would replace, highlighted */
					const char *p = lab;
					int k = 0;
					while (*p) {
						int n = 1;
						while ((p[n] & 0xc0) == 0x80) n++;
						if (k >= 2 && k - 2 == pendidx)
							printf("\033[1;38;5;232;48;5;80m%.*s\033[48;5;%dm", n, p, bg);
						else
							printf("%s%.*s", k == 0 ? FG_W : FG_DI, n, p);
						p += n;
						k++;
					}
				} else {
					snprintf(tmp, sizeof(tmp), "%s", lab);
					printf("%s%s", b->fg, tmp);
				}
				printf("\033[48;5;%dm%*s", bg, inner - lw - pad, "");
			} else {
				printf("%*s", inner, "");
			}
			if (round) printf("\033[0m\033[38;5;%dm▙", bg);
		}
		printf("\033[0m");
	}
}

static void draw_nav(void)
{
	int lh = H >= 2 ? 2 : 1;

	if (pagenum == 1) {
		place(0, 0, 0, lh);
		place(1, 2, lh, lh);        /* arrow block: same columns for both rows */
	} else if (pagenum == 2) {
		place(0, 3, 0, lh);         /* keypad grid */
		place(4, 4, 4 * lh, lh);
	} else {
		place(0, 0, 0, lh);
	}
	printf("\033[2J");
	for (int i = 0; i < npage; i++)
		draw_button(&page[i], i);
}

static void set_page(int n, int relayout)
{
	static const char *names[3] = { "main", "keys", "pad" };
	char cmd[96];

	pagenum = n;
	page = n == 1 ? keys_page : n == 2 ? pad_page : main_page;
	npage = n == 1 ? N(keys_page) : n == 2 ? N(pad_page) : N(main_page);
	pend = -1;
	if (!relayout)
		return;
	/* the keys / pad pages need more rows: watch-frame --page resizes (and reopens
	 * a menu that the resize closed) */
	snprintf(cmd, sizeof(cmd), "watch-frame --page %s", names[n]);
	run(cmd);
}

/* send-keys to the middle pane without a shell (a literal ' or $ stays literal) */
static void send(const char *key, int literal)
{
	pid_t p = fork();
	if (p == 0) {
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); }
		if (literal)
			execlp("tmux", "tmux", "-L", "frame", "send-keys", "-t", center[0] ? center : "{last}",
			       "-l", key, (char *)NULL);
		else
			execlp("tmux", "tmux", "-L", "frame", "send-keys", "-t", center[0] ? center : "{last}",
			       key, (char *)NULL);
		_exit(127);
	}
	if (p > 0)
		waitpid(p, NULL, 0);
}

static void pad_tap(int i)
{
	struct button *b = &page[i];
	const char *letters = b->pad;
	int n = (int)strlen(letters);
	char c[2] = { 0, 0 };
	long long t = now_ms();

	if (casemode == 3) {                  /* 123: the key's digit */
		c[0] = letters[n - 1];
		send(c, 1);
		pend = -1;
		return;
	}
	if (pend == i && t - pendt < MULTITAP_MS) {
		pendidx = (pendidx + 1) % n;
		send("BSpace", 0);
	} else {
		if (casemode == 1 && pend >= 0)
			casemode = 0;                 /* "Abc": the capital was used */
		pend = i;
		pendidx = 0;
	}
	pendt = t;
	c[0] = letters[pendidx];
	if (casemode == 2 || casemode == 1)
		c[0] = (char)toupper((unsigned char)c[0]);
	send(c, 1);
}

static void click(int x, int y)
{
	int best = -1, bestd = 3;

	if (strcmp(mode, "nav"))
		return;
	/* nearest button; a tap up to one cell beside it counts (fingers are fat) */
	for (int i = 0; i < npage; i++) {
		struct button *b = &page[i];
		int dx = x < b->x0 ? b->x0 - x : x > b->x1 ? x - b->x1 : 0;
		int dy = y < b->y0 ? b->y0 - y : y > b->y1 ? y - b->y1 : 0;
		if (dx <= 1 && dy <= 1 && dx + dy < bestd) { bestd = dx + dy; best = i; }
	}
	if (best < 0)
		return;

	/* press feedback: bright, then a short fade back */
	pressed = best;
	pressfade = 0;
	draw_button(&page[best], best);
	fflush(stdout);
	{
		struct button *b = &page[best];
		if (b->pad) {
			pad_tap(best);
		} else if (b->cmd && b->cmd[0] == '@') {
			if (pend >= 0 && casemode == 1) casemode = 0;
			pend = -1;
			if (!strcmp(b->cmd, "@case")) casemode = (casemode + 1) % 4;
			else set_page(!strcmp(b->cmd, "@keys") ? 1 : !strcmp(b->cmd, "@pad") ? 2 : 0, 1);
		} else if (b->keys) {
			pend = -1;
			send(b->keys, 0);
		} else {
			pend = -1;
			run(b->cmd);
		}
	}
	usleep(70000);
	pressfade = 1;
	if (best < npage) draw_button(&page[best], best);
	fflush(stdout);
	usleep(70000);
	pressed = -1;
	pressfade = 0;
}

/* ------------------------------------------------------------------ main */

static void draw(void)
{
	static int n;
	/* also re-check every ~10 s: the pane can move without changing size */
	if (resized || ++n % 5 == 0 || n < 4) { resized = 0; geometry(); }
	if (!strcmp(mode, "mem")) draw_mem();
	else if (!strcmp(mode, "cpu")) draw_cpu();
	else draw_nav();
	fflush(stdout);
}

int main(int argc, char **argv)
{
	struct termios raw;
	char in[64], cur[32];
	int ilen = 0;

	if (argc != 2 || (strcmp(argv[1], "mem") && strcmp(argv[1], "cpu") && strcmp(argv[1], "nav"))) {
		fprintf(stderr, "usage: watch-panel mem|cpu|nav\n");
		return 2;
	}
	snprintf(mode, sizeof(mode), "%s", argv[1]);
	if (getenv("TMUX_PANE"))
		snprintf(self, sizeof(self), "%s", getenv("TMUX_PANE"));
	unsetenv("TMUX");          /* plain "tmux" = the inner "watch" tmux */
	unsetenv("TMUX_PANE");
	signal(SIGWINCH, on_winch);
	signal(SIGTERM, on_term);
	signal(SIGHUP, on_term);
	signal(SIGCHLD, SIG_DFL);

	if (!strcmp(mode, "nav") && getenv("WATCH_NAV_PAGE"))
		snprintf(cur, sizeof(cur), "%s", getenv("WATCH_NAV_PAGE"));
	if (!strcmp(mode, "nav") && (getenv("WATCH_NAV_PAGE") ||
	    read_cmd(FRAME "show -gv @navmode 2>/dev/null", cur, sizeof(cur))))
		set_page(!strcmp(cur, "keys") ? 1 : !strcmp(cur, "pad") ? 2 : 0, 0);

	tcgetattr(0, &saved);
	raw = saved;
	raw.c_lflag &= ~(ICANON | ECHO);
	raw.c_cc[VMIN] = 0;
	raw.c_cc[VTIME] = 0;
	tcsetattr(0, TCSANOW, &raw);
	printf("\033[?25l\033[?1000h\033[?1006h");
	if (!strcmp(mode, "cpu")) { geometry(); resized = 0; draw_cpu(); usleep(300000); }

	for (;;) {
		struct pollfd pf = { .fd = 0, .events = POLLIN };
		int timeout = 2000;
		while (waitpid(-1, NULL, WNOHANG) > 0)
			;                    /* reap button commands */
		if (pend >= 0) {
			long long left = MULTITAP_MS - (now_ms() - pendt);
			if (left <= 0) {
				pend = -1;       /* letter accepted */
				if (casemode == 1) casemode = 0;
			} else {
				timeout = (int)left + 10;
			}
		}
		draw();
		if (poll(&pf, 1, timeout) > 0) {
			ssize_t r = read(0, in + ilen, sizeof(in) - 1 - ilen);
			if (r <= 0) {
				if (r == 0) { restore(); return 0; }
				continue;
			}
			ilen += (int)r;
			in[ilen] = 0;
			/* SGR mouse: ESC [ < b ; x ; y M  (press) / m (release) */
			for (char *s = strstr(in, "\033[<"); s; s = strstr(s + 1, "\033[<")) {
				int b, x, y;
				char t;
				if (sscanf(s + 3, "%d;%d;%d%c", &b, &x, &y, &t) == 4 && t == 'M' && b == 0)
					click(x - 1, y - 1);
			}
			ilen = 0;
		}
	}
}
