/* apps/sys_status.c
 *
 * System status dashboard for BGCE/BGTK.
 * Shows clock, uptime, CPU, memory, swap, disk, internet, and weather.
 *
 * Build (Linux):  make sys_status
 * Headless test:  make test_sys_status && ./test_sys_status
 */

#include <ctype.h>
#include <errno.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>

#if defined(__APPLE__)
#include <mach/mach.h>
#include <mach/processor_info.h>
#include <mach/mach_host.h>
#include <sys/mount.h>
#include <sys/sysctl.h>
#else
#include <sys/sysinfo.h>
#endif

#include <linux/input.h>
#include <sys/mman.h>

#include "bgtk.h"
#include "internal.h"

/* Bootstrap buffer only — final size is measured from content. */
#define BOOT_W 640
#define BOOT_H 400
#define N_ROWS 8
#define BGCE_BPP 4

enum {
	ROW_CLOCK = 0,
	ROW_UPTIME,
	ROW_CPU,
	ROW_MEM,
	ROW_SWAP,
	ROW_DISK,
	ROW_NET,
	ROW_WEATHER
};

static const char *row_names[N_ROWS] = {
	"Clock", "Uptime", "CPU", "Memory", "Swap", "Disk /", "Internet", "Weather"
};

static struct BGTK_Context *ctx;
static struct BGTK_Widget *name_labels[N_ROWS];
static struct BGTK_Widget *value_labels[N_ROWS];
static struct BGTK_Widget *row_widgets[N_ROWS];
static struct BGTK_Widget *root_list;

/* CPU sample for delta % */
static unsigned long long cpu_idle_prev, cpu_total_prev;
static int cpu_have_prev;

/* ------------------------------------------------------------------ */
/* Formatting helpers                                                  */
/* ------------------------------------------------------------------ */

static void fmt_bytes(char *out, size_t n, unsigned long long bytes)
{
	const double kb = 1024.0, mb = kb * 1024.0, gb = mb * 1024.0;
	double b = (double)bytes;

	if (b >= gb)
		snprintf(out, n, "%.1f GB", b / gb);
	else if (b >= mb)
		snprintf(out, n, "%.0f MB", b / mb);
	else if (b >= kb)
		snprintf(out, n, "%.0f KB", b / kb);
	else
		snprintf(out, n, "%llu B", bytes);
}

static void fmt_bar(char *out, size_t n, int pct)
{
	/* 10-char bar */
	int i, filled;

	if (pct < 0)
		pct = 0;
	if (pct > 100)
		pct = 100;
	filled = (pct + 5) / 10;
	out[0] = '[';
	for (i = 0; i < 10; i++)
		out[1 + i] = (i < filled) ? '#' : '-';
	out[11] = ']';
	out[12] = '\0';
	(void)n;
}

static void set_label_text(struct BGTK_Widget *w, const char *s)
{
	if (!w || !s)
		return;
	if (w->type == BGTK_WIDGET_TEXT) {
		free(w->data.text.text);
		w->data.text.text = strdup(s);
	} else if (w->type == BGTK_WIDGET_LABEL && w->data.label.set_label) {
		w->data.label.set_label(w, (char *)s);
	}
}

/* ------------------------------------------------------------------ */
/* Metrics                                                             */
/* ------------------------------------------------------------------ */

static void metric_clock(char *out, size_t n)
{
	time_t now = time(NULL);
	struct tm tm;

	localtime_r(&now, &tm);
	strftime(out, n, "%a %Y-%m-%d %H:%M:%S", &tm);
}

static void metric_uptime(char *out, size_t n)
{
	unsigned long sec = 0;
	unsigned long d, h, m;

#if defined(__APPLE__)
	struct timeval boot;
	size_t len = sizeof(boot);
	if (sysctlbyname("kern.boottime", &boot, &len, NULL, 0) == 0) {
		time_t now = time(NULL);
		if (now > boot.tv_sec)
			sec = (unsigned long)(now - boot.tv_sec);
	}
#else
	struct sysinfo si;
	if (sysinfo(&si) == 0)
		sec = (unsigned long)si.uptime;
#endif
	d = sec / 86400;
	h = (sec / 3600) % 24;
	m = (sec / 60) % 60;
	if (d > 0)
		snprintf(out, n, "%lud %luh %lum", d, h, m);
	else if (h > 0)
		snprintf(out, n, "%luh %lum %lus", h, m, sec % 60);
	else
		snprintf(out, n, "%lum %lus", m, sec % 60);
}

