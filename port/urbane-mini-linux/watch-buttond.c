// SPDX-License-Identifier: MIT
/*
 * watch-buttond - side button, screen blanking and haptics for the LG Watch Urbane.
 *
 * Runs as the session user (started by watch-session), so every action runs
 * with the user's tmux / Wayland environment.
 *
 *  - Side button (pm8941_pwrkey): single / double / triple / long press,
 *    each mapped to a shell command in ~/.config/urbane/buttons.conf
 *    (falls back to /etc/urbane/buttons.conf). The file is re-read when it
 *    changes, so the mapping can be changed live.
 *  - Screen blanking: after IDLE_OFF seconds without keyboard / touch /
 *    button input the backlight is switched off. Any input wakes it; that
 *    first touch or key press is swallowed (devices are grabbed while dark).
 *  - Short vibration as feedback when a button action fires (HAPTIC=1).
 *
 * Built-in actions (use them as the command):
 *   @screen-toggle  @screen-off  @screen-on  @none
 *
 * Signals: SIGUSR1 = screen toggle, SIGHUP = re-read config.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/input.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define MAX_DEV 16
#define CMD_LEN 512
#define BITS_PER_LONG (sizeof(long) * 8)
#define NLONGS(x) (((x) + BITS_PER_LONG - 1) / BITS_PER_LONG)
#define TEST_BIT(b, a) (((a)[(b) / BITS_PER_LONG] >> ((b) % BITS_PER_LONG)) & 1UL)

enum { P_SINGLE, P_DOUBLE, P_TRIPLE, P_LONG, P_COUNT };
static const char *press_name[P_COUNT] = { "single", "double", "triple", "long" };

struct config {
	char action[P_COUNT][CMD_LEN];
	int idle_off;       /* seconds, 0 = never */
	int multi_ms;       /* max gap between presses of a double/triple press */
	int long_ms;        /* hold time for a long press */
	int haptic;         /* 0/1 */
	int haptic_ms;
	char backlight[200];
};

struct dev {
	int fd;
	char path[300];
	int is_button;   /* the watch side button */
	int is_vibra;
	int grabbed;
};

static struct config cfg;
static struct dev devs[MAX_DEV];
static int ndev;
static char conf_user[PATH_MAX], conf_sys[] = "/etc/urbane/buttons.conf";
static time_t conf_mtime;
static char state_file[PATH_MAX];
static volatile sig_atomic_t sig_toggle, sig_reload;
static int verbose;
static const char *force_button;       /* -b PATH: treat PATH as the side button (testing) */
static int bl_ok = 1;                  /* 0 when the backlight is not writable */
static long long ungrab_at;            /* release grabs after the wake press */

static void logmsg(const char *fmt, ...)
{
	va_list ap;
	if (!verbose)
		return;
	va_start(ap, fmt);
	fprintf(stderr, "watch-buttond: ");
	vfprintf(stderr, fmt, ap);
	fputc('\n', stderr);
	va_end(ap);
}

static long long now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ config */

static void set_defaults(struct config *c)
{
	memset(c, 0, sizeof(*c));
	strcpy(c->action[P_SINGLE], "@screen-toggle");
	strcpy(c->action[P_DOUBLE], "watch-menu");
	strcpy(c->action[P_TRIPLE], "watch-viewport --cycle");
	strcpy(c->action[P_LONG], "watch-menu power");
	c->idle_off = 60;
	c->multi_ms = 350;
	c->long_ms = 800;
	c->haptic = 1;
	c->haptic_ms = 40;
}