#if defined(__APPLE__)
static int cpu_sample_mac(unsigned long long *idle, unsigned long long *total)
{
	natural_t count = 0;
	processor_info_array_t info = NULL;
	mach_msg_type_number_t info_count = 0;
	unsigned long long i_sum = 0, t_sum = 0;
	unsigned int i;

	if (host_processor_info(mach_host_self(), PROCESSOR_CPU_LOAD_INFO,
				&count, &info, &info_count) != KERN_SUCCESS)
		return -1;
	for (i = 0; i < count; i++) {
		processor_cpu_load_info_data_t *c =
			(processor_cpu_load_info_data_t *)info + i;
		unsigned long long user = c->cpu_ticks[CPU_STATE_USER];
		unsigned long long sys = c->cpu_ticks[CPU_STATE_SYSTEM];
		unsigned long long idle_t = c->cpu_ticks[CPU_STATE_IDLE];
		unsigned long long nice = c->cpu_ticks[CPU_STATE_NICE];
		i_sum += idle_t;
		t_sum += user + sys + idle_t + nice;
	}
	vm_deallocate(mach_task_self(), (vm_address_t)info,
		      info_count * sizeof(integer_t));
	*idle = i_sum;
	*total = t_sum;
	return 0;
}
#else
static int cpu_sample_linux(unsigned long long *idle, unsigned long long *total)
{
	FILE *f = fopen("/proc/stat", "r");
	unsigned long long u, n, s, id, io, irq, sirq, st;
	char line[256];

	if (!f)
		return -1;
	if (!fgets(line, sizeof(line), f)) {
		fclose(f);
		return -1;
	}
	fclose(f);
	/* cpu  user nice system idle iowait irq softirq steal ... */
	if (sscanf(line, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
		   &u, &n, &s, &id, &io, &irq, &sirq, &st) < 4)
		return -1;
	*idle = id + io;
	*total = u + n + s + id + io + irq + sirq + st;
	return 0;
}
#endif

static void metric_cpu(char *out, size_t n)
{
	unsigned long long idle = 0, total = 0;
	int pct = 0;
	char bar[16];

#if defined(__APPLE__)
	if (cpu_sample_mac(&idle, &total) != 0) {
		snprintf(out, n, "n/a");
		return;
	}
#else
	if (cpu_sample_linux(&idle, &total) != 0) {
		snprintf(out, n, "n/a");
		return;
	}
#endif
	if (cpu_have_prev && total > cpu_total_prev) {
		unsigned long long d_tot = total - cpu_total_prev;
		unsigned long long d_idle = idle - cpu_idle_prev;
		if (d_tot > 0)
			pct = (int)((d_tot - d_idle) * 100ull / d_tot);
	} else {
		pct = -1;
	}
	cpu_idle_prev = idle;
	cpu_total_prev = total;
	cpu_have_prev = 1;

	if (pct < 0) {
		snprintf(out, n, "sampling…");
		return;
	}
	fmt_bar(bar, sizeof(bar), pct);
	snprintf(out, n, "%d%%  %s", pct, bar);
}

static void metric_mem_swap(char *mem_out, size_t mn, char *swap_out, size_t sn)
{
	unsigned long long mem_total = 0, mem_used = 0;
	unsigned long long swap_total = 0, swap_used = 0;
	char tbuf[32], ubuf[32], bar[16];
	int pct;

#if defined(__APPLE__)
	{
		int mib[2] = { CTL_HW, HW_MEMSIZE };
		uint64_t memsize = 0;
		size_t len = sizeof(memsize);
		mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
		vm_statistics64_data_t vm;

		sysctl(mib, 2, &memsize, &len, NULL, 0);
		mem_total = memsize;
		if (host_statistics64(mach_host_self(), HOST_VM_INFO64,
				      (host_info64_t)&vm, &count) == KERN_SUCCESS) {
			/* active + wired ≈ used; inactive freeable */
			uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
			mem_used = (uint64_t)(vm.active_count + vm.wire_count) * page;
			swap_total = 0;
			swap_used = 0;
			/* xsw_usage for swap */
			struct xsw_usage xsw;
			len = sizeof(xsw);
			if (sysctlbyname("vm.swapusage", &xsw, &len, NULL, 0) == 0) {
				swap_total = xsw.xsu_total;
				swap_used = xsw.xsu_used;
			}
		} else {
			mem_used = 0;
		}
	}
#else
	{
		FILE *f = fopen("/proc/meminfo", "r");
		char key[64];
		unsigned long val;
		unsigned long mem_total_k = 0, mem_avail_k = 0, mem_free_k = 0;
		unsigned long buffers_k = 0, cached_k = 0;
		unsigned long swap_total_k = 0, swap_free_k = 0;
		char line[256];

		if (f) {
			while (fgets(line, sizeof(line), f)) {
				if (sscanf(line, "%63[^:]: %lu", key, &val) != 2)
					continue;
				if (!strcmp(key, "MemTotal"))
					mem_total_k = val;
				else if (!strcmp(key, "MemAvailable"))
					mem_avail_k = val;
				else if (!strcmp(key, "MemFree"))
					mem_free_k = val;
				else if (!strcmp(key, "Buffers"))
					buffers_k = val;
				else if (!strcmp(key, "Cached"))
					cached_k = val;
				else if (!strcmp(key, "SwapTotal"))
					swap_total_k = val;
				else if (!strcmp(key, "SwapFree"))
					swap_free_k = val;
			}
			fclose(f);
		}
		mem_total = mem_total_k * 1024ull;
		if (mem_avail_k)
			mem_used = (mem_total_k - mem_avail_k) * 1024ull;
		else
			mem_used = (mem_total_k - mem_free_k - buffers_k - cached_k) *
				   1024ull;
		swap_total = swap_total_k * 1024ull;
		swap_used = (swap_total_k - swap_free_k) * 1024ull;
	}
#endif

	fmt_bytes(ubuf, sizeof(ubuf), mem_used);
	fmt_bytes(tbuf, sizeof(tbuf), mem_total);
	pct = mem_total ? (int)(mem_used * 100ull / mem_total) : 0;
	fmt_bar(bar, sizeof(bar), pct);
	snprintf(mem_out, mn, "%s / %s (%d%%) %s", ubuf, tbuf, pct, bar);

	fmt_bytes(ubuf, sizeof(ubuf), swap_used);
	fmt_bytes(tbuf, sizeof(tbuf), swap_total);
	pct = swap_total ? (int)(swap_used * 100ull / swap_total) : 0;
	if (swap_total == 0)
		snprintf(swap_out, sn, "none");
	else {
		fmt_bar(bar, sizeof(bar), pct);
		snprintf(swap_out, sn, "%s / %s (%d%%) %s", ubuf, tbuf, pct, bar);
	}
}

static void metric_disk(char *out, size_t n)
{
	struct statvfs st;
	unsigned long long total, used, free_b;
	char tbuf[32], ubuf[32], bar[16];
	int pct;

	if (statvfs("/", &st) != 0) {
		snprintf(out, n, "n/a (%s)", strerror(errno));
		return;
	}
	total = (unsigned long long)st.f_blocks * st.f_frsize;
	free_b = (unsigned long long)st.f_bavail * st.f_frsize;
	used = total > free_b ? total - free_b : 0;
	pct = total ? (int)(used * 100ull / total) : 0;
	fmt_bytes(ubuf, sizeof(ubuf), used);
	fmt_bytes(tbuf, sizeof(tbuf), total);
	fmt_bar(bar, sizeof(bar), pct);
	snprintf(out, n, "%s / %s (%d%%) %s", ubuf, tbuf, pct, bar);
}

/*
 * Internet / weather need only libc networking (no curl/libtls):
 *   - Internet: dual-stack TCP :80 via getaddrinfo (IPv4 + IPv6)
 *   - Weather: DNS + HTTP/1.0 :80 to wttr.in (AF_UNSPEC)
 * No package deps. IPv6-only boards must not hardcode 1.1.1.1.
 */