static char *trim(char *s)
{
	char *e;
	while (*s == ' ' || *s == '\t')
		s++;
	e = s + strlen(s);
	while (e > s && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t'))
		*--e = 0;
	/* strip one level of matching quotes */
	if (e - s >= 2 && ((*s == '"' && e[-1] == '"') || (*s == '\'' && e[-1] == '\''))) {
		e[-1] = 0;
		s++;
	}
	return s;
}

static void parse_file(const char *path, struct config *c)
{
	char line[CMD_LEN + 64];
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	while (fgets(line, sizeof(line), f)) {
		char *k = trim(line), *v;
		if (!*k || *k == '#')
			continue;
		v = strchr(k, '=');
		if (!v)
			continue;
		*v++ = 0;
		k = trim(k);
		v = trim(v);
		for (int i = 0; i < P_COUNT; i++)
			if (!strcasecmp(k, press_name[i]))
				snprintf(c->action[i], CMD_LEN, "%s", v);
		if (!strcasecmp(k, "IDLE_OFF"))
			c->idle_off = atoi(v);
		else if (!strcasecmp(k, "MULTI_PRESS_MS"))
			c->multi_ms = atoi(v);
		else if (!strcasecmp(k, "LONG_PRESS_MS"))
			c->long_ms = atoi(v);
		else if (!strcasecmp(k, "HAPTIC"))
			c->haptic = atoi(v);
		else if (!strcasecmp(k, "HAPTIC_MS"))
			c->haptic_ms = atoi(v);
		else if (!strcasecmp(k, "BACKLIGHT"))
			snprintf(c->backlight, sizeof(c->backlight), "%s", v);
	}
	fclose(f);
}

static time_t mtime_of(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? st.st_mtime : 0;
}

static void find_backlight(struct config *c)
{
	DIR *d;
	struct dirent *e;
	if (c->backlight[0])
		return;
	d = opendir("/sys/class/backlight");
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (e->d_name[0] == '.')
			continue;
		snprintf(c->backlight, sizeof(c->backlight), "/sys/class/backlight/%s", e->d_name);
		break;
	}
	closedir(d);
}

static void load_config(void)
{
	struct config c;
	set_defaults(&c);
	parse_file(conf_sys, &c);
	parse_file(conf_user, &c);
	find_backlight(&c);
	if (c.multi_ms < 100)
		c.multi_ms = 100;
	if (c.long_ms < 300)
		c.long_ms = 300;
	cfg = c;
	conf_mtime = mtime_of(conf_user) ^ mtime_of(conf_sys);
	logmsg("config: single='%s' double='%s' triple='%s' long='%s' idle=%ds",
	       cfg.action[0], cfg.action[1], cfg.action[2], cfg.action[3], cfg.idle_off);
}

/* ------------------------------------------------------------------ screen */

static int read_int(const char *path, int def)
{
	char buf[32];
	int fd = open(path, O_RDONLY), n;
	if (fd < 0)
		return def;
	n = read(fd, buf, sizeof(buf) - 1);
	close(fd);
	if (n <= 0)
		return def;
	buf[n] = 0;
	return atoi(buf);
}

static int write_int_flags(const char *path, int v, int flags)
{
	char buf[32];
	int fd = open(path, O_WRONLY | flags, 0600), n, r;
	if (fd < 0)
		return -1;
	n = snprintf(buf, sizeof(buf), "%d\n", v);
	r = write(fd, buf, n) == n ? 0 : -1;
	close(fd);
	return r;
}

static int write_int(const char *path, int v)
{
	return write_int_flags(path, v, O_TRUNC);
}

static int bl_path(char *out, size_t len, const char *file)
{
	if (!cfg.backlight[0])
		return -1;
	snprintf(out, len, "%s/%s", cfg.backlight, file);
	return 0;
}

static int screen_is_on(void)
{
	char p[PATH_MAX];
	if (bl_path(p, sizeof(p), "brightness"))
		return 1;
	return read_int(p, 1) > 0;
}

static void set_grab(int on);