/* 0 ok, -1 fail, -2 timeout */
static int tcp_probe_ai(const struct addrinfo *rp, long *out_ms)
{
	int fd, flags, err = 0;
	struct timeval tv0, tv1, tv;
	socklen_t elen = sizeof(err);
	fd_set wfds;

	fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
	if (fd < 0)
		return -1;
	flags = fcntl(fd, F_GETFL, 0);
	if (flags >= 0)
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	gettimeofday(&tv0, NULL);
	if (connect(fd, rp->ai_addr, rp->ai_addrlen) < 0 &&
	    errno != EINPROGRESS) {
		close(fd);
		return -1;
	}
	FD_ZERO(&wfds);
	FD_SET(fd, &wfds);
	tv.tv_sec = 2;
	tv.tv_usec = 0;
	if (select(fd + 1, NULL, &wfds, NULL, &tv) <= 0) {
		close(fd);
		return -2;
	}
	if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) < 0 || err != 0) {
		close(fd);
		return -1;
	}
	gettimeofday(&tv1, NULL);
	if (out_ms)
		*out_ms = (tv1.tv_sec - tv0.tv_sec) * 1000L +
			  (tv1.tv_usec - tv0.tv_usec) / 1000L;
	close(fd);
	return 0;
}

/* Resolve host:port (AF_UNSPEC) and try each address. */
static int tcp_probe_host(const char *host, const char *port, long *out_ms)
{
	struct addrinfo hints, *res = NULL, *rp;
	int saw_timeout = 0, r;

	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return -1;
	for (rp = res; rp; rp = rp->ai_next) {
		r = tcp_probe_ai(rp, out_ms);
		if (r == 0) {
			freeaddrinfo(res);
			return 0;
		}
		if (r == -2)
			saw_timeout = 1;
	}
	freeaddrinfo(res);
	return saw_timeout ? -2 : -1;
}

static void metric_internet(char *out, size_t n)
{
	long ms = 0;
	int r;
	static const char *hosts[] = {
		"one.one.one.one", /* Cloudflare A+AAAA */
		"dns.google",
		"cloudflare.com",
		NULL
	};
	int i;

	for (i = 0; hosts[i]; i++) {
		r = tcp_probe_host(hosts[i], "80", &ms);
		if (r == 0) {
			snprintf(out, n, "online (~%ld ms %s)", ms, hosts[i]);
			return;
		}
	}
	/* Literal v6 in case DNS itself is broken but connectivity works. */
	r = tcp_probe_host("2606:4700:4700::1111", "80", &ms);
	if (r == 0) {
		snprintf(out, n, "online (~%ld ms cf6)", ms);
		return;
	}
	r = tcp_probe_host("1.1.1.1", "80", &ms);
	if (r == 0) {
		snprintf(out, n, "online (~%ld ms 1.1.1.1)", ms);
		return;
	}
	if (r == -2)
		snprintf(out, n, "offline (timeout)");
	else
		snprintf(out, n, "offline (no route/DNS?)");
}

/* Minimal HTTP/1.0 GET (no TLS). err: 0 ok, -1 DNS, -2 connect, -3 HTTP. */
static int http_get_plain(const char *host, const char *path, char *body,
			  size_t body_len, int *err_kind)
{
	struct addrinfo hints, *res = NULL, *rp;
	int fd = -1, n;
	char req[512], buf[2048];
	size_t blen = 0;
	char *hdr_end;

	if (err_kind)
		*err_kind = -1;
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	if (getaddrinfo(host, "80", &hints, &res) != 0) {
		if (err_kind)
			*err_kind = -1;
		return -1;
	}
	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;
		{
			struct timeval tv = { .tv_sec = 4, .tv_usec = 0 };
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		}
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		if (err_kind)
			*err_kind = -2;
		return -1;
	}

	n = snprintf(req, sizeof(req),
		     "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: bgtk-sys_status\r\n"
		     "Connection: close\r\n\r\n",
		     path, host);
	if (write(fd, req, (size_t)n) < 0) {
		close(fd);
		if (err_kind)
			*err_kind = -2;
		return -1;
	}
	while (blen + 1 < sizeof(buf)) {
		ssize_t r = read(fd, buf + blen, sizeof(buf) - 1 - blen);
		if (r <= 0)
			break;
		blen += (size_t)r;
	}
	close(fd);
	buf[blen] = '\0';
	/* Redirects to HTTPS look like "HTTP/1.1 301" with empty useful body. */
	if (strncmp(buf, "HTTP/", 5) == 0) {
		int code = 0;
		sscanf(buf, "HTTP/%*s %d", &code);
		if (code >= 300 && code < 400) {
			if (err_kind)
				*err_kind = -3;
			return -1;
		}
	}
	hdr_end = strstr(buf, "\r\n\r\n");
	if (!hdr_end) {
		if (err_kind)
			*err_kind = -3;
		return -1;
	}
	hdr_end += 4;
	while (*hdr_end && isspace((unsigned char)*hdr_end))
		hdr_end++;
	{
		char *e = hdr_end + strlen(hdr_end);
		while (e > hdr_end && isspace((unsigned char)e[-1]))
			*--e = '\0';
	}
	if (!*hdr_end) {
		if (err_kind)
			*err_kind = -3;
		return -1;
	}
	snprintf(body, body_len, "%s", hdr_end);
	if (err_kind)
		*err_kind = 0;
	return 0;
}

static void metric_weather(char *out, size_t n)
{
	char body[256];
	char clean[256];
	char *p, *q;
	int sp = 0;
	int err = 0;

	/* Location + temp only (ASCII). Avoid emoji — BGTK draws one byte per
	 * FreeType index, so multi-byte UTF-8 looks like garbage.
	 * Needs DNS + HTTP:80 to wttr.in (no libcurl / no TLS). */
	if (http_get_plain("wttr.in", "/?format=%l:+%t", body, sizeof(body),
			   &err) != 0) {
		if (err == -1)
			snprintf(out, n, "n/a (DNS)");
		else if (err == -2)
			snprintf(out, n, "n/a (connect)");
		else
			snprintf(out, n, "n/a (HTTP)");
		return;
	}
	/* Keep printable ASCII; map other bytes to nothing (strip UTF-8). */
	for (p = body, q = clean; *p && (size_t)(q - clean) + 1 < sizeof(clean); p++) {
		unsigned char c = (unsigned char)*p;
		if (c >= 32 && c < 127) {
			if (c == ' ') {
				if (!sp) {
					*q++ = ' ';
					sp = 1;
				}
			} else {
				*q++ = (char)c;
				sp = 0;
			}
		}
	}
	*q = '\0';
	if (!clean[0])
		snprintf(out, n, "n/a");
	else
		snprintf(out, n, "%s", clean);
}

/* Refresh bitflags — different intervals in the main loop. */
enum {
	REFRESH_CLOCK = 1u << 0,   /* 1s */
	REFRESH_FAST = 1u << 1,    /* 2s: uptime, cpu, mem, swap */
	REFRESH_DISK = 1u << 2,    /* 30s */
	REFRESH_NET = 1u << 3,     /* 30s */
	REFRESH_WEATHER = 1u << 4, /* 10 min */
	REFRESH_ALL = 0x1fu
};

/* ------------------------------------------------------------------ */
/* Collect selected metrics + push into labels                         */
/* ------------------------------------------------------------------ */