static void screen_off(void)
{
	char p[PATH_MAX];
	int cur;
	if (bl_path(p, sizeof(p), "brightness"))
		return;
	cur = read_int(p, 0);
	if (cur > 0)
		write_int_flags(state_file, cur, O_CREAT | O_TRUNC);   /* remembered for screen on */
	if (write_int(p, 0) != 0) {
		/* never grab input while the screen stays lit */
		if (bl_ok)
			fprintf(stderr, "watch-buttond: cannot write %s - screen blanking disabled "
				"(needs group video, see urbane-hw service)\n", p);
		bl_ok = 0;
		return;
	}
	bl_path(p, sizeof(p), "bl_power");
	write_int(p, 4);                  /* FB_BLANK_POWERDOWN */
	set_grab(1);
	logmsg("screen off");
}

static void screen_on_keep_grab(void)
{
	char p[PATH_MAX], m[PATH_MAX];
	int v = read_int(state_file, 0);
	if (bl_path(p, sizeof(p), "bl_power"))
		return;
	write_int(p, 0);
	bl_path(m, sizeof(m), "max_brightness");
	if (v <= 0)
		v = read_int(m, 255);
	bl_path(p, sizeof(p), "brightness");
	write_int(p, v);
	logmsg("screen on (%d)", v);
}

static void screen_on(void)
{
	screen_on_keep_grab();
	set_grab(0);
}

/* ------------------------------------------------------------------ input devices */

static void close_dev(int i)
{
	close(devs[i].fd);
	devs[i] = devs[--ndev];
}

static int have_dev(const char *path)
{
	for (int i = 0; i < ndev; i++)
		if (!strcmp(devs[i].path, path))
			return 1;
	return 0;
}

static void open_dev(const char *path)
{
	unsigned long ev[NLONGS(EV_CNT)] = { 0 }, keys[NLONGS(KEY_CNT)] = { 0 };
	char name[128] = "";
	struct dev d = { .fd = -1 };
	int fd;

	if (ndev >= MAX_DEV || have_dev(path))
		return;
	fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
	if (fd < 0)
		return;
	ioctl(fd, EVIOCGNAME(sizeof(name)), name);
	ioctl(fd, EVIOCGBIT(0, sizeof(ev)), ev);
	ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keys)), keys);

	d.fd = fd;
	snprintf(d.path, sizeof(d.path), "%s", path);
	d.is_vibra = TEST_BIT(EV_FF, ev);
	d.is_button = (force_button && !strcmp(path, force_button)) ||
		      !strcmp(name, "pm8941_pwrkey") ||
		      (TEST_BIT(EV_KEY, ev) && TEST_BIT(KEY_POWER, keys) && !TEST_BIT(KEY_A, keys));
	/* keep only devices that are useful: button, vibrator, keys, touch, pointers */
	if (!d.is_vibra && !d.is_button && !TEST_BIT(EV_KEY, ev) && !TEST_BIT(EV_ABS, ev) &&
	    !TEST_BIT(EV_REL, ev)) {
		close(fd);
		return;
	}
	devs[ndev++] = d;
	logmsg("watching %s \"%s\"%s%s", path, name, d.is_button ? " [button]" : "",
	       d.is_vibra ? " [vibra]" : "");
	if (!screen_is_on() && !d.is_button && !d.is_vibra) {
		if (ioctl(fd, EVIOCGRAB, 1) == 0)
			devs[ndev - 1].grabbed = 1;
	}
}

static void scan_devices(void)
{
	DIR *d = opendir("/dev/input");
	struct dirent *e;
	char p[300];
	if (!d)
		return;
	while ((e = readdir(d))) {
		if (strncmp(e->d_name, "event", 5))
			continue;
		snprintf(p, sizeof(p), "/dev/input/%s", e->d_name);
		open_dev(p);
	}
	closedir(d);
}

static void set_grab(int on)
{
	for (int i = 0; i < ndev; i++) {
		if (devs[i].is_button || devs[i].is_vibra || devs[i].grabbed == on)
			continue;
		if (ioctl(devs[i].fd, EVIOCGRAB, on ? 1 : 0) == 0)
			devs[i].grabbed = on;
	}
}

static void buzz(int ms)
{
	struct ff_effect eff;
	struct input_event play;
	if (!cfg.haptic || ms <= 0)
		return;
	for (int i = 0; i < ndev; i++) {
		if (!devs[i].is_vibra)
			continue;
		memset(&eff, 0, sizeof(eff));
		eff.type = FF_RUMBLE;
		eff.id = -1;
		eff.u.rumble.strong_magnitude = 0xc000;
		eff.u.rumble.weak_magnitude = 0xc000;
		eff.replay.length = ms;
		if (ioctl(devs[i].fd, EVIOCSFF, &eff) < 0)
			continue;
		memset(&play, 0, sizeof(play));
		play.type = EV_FF;
		play.code = eff.id;
		play.value = 1;
		if (write(devs[i].fd, &play, sizeof(play)) < 0)
			logmsg("vibra write failed: %s", strerror(errno));
		/* the effect slot is freed on the next upload; keep it simple */
		usleep((ms + 10) * 1000);
		ioctl(devs[i].fd, EVIOCRMFF, eff.id);
		return;
	}
}

/* ------------------------------------------------------------------ actions */

static void run_action(int press)
{
	const char *a;
	pid_t pid;

	a = cfg.action[press];
	logmsg("%s press -> %s", press_name[press], a);
	if (!*a || !strcmp(a, "@none"))
		return;
	buzz(press == P_LONG ? cfg.haptic_ms * 2 : cfg.haptic_ms);
	if (!strcmp(a, "@screen-toggle")) {
		screen_is_on() ? screen_off() : screen_on();
		return;
	}
	if (!strcmp(a, "@screen-off")) {
		screen_off();
		return;
	}
	if (!strcmp(a, "@screen-on")) {
		screen_on();
		return;
	}
	/* any other action needs a visible screen */
	if (!screen_is_on())
		screen_on();
	pid = fork();
	if (pid == 0) {
		setsid();
		for (int i = 0; i < ndev; i++)
			close(devs[i].fd);
		execl("/bin/sh", "sh", "-c", a, (char *)NULL);
		_exit(127);
	}
}

static void on_signal(int s)
{
	if (s == SIGUSR1)
		sig_toggle = 1;
	else if (s == SIGHUP)
		sig_reload = 1;
}

/* ------------------------------------------------------------------ main loop */