void sys_status_refresh(unsigned flags)
{
	char buf[160];

	if (flags & REFRESH_CLOCK) {
		metric_clock(buf, sizeof(buf));
		if (value_labels[ROW_CLOCK])
			set_label_text(value_labels[ROW_CLOCK], buf);
	}
	if (flags & REFRESH_FAST) {
		metric_uptime(buf, sizeof(buf));
		if (value_labels[ROW_UPTIME])
			set_label_text(value_labels[ROW_UPTIME], buf);
		metric_cpu(buf, sizeof(buf));
		if (value_labels[ROW_CPU])
			set_label_text(value_labels[ROW_CPU], buf);
		{
			char swap[160];
			metric_mem_swap(buf, sizeof(buf), swap, sizeof(swap));
			if (value_labels[ROW_MEM])
				set_label_text(value_labels[ROW_MEM], buf);
			if (value_labels[ROW_SWAP])
				set_label_text(value_labels[ROW_SWAP], swap);
		}
	}
	if (flags & REFRESH_DISK) {
		metric_disk(buf, sizeof(buf));
		if (value_labels[ROW_DISK])
			set_label_text(value_labels[ROW_DISK], buf);
	}
	if (flags & REFRESH_NET) {
		metric_internet(buf, sizeof(buf));
		if (value_labels[ROW_NET])
			set_label_text(value_labels[ROW_NET], buf);
	}
	if (flags & REFRESH_WEATHER) {
		metric_weather(buf, sizeof(buf));
		if (value_labels[ROW_WEATHER])
			set_label_text(value_labels[ROW_WEATHER], buf);
	}
}

/* ------------------------------------------------------------------ */
/* UI                                                                  */
/* ------------------------------------------------------------------ */

static int ss_pad(struct BGTK_Context *c)
{
	return (c && c->theme.padding > 0) ? c->theme.padding : 12;
}

static int ss_mar(struct BGTK_Context *c)
{
	return (c && c->theme.margin > 0) ? c->theme.margin : 8;
}

/* Size root frame from label text (call after metrics are filled). */
static void sys_status_fit_size(struct BGTK_Context *c, struct BGTK_Widget *frame)
{
	int i, tw, th;
	int name_w = 0, val_w = 0, text_h = 0;
	int pad = ss_pad(c);
	int mar = ss_mar(c);
	int half = mar > 1 ? mar / 2 : 1;
	int text_p = pad > 2 ? pad / 2 : 2;
	int text_m = half > 0 ? half : 1;
	int row_m = half, row_p = half;
	int list_m = mar, list_p = pad > 2 ? pad / 2 : 2;
	int frame_p = pad, bw;
	int row_w, row_h, list_w, list_h;
	int text_chrome = 2 * (text_p + text_m);

	if (!c || !frame || !root_list)
		return;

	for (i = 0; i < N_ROWS; i++) {
		const char *ns = name_labels[i] && name_labels[i]->data.text.text
					 ? name_labels[i]->data.text.text
					 : row_names[i];
		const char *vs = value_labels[i] && value_labels[i]->data.text.text
					 ? value_labels[i]->data.text.text
					 : "?";
		int nh, vh;

		measure_text(c->ft_face, ns, &tw, &th);
		nh = th + text_chrome;
		tw += text_chrome;
		if (tw > name_w)
			name_w = tw;
		if (nh > text_h)
			text_h = nh;

		measure_text(c->ft_face, vs, &tw, &th);
		vh = th + text_chrome;
		tw += text_chrome;
		if (tw > val_w)
			val_w = tw;
		if (vh > text_h)
			text_h = vh;
	}
	/* A little slack so a slightly longer sample does not clip. */
	name_w += 4;
	val_w += 8;

	for (i = 0; i < N_ROWS; i++) {
		if (name_labels[i]) {
			name_labels[i]->w = name_w;
			name_labels[i]->h = text_h;
		}
		if (value_labels[i]) {
			value_labels[i]->w = val_w;
			value_labels[i]->h = text_h;
		}
		/* horizontal list: content = sum(w) + 2*m*(n-1), plus pad/margin */
		row_w = name_w + val_w + 2 * row_m + 2 * (row_p + row_m);
		row_h = text_h + 2 * (row_p + row_m);
		if (row_widgets[i]) {
			row_widgets[i]->w = row_w;
			row_widgets[i]->h = row_h;
		}
	}

	/* vertical list of N rows */
	list_w = row_w + 2 * (list_p + list_m);
	list_h = N_ROWS * row_h + 2 * list_m * (N_ROWS - 1) +
		 2 * (list_p + list_m);
	root_list->w = list_w;
	root_list->h = list_h;

	bw = frame->data.frame.border_w;
	if (bw < 1)
		bw = 1;
	frame->w = list_w + 2 * (frame_p + bw);
	frame->h = list_h + 2 * (frame_p + bw);
	if (frame->w < 1)
		frame->w = 1;
	if (frame->h < 1)
		frame->h = 1;
}