int main(int argc, char **argv)
{
	const char *home = getenv("HOME"), *rt = getenv("XDG_RUNTIME_DIR");
	struct pollfd pfd[MAX_DEV + 1];
	struct sigaction sa;
	long long last_input = now_ms(), down_at = 0, last_release = 0;
	int down = 0, presses = 0, long_fired = 0, ino;
	char ibuf[4096];

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = 1;
		else if (!strcmp(argv[i], "-b") && i + 1 < argc)
			force_button = argv[++i];
		else {
			fprintf(stderr, "usage: watch-buttond [-v] [-b /dev/input/eventN]\n");
			return 2;
		}
	}
	snprintf(conf_user, sizeof(conf_user), "%s/.config/urbane/buttons.conf", home ? home : "/root");
	snprintf(state_file, sizeof(state_file), "%s/watch-brightness", rt ? rt : "/tmp");

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	sigaction(SIGUSR1, &sa, NULL);
	sigaction(SIGHUP, &sa, NULL);
	sa.sa_handler = SIG_DFL;
	sa.sa_flags = SA_NOCLDWAIT;       /* actions are fire-and-forget */
	sigaction(SIGCHLD, &sa, NULL);

	load_config();
	ino = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
	if (ino >= 0)
		inotify_add_watch(ino, "/dev/input", IN_CREATE | IN_ATTRIB);
	scan_devices();
	if (force_button)
		open_dev(force_button);
	if (!ndev)
		fprintf(stderr, "watch-buttond: no readable input devices (is the user in group 'input'?)\n");

	for (;;) {
		long long t = now_ms();
		int timeout = 1000, n, np = 0;

		if (sig_reload || (mtime_of(conf_user) ^ mtime_of(conf_sys)) != conf_mtime) {
			sig_reload = 0;
			load_config();
		}
		if (sig_toggle) {
			sig_toggle = 0;
			screen_is_on() ? screen_off() : screen_on();
			last_input = t;
		}

		/* resolve pending presses */
		if (down && !long_fired && t - down_at >= cfg.long_ms) {
			long_fired = 1;
			presses = 0;
			run_action(P_LONG);
		}
		if (!down && presses && t - last_release >= cfg.multi_ms) {
			int p = presses >= 3 ? P_TRIPLE : presses == 2 ? P_DOUBLE : P_SINGLE;
			presses = 0;
			run_action(p);
			last_input = now_ms();
		}
		/* idle blanking */
		if (ungrab_at && t >= ungrab_at) {
			ungrab_at = 0;
			set_grab(0);
		}
		if (bl_ok && cfg.idle_off > 0 && t - last_input >= (long long)cfg.idle_off * 1000 && screen_is_on())
			screen_off();

		if (down || ungrab_at)
			timeout = 50;
		else if (presses)
			timeout = cfg.multi_ms - (int)(t - last_release) + 5;
		else if (cfg.idle_off > 0) {
			long long left = (long long)cfg.idle_off * 1000 - (t - last_input);
			timeout = left > 5000 ? 5000 : left < 50 ? 50 : (int)left;
		}
		if (timeout < 10)
			timeout = 10;

		for (int i = 0; i < ndev; i++) {
			pfd[np].fd = devs[i].fd;
			pfd[np++].events = POLLIN;
		}
		if (ino >= 0) {
			pfd[np].fd = ino;
			pfd[np++].events = POLLIN;
		}
		n = poll(pfd, np, timeout);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			perror("poll");
			return 1;
		}
		if (ino >= 0 && (pfd[np - 1].revents & POLLIN)) {
			while (read(ino, ibuf, sizeof(ibuf)) > 0)
				;
			usleep(200000);   /* let udev fix the permissions */
			scan_devices();
		}
		for (int i = ndev - 1; i >= 0; i--) {
			struct input_event ev[32];
			ssize_t r;
			if (!(pfd[i].revents & (POLLIN | POLLERR | POLLHUP)))
				continue;
			r = read(devs[i].fd, ev, sizeof(ev));
			if (r < 0 && errno != EAGAIN) {
				logmsg("lost %s", devs[i].path);
				close_dev(i);
				continue;
			}
			for (int k = 0; k < r / (ssize_t)sizeof(ev[0]); k++) {
				if (devs[i].is_vibra && ev[k].type != EV_KEY)
					continue;
				if (ev[k].type == EV_SYN || ev[k].type == EV_MSC)
					continue;
				last_input = now_ms();
				if (devs[i].is_button && ev[k].type == EV_KEY && ev[k].code == KEY_POWER) {
					if (ev[k].value == 1 && !down) {
						down = 1;
						long_fired = 0;
						down_at = last_input;
					} else if (ev[k].value == 0 && down) {
						down = 0;
						if (!long_fired) {
							presses++;
							last_release = last_input;
						}
					}
				} else if (!devs[i].is_button && devs[i].grabbed) {
					/* While dark the devices are grabbed: the first key press
					 * or touch only wakes the screen. Keep the grab until it
					 * is released so the app never sees half a key press. */
					if (ev[k].type == EV_KEY && ev[k].value == 1 && !screen_is_on()) {
						screen_on_keep_grab();
						ungrab_at = now_ms() + 1500;
					} else if (ev[k].type == EV_KEY && ev[k].value == 0 && screen_is_on()) {
						ungrab_at = now_ms() + 100;
					}
				}
			}
		}
	}
}