struct BGTK_Widget *sys_status_build_ui(struct BGTK_Context *c)
{
	struct BGTK_Widget *rows[N_ROWS];
	struct BGTK_Widget *list, *frame;
	int i;
	int pad = ss_pad(c);
	int mar = ss_mar(c);
	int half = mar > 1 ? mar / 2 : 1;
	int text_p = pad > 2 ? pad / 2 : 2;
	int text_m = half > 0 ? half : 1;
	int list_p = pad > 2 ? pad / 2 : 2;

	ctx = c;
	for (i = 0; i < N_ROWS; i++) {
		name_labels[i] = NULL;
		value_labels[i] = NULL;
		row_widgets[i] = NULL;
	}
	root_list = NULL;

	for (i = 0; i < N_ROWS; i++) {
		struct BGTK_Widget *name =
			bgtk_text(c, (char *)row_names[i],
				  (BGTK_Options){.padding = text_p,
						 .margin = text_m});
		struct BGTK_Widget *val =
			bgtk_text(c, "…",
				  (BGTK_Options){.padding = text_p,
						 .margin = text_m});
		struct BGTK_Widget *pair[2] = { name, val };
		struct BGTK_Widget *row;

		name_labels[i] = name;
		value_labels[i] = val;
		row = bgtk_list(c, pair, 2,
				(BGTK_Options){
					.orientation = BGTK_LIST_HORIZONTAL,
					.padding = half,
					.margin = half,
				});
		row_widgets[i] = row;
		rows[i] = row;
	}

	list = bgtk_list(c, rows, N_ROWS,
			 (BGTK_Options){
				 .orientation = BGTK_LIST_VERTICAL,
				 .padding = list_p,
				 .margin = mar,
			 });
	root_list = list;
	/* Temporary size; sys_status_fit_size sets the real one. */
	frame = bgtk_frame(c, list, 1, 1,
			   (BGTK_Options){.padding = pad, .margin = 0});

	/* Prime CPU sample, then full fill (weather may block once). */
	sys_status_refresh(REFRESH_ALL & ~REFRESH_WEATHER);
	sys_status_refresh(REFRESH_FAST | REFRESH_WEATHER);
	sys_status_fit_size(c, frame);
	return frame;
}

#ifndef SYS_STATUS_TEST_MODE

/* Replace the mapped buffer with one of the measured size. */
static int sys_status_apply_size(struct BGTK_Context *c, int conn, int w, int h)
{
	struct BufferRequest req;
	void *nb;
	size_t old_bytes;

	if (!c || w < 1 || h < 1)
		return -1;
	if (w == c->width && h == c->height)
		return 0;

	req.width = (uint32_t)w;
	req.height = (uint32_t)h;
	/* Same calling convention as other BGTK apps (by value). */
	nb = bgce_get_buffer(conn, req);
	if (!nb) {
		bgtk_log("resize get_buffer %dx%d failed", w, h);
		return -1;
	}
	old_bytes = (size_t)c->width * (size_t)c->height * BGCE_BPP;
	if (c->shm_buffer) {
		if (c->buffer_mapped && old_bytes > 0)
			munmap(c->shm_buffer, old_bytes);
		else
			free(c->shm_buffer);
	}
	c->shm_buffer = nb;
	c->buffer_mapped = 1;
	c->width = w;
	c->height = h;
	if (c->root_widget) {
		c->root_widget->w = c->width;
		c->root_widget->h = c->height;
	}
	bgtk_log("window sized to %dx%d", c->width, c->height);
	return 0;
}

int main(void)
{
	int conn;
	struct BufferRequest req;
	void *buf;
	struct BGCEMessage msg;
	struct timeval now, t_clock, t_fast, t_disk, t_weather;
	int need_draw = 1;
	struct BGTK_Widget *root;

	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("sys_status");
	bgtk_log("starting system status");

	conn = bgce_connect();
	if (conn < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return 1;
	}
	bgtk_log("bgce_connect ok fd=%d", conn);
	/* Bootstrap buffer so FreeType is available to measure labels. */
	req.width = BOOT_W;
	req.height = BOOT_H;
	buf = bgce_get_buffer(conn, req);
	if (!buf) {
		bgtk_log("bgce_get_buffer %dx%d failed", BOOT_W, BOOT_H);
		bgce_disconnect(conn);
		return 1;
	}
	bgtk_log("bgce_get_buffer ok %p", buf);
	ctx = bgtk_init(conn, buf, BOOT_W, BOOT_H);
	if (!ctx) {
		bgtk_log("bgtk_init failed — check fonts / FreeType");
		bgce_disconnect(conn);
		return 1;
	}

	root = sys_status_build_ui(ctx);
	ctx->root_widget = root;
	/* Request the measured content size (tight fit). */
	if (sys_status_apply_size(ctx, conn, root->w, root->h) != 0)
		bgtk_log("keeping bootstrap size %dx%d", ctx->width, ctx->height);

	gettimeofday(&now, NULL);
	t_clock = t_fast = t_disk = t_weather = now;
	bgtk_draw_widgets(ctx);

	/* Poll; refresh groups on their own intervals (not everything at 1s). */
	for (;;) {
		fd_set rfds;
		struct timeval tv;
		int r;
		unsigned flags = 0;
		long ms_clock, ms_fast, ms_disk, ms_weather;

		FD_ZERO(&rfds);
		FD_SET(conn, &rfds);
		tv.tv_sec = 0;
		tv.tv_usec = 200000; /* 200ms */
		r = select(conn + 1, &rfds, NULL, NULL, &tv);
		if (r < 0) {
			if (errno == EINTR)
				continue;
			bgtk_log_errno("select");
			break;
		}
		if (r > 0 && FD_ISSET(conn, &rfds)) {
			ssize_t n = bgce_recv_msg(conn, &msg);
			if (n <= 0) {
				bgtk_log("connection closed");
				break;
			}
			if (msg.type == MSG_INPUT_EVENT) {
				struct InputEvent *ev = &msg.data.input_event;
				if (ev->type == EV_REL || ev->type == EV_ABS)
					continue;
				bgtk_update_modifiers(ctx, *ev);
				if (bgtk_is_app_quit_event(ctx, *ev))
					break;
				if (bgtk_handle_input_event(ctx, *ev))
					need_draw = 1;
			} else if (msg.type == MSG_FOCUS_CHANGE) {
				bgtk_set_window_focus(ctx,
						      msg.data.focus_event.state);
			} else if (msg.type == MSG_BUFFER_CHANGE) {
				if (bgtk_handle_buffer_change(
					    ctx, &msg.data.buffer_reply) == 0) {
					if (ctx->root_widget) {
						ctx->root_widget->w = ctx->width;
						ctx->root_widget->h = ctx->height;
					}
					need_draw = 1;
				}
			}
		}

		gettimeofday(&now, NULL);
#define MS_SINCE(t0) \
	((now.tv_sec - (t0).tv_sec) * 1000L + \
	 (now.tv_usec - (t0).tv_usec) / 1000L)
		ms_clock = MS_SINCE(t_clock);
		ms_fast = MS_SINCE(t_fast);
		ms_disk = MS_SINCE(t_disk);
		ms_weather = MS_SINCE(t_weather);
#undef MS_SINCE
		if (ms_clock >= 1000) {
			flags |= REFRESH_CLOCK;
			t_clock = now;
		}
		if (ms_fast >= 2000) {
			flags |= REFRESH_FAST;
			t_fast = now;
		}
		if (ms_disk >= 30000) {
			flags |= REFRESH_DISK | REFRESH_NET;
			t_disk = now;
		}
		if (ms_weather >= 600000) {
			flags |= REFRESH_WEATHER;
			t_weather = now;
		}
		if (flags) {
			sys_status_refresh(flags);
			need_draw = 1;
		}
		if (need_draw) {
			bgtk_draw_widgets(ctx);
			need_draw = 0;
		}
	}

	bgtk_destroy(ctx);
	bgce_disconnect(conn);
	return 0;
}

#endif /* SYS_STATUS_TEST_MODE */
