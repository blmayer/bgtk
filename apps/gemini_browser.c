#include <bgce.h>
#include <bgtk.h>
#include "internal.h"
#include <ctype.h>
#include <errno.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

/*
 * BGTK port of ereandel keybindings (see ../ereandel):
 *   b back · u up path · o open URL · r reload · H home
 *   g go to link # · s save page · m mark · M bookmarks · K unmark · q quit
 * Also: Ctrl+L / Ctrl+R, j/k scroll, Esc blur URL then quit.
 */

static struct BGTK_Context *ctx = NULL;
static struct BGTK_Widget *content_scroll = NULL;
static struct BGTK_Widget *addr_input = NULL;
static struct BGTK_Widget *main_list = NULL;
static struct BGTK_Widget *root_frame = NULL;

static char current_url[512] = "gemini://geminiprotocol.net/";
static const char *homepage = "gemini://geminiprotocol.net/";
/* Retained gemtext body so resize can reflow wrap without re-fetch. */
static char *last_body = NULL;

static struct BGTK_Widget *link_target_widgets[128];
static char link_targets[128][512];
static int num_page_links = 0;

/* Navigation history (ereandel histfile). */
#define HIST_MAX 64
static char hist[HIST_MAX][512];
static int hist_n;

/* g = go-to-link: digits then Enter (or single-digit 1–9). */
static int go_link_mode;
static int go_link_num;

static void content_scroll_invalidate_tmp(void);
static void rebuild_content_from_gemtext(const char *body);
static void load_url(const char *url);
static void navigate(const char *url, int push_hist);

/* Line chrome: modest pad; structure spacing via GEM_V_* on height. */
static BGTK_Options line_opts(void)
{
	return (BGTK_Options){.padding = 2, .margin = 1};
}

/* Extra vertical space (px) around gemtext structure (kept across redraw). */
#define GEM_V_BEFORE_HEADER 28
#define GEM_V_AFTER_HEADER  20
#define GEM_V_AFTER_PARA    22
#define GEM_V_EMPTY_LINE    16

/* Keep content_height in sync with items (scroll keys need this before draw). */
static void gemini_recompute_scroll_height(void)
{
	int cy = 0, i, n;

	if (!content_scroll)
		return;
	n = content_scroll->data.scrollable.widget_count;
	for (i = 0; i < n; i++) {
		struct BGTK_Widget *ch = content_scroll->data.scrollable.items[i];

		if (ch)
			cy += ch->h + 2 * content_scroll->margin;
	}
	content_scroll->data.scrollable.content_height =
		cy + 2 * (content_scroll->margin + content_scroll->padding);
}

static void hist_push(const char *url)
{
	if (!url || !url[0])
		return;
	if (hist_n > 0 && strcmp(hist[hist_n - 1], url) == 0)
		return;
	if (hist_n >= HIST_MAX) {
		memmove(hist[0], hist[1], (HIST_MAX - 1) * sizeof(hist[0]));
		hist_n = HIST_MAX - 1;
	}
	snprintf(hist[hist_n], sizeof(hist[0]), "%s", url);
	hist_n++;
}

/* Pop previous URL into out (history does not include current page). */
static int hist_back(char *out, size_t n)
{
	if (hist_n < 1 || !out || n == 0)
		return -1;
	snprintf(out, n, "%s", hist[--hist_n]);
	return 0;
}

static void config_paths(char *bookmarks, size_t bn, char *certdir, size_t cn)
{
	const char *xdg = getenv("XDG_CONFIG_HOME");
	const char *home = getenv("HOME");
	char base[512];

	if (xdg && xdg[0])
		snprintf(base, sizeof(base), "%s/ereandel", xdg);
	else if (home && home[0])
		snprintf(base, sizeof(base), "%s/.config/ereandel", home);
	else
		snprintf(base, sizeof(base), "/tmp/ereandel");
	mkdir(base, 0755);
	if (bookmarks && bn)
		snprintf(bookmarks, bn, "%s/bookmarks", base);
	if (certdir && cn) {
		snprintf(certdir, cn, "%s/certs", base);
		mkdir(certdir, 0755);
	}
}

/* Strip last path segment: gemini://h/a/b -> gemini://h/a */
static void url_go_up(char *url)
{
	char *scheme, *host_end, *slash;
	size_t len;

	if (!url || !url[0])
		return;
	scheme = strstr(url, "://");
	if (!scheme)
		return;
	host_end = strchr(scheme + 3, '/');
	if (!host_end)
		return;
	len = strlen(url);
	while (len > 1 && url[len - 1] == '/' && &url[len - 1] > host_end) {
		url[--len] = '\0';
	}
	slash = strrchr(url, '/');
	if (!slash || slash <= host_end)
		return;
	if (slash == host_end)
		slash[1] = '\0';
	else
		*slash = '\0';
}

static void navigate(const char *url, int push_hist)
{
	if (!url || !url[0])
		return;
	if (push_hist && current_url[0])
		hist_push(current_url);
	go_link_mode = 0;
	go_link_num = 0;
	load_url(url);
}

static void bookmark_add(void)
{
	char path[512];
	FILE *f;

	config_paths(path, sizeof(path), NULL, 0);
	f = fopen(path, "a");
	if (!f) {
		bgtk_log("bookmark_add: cannot open %s", path);
		return;
	}
	fprintf(f, "%s\n", current_url);
	fclose(f);
	bgtk_log("bookmark added: %s", current_url);
}

static void bookmark_del(void)
{
	char path[512], tmp[520], line[600];
	FILE *in, *out;

	config_paths(path, sizeof(path), NULL, 0);
	snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	in = fopen(path, "r");
	if (!in)
		return;
	out = fopen(tmp, "w");
	if (!out) {
		fclose(in);
		return;
	}
	while (fgets(line, sizeof(line), in)) {
		char *nl = strchr(line, '\n');
		if (nl)
			*nl = '\0';
		if (strncmp(line, current_url, strlen(current_url)) != 0 ||
		    (line[strlen(current_url)] != '\0' &&
		     line[strlen(current_url)] != ' '))
			fprintf(out, "%s\n", line);
	}
	fclose(in);
	fclose(out);
	rename(tmp, path);
	bgtk_log("bookmark removed for %s", current_url);
}

/* Show bookmarks as a synthetic gemtext page (like ereandel M). */
static void bookmark_goto_page(void)
{
	char path[512], line[600], *body = NULL;
	size_t cap = 0, len = 0;
	FILE *f;
	int n = 0;

	config_paths(path, sizeof(path), NULL, 0);
	f = fopen(path, "r");
	body = strdup("# Bookmarks\n\n");
	if (!body)
		return;
	len = strlen(body);
	cap = len + 1;
	if (f) {
		while (fgets(line, sizeof(line), f)) {
			char url[512], *sp, *nl;
			char entry[640];
			size_t el;

			nl = strchr(line, '\n');
			if (nl)
				*nl = '\0';
			if (!line[0])
				continue;
			sp = strchr(line, ' ');
			if (sp) {
				size_t ul = (size_t)(sp - line);
				if (ul >= sizeof(url))
					ul = sizeof(url) - 1;
				memcpy(url, line, ul);
				url[ul] = '\0';
				snprintf(entry, sizeof(entry), "=> %s %s\n", url,
					 sp + 1);
			} else {
				snprintf(entry, sizeof(entry), "=> %s\n", line);
			}
			el = strlen(entry);
			if (len + el + 1 > cap) {
				char *nb = realloc(body, len + el + 256);
				if (!nb)
					break;
				body = nb;
				cap = len + el + 256;
			}
			memcpy(body + len, entry, el + 1);
			len += el;
			n++;
		}
		fclose(f);
	}
	if (n == 0) {
		const char *empty = "No bookmarks yet. Press m to add.\n";
		size_t el = strlen(empty);
		char *nb = realloc(body, len + el + 1);
		if (nb) {
			body = nb;
			memcpy(body + len, empty, el + 1);
		}
	}
	if (current_url[0])
		hist_push(current_url);
	free(last_body);
	last_body = body;
	snprintf(current_url, sizeof(current_url), "about:bookmarks");
	if (addr_input && addr_input->data.text_input.text) {
		free(addr_input->data.text_input.text);
		addr_input->data.text_input.text = strdup(current_url);
		addr_input->data.text_input.cursor_pos =
			(uint32_t)strlen(current_url);
		addr_input->data.text_input.scroll_x = 0;
	}
	rebuild_content_from_gemtext(last_body);
	if (content_scroll)
		bgtk_set_focus(ctx, content_scroll);
	else
		bgtk_draw_widgets(ctx);
}

static void save_page(void)
{
	const char *home = getenv("HOME");
	char path[640];
	FILE *f;
	time_t t = time(NULL);

	if (!last_body) {
		bgtk_log("save_page: no body");
		return;
	}
	if (home && home[0])
		snprintf(path, sizeof(path), "%s/gemini-%ld.gmi", home,
			 (long)t);
	else
		snprintf(path, sizeof(path), "/tmp/gemini-%ld.gmi", (long)t);
	f = fopen(path, "w");
	if (!f) {
		bgtk_log_errno("save_page %s", path);
		return;
	}
	fputs(last_body, f);
	fclose(f);
	bgtk_log("saved page to %s", path);
}

static void go_link_index(int idx)
{
	/* 1-based like ereandel */
	if (idx < 1 || idx > num_page_links)
		return;
	navigate(link_targets[idx - 1], 1);
}

/* Root chrome: [frame_margin][border][padding][content]… (from theme). */
static void gemini_chrome(int *pad, int *fmar, int *bw)
{
	int p = (ctx && ctx->theme.padding > 0) ? ctx->theme.padding : 12;
	int m = (ctx && ctx->theme.frame_margin >= 0) ? ctx->theme.frame_margin
						      : 0;
	int b = ctx ? (int)ctx->theme.frame_border_size : 1;

	if (b < 1)
		b = 1;
	if (pad)
		*pad = p;
	if (fmar)
		*fmar = m;
	if (bw)
		*bw = b;
}

/*
 * Vertical list height overhead beyond sum of children (matches bgtk_list /
 * draw_list): inter-item 2*M*(n-1) + outer 2*(M+P).
 */
static int list_v_overhead(int n, int list_m, int list_p)
{
	if (n < 1)
		return 2 * (list_m + list_p);
	return 2 * list_m * (n - 1) + 2 * (list_m + list_p);
}

/* Size scroll + URL bar so they fit inside the frame content box. */
static void gemini_layout_chrome(void)
{
	int pad, fmar, bw, box_w, box_h, list_m, list_p, field_pad;
	int usable_w, ah, scroll_h, over;

	if (!ctx || !content_scroll || !addr_input)
		return;

	gemini_chrome(&pad, &fmar, &bw);
	/* Inner box the frame assigns to its child (main_list). */
	box_w = ctx->width - 2 * (fmar + bw + pad);
	box_h = ctx->height - 2 * (fmar + bw + pad);
	if (box_w < 80)
		box_w = 80;
	if (box_h < 80)
		box_h = 80;

	/* Page + URL bar fill the frame box; list chrome stays zero. */
	list_m = 0;
	list_p = 0;
	field_pad = pad > 4 ? pad / 2 : 4;

	addr_input->padding = field_pad;
	addr_input->margin = 0;
	content_scroll->padding = field_pad;
	content_scroll->margin = 0;
	if (main_list) {
		main_list->padding = list_p;
		main_list->margin = list_m;
		main_list->w = box_w;
		main_list->h = box_h;
	}
	if (root_frame) {
		root_frame->padding = pad;
		root_frame->margin = fmar;
		root_frame->w = ctx->width;
		root_frame->h = ctx->height;
	}

	usable_w = box_w - 2 * (list_m + list_p);
	if (usable_w < 40)
		usable_w = 40;

	{
		int text_h = ctx->font_size > 0 ? ctx->font_size + 4 : 18;
		addr_input->h = text_h + 2 * (addr_input->padding + addr_input->margin);
		if (addr_input->h < 28)
			addr_input->h = 28;
	}
	ah = addr_input->h;
	over = list_v_overhead(2, list_m, list_p);
	scroll_h = box_h - ah - over;
	if (scroll_h < 40)
		scroll_h = 40;

	content_scroll->w = usable_w;
	content_scroll->h = scroll_h;
	addr_input->w = usable_w;
	content_scroll_invalidate_tmp();
}

/* Resize chrome and reflow wrapped gemtext to the new width. */
static void gemini_on_resize(void)
{
	gemini_layout_chrome();
	if (last_body)
		rebuild_content_from_gemtext(last_body);
	else
		gemini_recompute_scroll_height();
	/* Keep j/k/wheel on the scroll surface after geometry change. */
	if (ctx && content_scroll)
		ctx->focused_widget = content_scroll;
}

/* libtls returns these when the underlying socket would block; they are
 * negative, so a naive `n <= 0` treats them as EOF and aborts the transfer. */
#ifndef TLS_WANT_POLLIN
#define TLS_WANT_POLLIN (-2)
#endif
#ifndef TLS_WANT_POLLOUT
#define TLS_WANT_POLLOUT (-3)
#endif

#define GEMINI_IO_TIMEOUT_MS 15000

static void load_url(const char *url);

/* Retry tls_read/write through WANT_POLLIN/WANT_POLLOUT with a deadline. */
static ssize_t gemini_tls_read(struct tls *t, void *buf, size_t len)
{
	int waited = 0;

	for (;;) {
		ssize_t n = tls_read(t, buf, len);
		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
			if (waited >= GEMINI_IO_TIMEOUT_MS) {
				bgtk_log("tls_read timed out after %dms (want=%zd)",
					 waited, n);
				return -1;
			}
			nanosleep(&ts, NULL);
			waited += 5;
			continue;
		}
		return n;
	}
}

static ssize_t gemini_tls_write(struct tls *t, const void *buf, size_t len)
{
	int waited = 0;
	const char *p = buf;
	size_t left = len;

	while (left > 0) {
		ssize_t n = tls_write(t, p, left);
		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
			if (waited >= GEMINI_IO_TIMEOUT_MS) {
				bgtk_log("tls_write timed out after %dms (want=%zd)",
					 waited, n);
				return -1;
			}
			nanosleep(&ts, NULL);
			waited += 5;
			continue;
		}
		if (n < 0)
			return n;
		if (n == 0)
			return (ssize_t)(len - left);
		p += n;
		left -= (size_t)n;
		waited = 0;
	}
	return (ssize_t)len;
}

/* --- URL resolution (relative links, ../ etc) --- */
static void resolve_url(const char *base, const char *rel, char *out, size_t outlen)
{
	if (!rel || !*rel) {
		strncpy(out, base, outlen - 1);
		out[outlen - 1] = 0;
		return;
	}
	if (strstr(rel, "://") || !strncmp(rel, "gemini:", 7)) {
		strncpy(out, rel, outlen - 1);
		out[outlen - 1] = 0;
		return;
	}

	char bhost[256] = {0};
	char bpath[512] = {0};
	int bport = 1965;

	const char *p = base;
	if (!strncmp(p, "gemini://", 9))
		p += 9;
	const char *he = strchr(p, '/');
	const char *pe = strchr(p, ':');
	if (pe && (!he || pe < he)) {
		int hl = (int)(pe - p);
		if (hl > 255)
			hl = 255;
		memcpy(bhost, p, hl);
		bhost[hl] = 0;
		bport = atoi(pe + 1);
		if (he)
			strncpy(bpath, he, sizeof(bpath) - 1);
		else
			strcpy(bpath, "/");
	} else if (he) {
		int hl = (int)(he - p);
		memcpy(bhost, p, hl);
		bhost[hl] = 0;
		strncpy(bpath, he, sizeof(bpath) - 1);
	} else {
		strncpy(bhost, p, sizeof(bhost) - 1);
		strcpy(bpath, "/");
	}
	if (!bpath[0])
		strcpy(bpath, "/");

	char portpart[32] = "";
	if (bport != 1965)
		snprintf(portpart, sizeof(portpart), ":%d", bport);

	if (rel[0] == '/') {
		snprintf(out, outlen, "gemini://%s%s%s", bhost, portpart, rel);
		return;
	}

	/* relative: dirname(base) + rel, with ../ handling */
	char dir[512];
	strncpy(dir, bpath, sizeof(dir) - 1);
	char *ls = strrchr(dir, '/');
	if (ls)
		*(ls + 1) = 0;
	else
		strcpy(dir, "/");

	char full[1024];
	snprintf(full, sizeof(full), "%s%s", dir, rel);

	/* normalize segments */
	char *segs[64];
	int nseg = 0;
	char *tmp = strdup(full);
	char *save = NULL;
	char *seg = strtok_r(tmp, "/", &save);
	while (seg) {
		if (!strcmp(seg, ".") || !*seg) {
			/* skip */
		} else if (!strcmp(seg, "..")) {
			if (nseg > 0)
				nseg--;
		} else {
			segs[nseg++] = seg;
		}
		seg = strtok_r(NULL, "/", &save);
	}

	char norm[1024] = "/";
	for (int i = 0; i < nseg; i++) {
		strcat(norm, segs[i]);
		if (i + 1 < nseg)
			strcat(norm, "/");
	}
	free(tmp);

	snprintf(out, outlen, "gemini://%s%s%s", bhost, portpart, norm);
}

/* --- Gemini fetch over TLS --- */
static int fetch_gemini(const char *req_url, int *out_status, char **out_meta,
			char **out_body)
{
	char host[256] = {0};
	char path[768] = "/";
	int port = 1965;
	char selector[1024];
	char portstr[16];
	char req[1200];
	int rlen;
	struct tls_config *cfg;
	struct tls *ctx_tls;
	char hbuf[1024];
	int hlen = 0;
	int status = 0;
	char meta[512] = {0};
	size_t cap = 8192;
	size_t blen = 0;
	char *body;
	const char *p;
	const char *he;
	const char *pe;

	*out_status = 0;
	*out_meta = NULL;
	*out_body = NULL;

	if (!req_url || !*req_url) {
		bgtk_log("fetch: empty url");
		return -1;
	}

	p = req_url;
	if (!strncmp(p, "gemini://", 9))
		p += 9;
	he = strchr(p, '/');
	pe = strchr(p, ':');
	if (pe && (!he || pe < he)) {
		int hl = (int)(pe - p);
		if (hl > 255)
			hl = 255;
		memcpy(host, p, (size_t)hl);
		host[hl] = 0;
		port = atoi(pe + 1);
		if (he)
			strncpy(path, he, sizeof(path) - 1);
	} else if (he) {
		int hl = (int)(he - p);
		memcpy(host, p, (size_t)hl);
		host[hl] = 0;
		strncpy(path, he, sizeof(path) - 1);
	} else {
		strncpy(host, p, sizeof(host) - 1);
	}
	if (!host[0]) {
		bgtk_log("fetch: no host in '%s'", req_url);
		return -1;
	}
	if (port < 1)
		port = 1965;
	if (!path[0])
		strcpy(path, "/");

	/* full selector is the original req_url (spec) */
	strncpy(selector, req_url, sizeof(selector) - 1);
	selector[sizeof(selector) - 1] = '\0';

	bgtk_log("fetch: begin host=%s port=%d path=%s", host, port, path);

	/* Gemini commonly uses TOFU; verify-none matches prior behaviour. */
	if (tls_init() == -1) {
		bgtk_log("fetch: tls_init failed");
		return -2;
	}

	cfg = tls_config_new();
	if (!cfg) {
		bgtk_log("fetch: tls_config_new failed");
		return -2;
	}
	tls_config_insecure_noverifycert(cfg);
	tls_config_insecure_noverifyname(cfg);

	ctx_tls = tls_client();
	if (!ctx_tls) {
		bgtk_log("fetch: tls_client failed");
		tls_config_free(cfg);
		return -2;
	}
	if (tls_configure(ctx_tls, cfg) == -1) {
		bgtk_log("fetch: tls_configure: %s", tls_error(ctx_tls));
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -2;
	}

	snprintf(portstr, sizeof(portstr), "%d", port);
	if (tls_connect(ctx_tls, host, portstr) == -1) {
		bgtk_log("fetch: tls_connect %s:%s: %s", host, portstr,
			 tls_error(ctx_tls));
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -6;
	}
	bgtk_log("fetch: connected to %s:%s", host, portstr);

	rlen = snprintf(req, sizeof(req), "%s\r\n", selector);
	if (gemini_tls_write(ctx_tls, req, (size_t)rlen) < 0) {
		bgtk_log("fetch: tls_write: %s", tls_error(ctx_tls));
		tls_close(ctx_tls);
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -7;
	}

	/* Read header up to first \r\n (must retry TLS_WANT_*). */
	memset(hbuf, 0, sizeof(hbuf));
	while (hlen < (int)sizeof(hbuf) - 1) {
		ssize_t n = gemini_tls_read(ctx_tls, hbuf + hlen, 1);
		if (n < 0) {
			bgtk_log("fetch: header read error n=%zd err=%s", n,
				 tls_error(ctx_tls) ? tls_error(ctx_tls) : "?");
			break;
		}
		if (n == 0) {
			bgtk_log("fetch: header EOF at hlen=%d", hlen);
			break;
		}
		hlen += (int)n;
		if (hlen >= 2 && hbuf[hlen - 2] == '\r' &&
		    hbuf[hlen - 1] == '\n')
			break;
	}
	hbuf[hlen] = 0;
	bgtk_log("fetch: header (%d bytes): %.80s", hlen, hbuf);

	if (sscanf(hbuf, "%d %511[^\r\n]", &status, meta) < 1) {
		bgtk_log("fetch: bad header, treating as 59");
		status = 59;
		snprintf(meta, sizeof(meta), "bad response header");
	}
	*out_status = status;
	if (out_meta)
		*out_meta = strdup(meta);

	body = malloc(cap);
	if (!body) {
		bgtk_log("fetch: OOM body buffer");
		tls_close(ctx_tls);
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -8;
	}
	for (;;) {
		char rbuf[2048];
		ssize_t n = gemini_tls_read(ctx_tls, rbuf, sizeof(rbuf));
		if (n < 0) {
			bgtk_log("fetch: body read error n=%zd err=%s (have %zu)",
				 n,
				 tls_error(ctx_tls) ? tls_error(ctx_tls) : "?",
				 blen);
			break;
		}
		if (n == 0)
			break;
		if (blen + (size_t)n + 1 > cap) {
			cap = cap * 2 + (size_t)n;
			char *nb = realloc(body, cap);
			if (!nb) {
				bgtk_log("fetch: OOM growing body to %zu", cap);
				free(body);
				tls_close(ctx_tls);
				tls_free(ctx_tls);
				tls_config_free(cfg);
				return -8;
			}
			body = nb;
		}
		memcpy(body + blen, rbuf, (size_t)n);
		blen += (size_t)n;
	}
	body[blen] = 0;
	*out_body = body;

	tls_close(ctx_tls);
	tls_free(ctx_tls);
	tls_config_free(cfg);
	bgtk_log("fetch: done status=%d meta='%.60s' body_len=%zu", status,
		 meta, blen);
	return 0;
}

/* Content line click: open link, else focus scroll (so j/k/wheel keep working). */
static int gemini_line_handler(struct BGTK_Widget *w, struct InputEvent ev)
{
	if (ev.code != BTN_LEFT || ev.value != 1)
		return 0;
	for (int i = 0; i < num_page_links; i++) {
		if (link_target_widgets[i] == w) {
			navigate(link_targets[i], 1);
			return 1;
		}
	}
	if (w && w->ctx && content_scroll)
		bgtk_set_focus(w->ctx, content_scroll);
	return 1;
}

/* Simple word-wrap for long paragraphs and link labels. Returns malloc'ed array of lines (caller frees). */
static char **wrap_text(FT_Face face, const char *text, int max_width, int *nlines)
{
	char **lines = NULL;
	int n = 0, cap = 0;
	int len;
	int i = 0;

	*nlines = 0;
	if (!face || !text || !*text || max_width < 20)
		return NULL;
	len = (int)strlen(text);
	while (i < len) {
		int j = i;
		int last_space = -1;
		int w = 0;

		while (j < len) {
			const char *cp = text + j;
			size_t left = (size_t)(len - j);
			const char *start = cp;
			uint32_t ch = bgtk_utf8_next_n(&cp, &left);
			int nbytes = (int)(cp - start);

			if (nbytes < 1)
				break;
			if (FT_Load_Char(face, (FT_ULong)ch, FT_LOAD_DEFAULT) == 0)
				w += face->glyph->advance.x >> 6;
			if (w > max_width && j > i)
				break;
			/* Space for break: only ASCII whitespace (common gemtext). */
			if (ch < 128 && isspace((unsigned char)ch))
				last_space = j;
			j += nbytes;
		}
		{
			int end = (j >= len) ? len : (last_space > i ? last_space : j);
			int lnlen = end - i;
			char *ln = malloc((size_t)lnlen + 1);

			if (!ln)
				break;
			memcpy(ln, text + i, (size_t)lnlen);
			ln[lnlen] = 0;
			while (lnlen > 0 &&
			       isspace((unsigned char)ln[lnlen - 1]))
				ln[--lnlen] = 0;
			if (n >= cap) {
				cap = cap ? cap * 2 : 8;
				lines = realloc(lines, (size_t)cap * sizeof(char *));
				if (!lines)
					break;
			}
			lines[n++] = ln;
			i = end;
			while (i < len && isspace((unsigned char)text[i]))
				i++;
		}
	}
	*nlines = n;
	return lines;
}

/* Drop offscreen scroll buffer so the next draw reallocates for new height. */
static void content_scroll_invalidate_tmp(void)
{
	if (!content_scroll)
		return;
	if (content_scroll->data.scrollable.tmp) {
		free(content_scroll->data.scrollable.tmp);
		content_scroll->data.scrollable.tmp = NULL;
	}
	content_scroll->data.scrollable.widget_capacity = 0;
}

/* replace page content from gemtext body (uses current content_scroll->w for wrap) */
static void rebuild_content_from_gemtext(const char *body)
{
	int max_text_w;
	int line_inset;

	/* free old line widgets (all plain text) */
	if (content_scroll->data.scrollable.items) {
		for (int i = 0; i < content_scroll->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *tw = content_scroll->data.scrollable.items[i];
			if (tw) {
				free(tw->data.text.text);
				free(tw);
			}
		}
		free(content_scroll->data.scrollable.items);
		content_scroll->data.scrollable.items = NULL;
		content_scroll->data.scrollable.widget_count = 0;
	}
	content_scroll_invalidate_tmp();
	num_page_links = 0;

	if (!body)
		return;

	line_inset = 2 * (1 + 1); /* line pad+margin */
	max_text_w = 480;
	if (content_scroll && content_scroll->w > 100) {
		max_text_w = content_scroll->w - 2 * content_scroll->padding -
			     line_inset - 4;
		if (max_text_w < 40)
			max_text_w = 40;
	}

	/* copy for strtok */
	char *work = strdup(body);
	if (!work)
		return;

	/* collect new widgets */
	struct BGTK_Widget **new_items = NULL;
	int cap = 0;
	int cnt = 0;

	char *save = NULL;
	char *line = strtok_r(work, "\n", &save);
	int in_pre = 0;
	while (line) {
		/* strip \r */
		char *cr = strchr(line, '\r');
		if (cr)
			*cr = 0;

		if (!strncmp(line, "```", 3)) {
			in_pre = !in_pre;
			line = strtok_r(NULL, "\n", &save);
			continue;
		}

		char vis[768];
		int is_link = 0;
		char link_to[512] = {0};
		int header_level = 0;
		int item_color = 0;

		if (in_pre) {
			snprintf(vis, sizeof(vis), "  %s", line);
		} else if (!strncmp(line, "=>", 2)) {
			/* link */
			char tgt[512] = {0};
			char disp[512] = {0};
			const char *rest = line + 2;
			while (*rest == ' ' || *rest == '\t')
				rest++;
			const char *sp = rest;
			while (*sp && *sp != ' ' && *sp != '\t')
				sp++;
			int tl = (int)(sp - rest);
			if (tl > 0 && tl < (int)sizeof(tgt))
				memcpy(tgt, rest, tl);
			tgt[tl] = 0;
			while (*sp == ' ' || *sp == '\t')
				sp++;
			if (*sp)
				strncpy(disp, sp, sizeof(disp) - 1);
			else
				strncpy(disp, tgt, sizeof(disp) - 1);

			/* ereandel-style label; g+N still uses link index. */
			snprintf(vis, sizeof(vis), "=> %s", disp);
			strncpy(link_to, tgt, sizeof(link_to) - 1);
			is_link = 1;
			item_color = 10;
		} else if (!strncmp(line, "# ", 2)) {
			snprintf(vis, sizeof(vis), "%s", line + 2);
			header_level = 1;
		} else if (!strncmp(line, "## ", 3)) {
			snprintf(vis, sizeof(vis), "%s", line + 3);
			header_level = 2;
		} else if (!strncmp(line, "### ", 4)) {
			snprintf(vis, sizeof(vis), "%s", line + 4);
			header_level = 3;
		} else if (!strncmp(line, "> ", 2)) {
			snprintf(vis, sizeof(vis), "%s", line);
		} else if (!strncmp(line, "* ", 2)) {
			/* U+2022 bullet (UTF-8); draw_text is codepoint-aware. */
			snprintf(vis, sizeof(vis), "\xe2\x80\xa2 %s", line + 2);
			item_color = 10;
		} else {
			snprintf(vis, sizeof(vis), "%s", line);
		}

		char resolved[512] = {0};
		if (is_link)
			resolve_url(current_url, link_to, resolved, sizeof(resolved));

		if (header_level > 0 && cnt > 0)
			new_items[cnt - 1]->h += GEM_V_BEFORE_HEADER;

		if (header_level > 0 && ctx->ft_face) {
			FT_Set_Pixel_Sizes(ctx->ft_face, 0,
					   ctx->font_size + 4 - header_level);
		}
		if (!in_pre) {
			int nsubs = 0;
			char **subs = wrap_text(ctx->ft_face, vis, max_text_w,
						&nsubs);
			/* No wrap (null face / empty): still show one line. */
			if (!subs || nsubs < 1) {
				struct BGTK_Widget *tw =
					bgtk_text(ctx, vis, line_opts());
				if (tw) {
					if (header_level > 0)
						tw->data.text.header_level =
							header_level;
					if (item_color)
						tw->data.text.header_level =
							item_color;
					if (is_link && num_page_links < 128) {
						link_target_widgets[num_page_links] =
							tw;
						strncpy(link_targets[num_page_links],
							resolved,
							sizeof(link_targets[0]) -
								1);
						num_page_links++;
					}
					tw->handle_event = gemini_line_handler;
					if (header_level > 0)
						tw->h += GEM_V_AFTER_HEADER;
					else if (!vis[0])
						tw->h += GEM_V_EMPTY_LINE;
					else
						tw->h += GEM_V_AFTER_PARA / 2;
					if (cnt >= cap) {
						cap = cap ? cap * 2 : 32;
						new_items = realloc(
							new_items,
							(size_t)cap *
								sizeof(*new_items));
					}
					if (new_items)
						new_items[cnt++] = tw;
				}
				if (subs) {
					for (int s = 0; s < nsubs; s++)
						free(subs[s]);
					free(subs);
				}
				if (header_level > 0 && ctx->ft_face)
					FT_Set_Pixel_Sizes(ctx->ft_face, 0,
							   ctx->font_size);
				line = strtok_r(NULL, "\n", &save);
				continue;
			}
			for (int s = 0; s < nsubs; s++) {
				struct BGTK_Widget *tw = bgtk_text(ctx, subs[s], line_opts());
				if (!tw) continue;
				if (header_level > 0) {
					tw->data.text.header_level = header_level;
				}
				if (item_color) {
					tw->data.text.header_level = item_color;
				}
				if (is_link && num_page_links < 128) {
					link_target_widgets[num_page_links] = tw;
					strncpy(link_targets[num_page_links], resolved, sizeof(link_targets[0]) - 1);
					num_page_links++;
				}
				tw->handle_event = gemini_line_handler;
				if (s < nsubs - 1) {
					/* tighter leading between wraps of same block */
					if (tw->h > 4)
						tw->h -= 2;
				} else if (header_level > 0) {
					tw->h += GEM_V_AFTER_HEADER;
				} else {
					tw->h += GEM_V_AFTER_PARA;
				}
				if (cnt >= cap) {
					cap = cap ? cap * 2 : 32;
					struct BGTK_Widget **ni = realloc(new_items, (size_t)cap * sizeof(*ni));
					if (!ni) {
						free(tw->data.text.text);
						free(tw);
						break;
					}
					new_items = ni;
				}
				new_items[cnt++] = tw;
			}
			for (int s = 0; s < nsubs; s++) free(subs[s]);
			free(subs);
		} else {
			struct BGTK_Widget *tw = bgtk_text(ctx, vis, line_opts());
			if (tw) {
				tw->h += 4;  /* space after pre block */
				tw->handle_event = gemini_line_handler;
				if (cnt >= cap) {
					cap = cap ? cap * 2 : 32;
					struct BGTK_Widget **ni = realloc(new_items, (size_t)cap * sizeof(*ni));
					if (!ni) {
						free(tw->data.text.text);
						free(tw);
					} else {
						new_items = ni;
						new_items[cnt++] = tw;
					}
				} else {
					new_items[cnt++] = tw;
				}
			}
		}
		if (header_level > 0 && ctx->ft_face) {
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
		}

		line = strtok_r(NULL, "\n", &save);
	}
	free(work);

	content_scroll->data.scrollable.items = new_items;
	content_scroll->data.scrollable.widget_count = cnt;
	content_scroll->data.scrollable.scroll_y = 0;
	content_scroll_invalidate_tmp();
	gemini_recompute_scroll_height();
	bgtk_log("rebuild: %d line widgets, %d links, content_h=%d", cnt,
		 num_page_links,
		 content_scroll->data.scrollable.content_height);
}

/* main navigation entry */
static void load_url(const char *url)
{
	char tourl[512];
	int redirects = 0;
	int status = 0;
	char *meta = NULL;
	char *body = NULL;
	int feres = -1;

	if (!url || !*url) {
		bgtk_log("load_url: empty url");
		return;
	}
	if (!ctx || !content_scroll || !addr_input) {
		bgtk_log("load_url: UI not ready (ctx=%p scroll=%p addr=%p)",
			 (void *)ctx, (void *)content_scroll, (void *)addr_input);
		return;
	}

	bgtk_log("load_url: %s", url);
	strncpy(current_url, url, sizeof(current_url) - 1);
	current_url[sizeof(current_url) - 1] = 0;

	/* follow redirects a few times */
	strncpy(tourl, url, sizeof(tourl) - 1);
	tourl[sizeof(tourl) - 1] = 0;

	for (;;) {
		if (meta) {
			free(meta);
			meta = NULL;
		}
		if (body) {
			free(body);
			body = NULL;
		}
		feres = fetch_gemini(tourl, &status, &meta, &body);
		bgtk_log("load_url: fetch res=%d status=%d redirect=%d", feres,
			 status, redirects);
		if (feres == 0 && (status == 30 || status == 31) && meta &&
		    *meta && redirects < 5) {
			char next[512];
			resolve_url(tourl, meta, next, sizeof(next));
			bgtk_log("load_url: redirect %d -> %s", status, next);
			strncpy(tourl, next, sizeof(tourl) - 1);
			tourl[sizeof(tourl) - 1] = 0;
			redirects++;
			continue;
		}
		break;
	}

	strncpy(current_url, tourl, sizeof(current_url) - 1);
	current_url[sizeof(current_url) - 1] = 0;

	/* free previous page lines */
	if (content_scroll->data.scrollable.items) {
		for (int i = 0;
		     i < content_scroll->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *tw =
				content_scroll->data.scrollable.items[i];
			if (!tw)
				continue;
			if (tw->type == BGTK_WIDGET_TEXT && tw->data.text.text)
				free(tw->data.text.text);
			free(tw);
		}
		free(content_scroll->data.scrollable.items);
		content_scroll->data.scrollable.items = NULL;
		content_scroll->data.scrollable.widget_count = 0;
	}
	content_scroll_invalidate_tmp();
	num_page_links = 0;

	if (feres != 0 || status / 10 != 2) {
		char errline[256];
		snprintf(errline, sizeof(errline), "Error %d (%d) %s", status,
			 feres, meta ? meta : "");
		bgtk_log("load_url: showing error page: %s", errline);
		struct BGTK_Widget *et =
			bgtk_text(ctx, errline, line_opts());
		struct BGTK_Widget **errs = calloc(1, sizeof(*errs));
		if (errs)
			errs[0] = et;
		content_scroll->data.scrollable.items = errs;
		content_scroll->data.scrollable.widget_count = errs ? 1 : 0;
		content_scroll->data.scrollable.scroll_y = 0;
		content_scroll_invalidate_tmp();
		gemini_recompute_scroll_height();
		/* Drop retained body so resize does not reflow a prior page. */
		free(last_body);
		last_body = NULL;
		if (body) {
			free(body);
			body = NULL;
		}
	} else {
		bgtk_log("load_url: rendering success body (%zu bytes)",
			 body ? strlen(body) : 0);
		free(last_body);
		/* Keep body for wrap reflow on MSG_BUFFER_CHANGE. */
		last_body = body;
		body = NULL;
		rebuild_content_from_gemtext(last_body);
	}

	/* sync address bar (it serves as the URL display) */
	free(addr_input->data.text_input.text);
	addr_input->data.text_input.text = strdup(current_url);
	if (!addr_input->data.text_input.text)
		addr_input->data.text_input.text = strdup("");
	addr_input->data.text_input.cursor_pos =
		(uint32_t)strlen(addr_input->data.text_input.text
					 ? addr_input->data.text_input.text
					 : "");
	addr_input->data.text_input.scroll_x = 0;

	if (meta)
		free(meta);
	if (body)
		free(body);

	/*
	 * Content focus so j/k/Page* scroll work after every navigation.
	 * Ctrl+L returns focus to the URL field.
	 */
	if (content_scroll)
		bgtk_set_focus(ctx, content_scroll);
	else
		bgtk_draw_widgets(ctx);
	bgtk_log("load_url: draw complete for %s focus=content", current_url);
}

static void addr_on_enter(void)
{
	char url[512];

	if (!addr_input || !addr_input->data.text_input.text)
		return;
	snprintf(url, sizeof(url), "%s", addr_input->data.text_input.text);
	/* Convenience: allow host/path without scheme (ereandel). */
	if (strncmp(url, "gemini://", 9) != 0 && strncmp(url, "about:", 6) != 0 &&
	    url[0]) {
		char full[512];
		snprintf(full, sizeof(full), "gemini://%s", url);
		navigate(full, 1);
		return;
	}
	navigate(url, 1);
}

int main(void)
{
	int width = 640;
	int height = 480;
	int conn_fd;
	struct BufferRequest req;
	void *buffer;
	struct BGTK_Widget *main_items[2];
	struct BGTK_Widget *frame;
	struct BGTK_Widget *loading;
	struct BGTK_Widget **loading_items;
	struct BGCEMessage msg;
	ssize_t bytes;
	const char *quit_reason = "unknown";

	/* File log under ~/.cache/bgtk/gemini_browser.log (not stderr — launcher
	 * often redirects stdio to /dev/null). */
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("gemini_browser");
	signal(SIGPIPE, SIG_IGN);

	bgtk_log("gemini_browser starting pid=%ld", (long)getpid());

	if (tls_init() == -1) {
		bgtk_log("tls_init failed — is libtls/libretls installed?");
		return 1;
	}
	bgtk_log("tls_init ok");

	conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect failed (is bgce running?)");
		return -1;
	}
	bgtk_log("bgce_connect ok fd=%d", conn_fd);

	req.width = width;
	req.height = height;
	buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return -3;
	}
	bgtk_log("got buffer %dx%d", width, height);

	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed");
		bgce_disconnect(conn_fd);
		return 1;
	}
	bgtk_log("bgtk_init ok face=%p mono=%p serif=%p",
		 (void *)ctx->ft_face, (void *)ctx->ft_face_mono,
		 (void *)ctx->ft_face_serif);

	{
		int pad, fmar, bw;

		gemini_chrome(&pad, &fmar, &bw);

		/* Build shell; gemini_layout_chrome sizes children to the frame box. */
		addr_input = bgtk_text_input(ctx, current_url, 200, 0,
					     (BGTK_Options){
						     .padding = pad > 4 ? pad / 2 : 4,
						     .margin = 0});
		addr_input->data.text_input.on_enter = addr_on_enter;

		content_scroll = bgtk_scrollable(
			ctx, NULL, 0,
			(BGTK_Options){.padding = pad > 4 ? pad / 2 : 4,
				       .margin = 0});

		loading = bgtk_text(ctx, "Loading…", line_opts());
		loading_items = calloc(1, sizeof(*loading_items));
		if (loading_items) {
			loading_items[0] = loading;
			content_scroll->data.scrollable.items = loading_items;
			content_scroll->data.scrollable.widget_count = 1;
		}

		main_items[0] = content_scroll;
		main_items[1] = addr_input;
		main_list = bgtk_list(ctx, main_items, 2,
				      (BGTK_Options){
					      .orientation = BGTK_LIST_VERTICAL,
					      .padding = 0,
					      .margin = 0});

		frame = bgtk_frame(ctx, main_list, width, height,
				   (BGTK_Options){.padding = pad,
						  .margin = fmar});
		root_frame = frame;
		gemini_layout_chrome();
	}

	ctx->root_widget = frame;
	/* Content focused so j/k/Page* scroll work; Ctrl+L focuses the URL. */
	bgtk_set_focus(ctx, content_scroll);
	bgtk_draw_widgets(ctx);
	bgtk_log("shell drawn; loading initial url %s", current_url);

	/* Network fetch (may take a few seconds). */
	load_url(current_url);

	bgtk_log("entering main loop (%dx%d) url=%s", width, height,
		 current_url);

	while (1) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0) {
				quit_reason = "server closed connection (recv=0)";
				bgtk_log("%s", quit_reason);
			} else if (errno == EINTR) {
				bgtk_log("bgce_recv_msg interrupted, retrying");
				continue;
			} else {
				bgtk_log_errno("bgce_recv_msg failed (recv=%zd)",
					       bytes);
				quit_reason = "bgce_recv_msg error";
			}
			break;
		}

		int res = 0;
		switch (msg.type) {
		case MSG_INPUT_EVENT: {
			struct InputEvent *ev = &msg.data.input_event;

			/* Ignore pure pointer motion (not wheel). Wheel is
			 * EV_REL/REL_WHEEL and must reach the scrollable. */
			if (ev->type == EV_ABS)
				break;
			if (ev->type == EV_REL && ev->code != REL_WHEEL)
				break;

			bgtk_update_modifiers(ctx, *ev);

			/*
			 * Ereandel-style keys when content is focused (not
			 * typing in the URL bar). Ctrl chords always work.
			 */
			if (ev->type == EV_KEY &&
			    (ev->value == 1 || ev->value == 2)) {
				int addr_focus =
					(ctx->focused_widget == addr_input);
				int mods = bgtk_mods_from_ctx(ctx);
				int scroll_key = 0;
				char prev[512];

				/* Ctrl+L / o: open (focus URL). */
				if (((mods & BGTK_MOD_CTRL) &&
				     ev->code == KEY_L) ||
				    (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
				     !(mods & BGTK_MOD_SHIFT) &&
				     ev->code == KEY_O)) {
					go_link_mode = 0;
					bgtk_set_focus(ctx, addr_input);
					res = 1;
					break;
				}
				/* Ctrl+R / r: reload. */
				if (((mods & BGTK_MOD_CTRL) &&
				     ev->code == KEY_R) ||
				    (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
				     !(mods & BGTK_MOD_SHIFT) &&
				     ev->code == KEY_R)) {
					go_link_mode = 0;
					load_url(current_url);
					res = 1;
					break;
				}
				/* Esc: cancel go-mode, blur URL, or quit. */
				if (ev->code == KEY_ESC) {
					if (go_link_mode) {
						go_link_mode = 0;
						go_link_num = 0;
						res = 1;
						break;
					}
					if (addr_focus) {
						bgtk_set_focus(ctx,
							       content_scroll);
						res = 1;
						break;
					}
					quit_reason = "Esc";
					goto done;
				}
				/* Ctrl+C / q: quit. */
				if (((mods & BGTK_MOD_CTRL) &&
				     ev->code == KEY_C) ||
				    (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
				     ev->code == KEY_Q)) {
					quit_reason = "quit key";
					goto done;
				}

				/* go-link mode: type number, Enter to open. */
				if (go_link_mode && !addr_focus) {
					if (ev->code >= KEY_1 &&
					    ev->code <= KEY_0) {
						int d = (ev->code == KEY_0)
								? 0
								: (int)(ev->code -
									KEY_1 +
									1);
						go_link_num =
							go_link_num * 10 + d;
						res = 1;
						break;
					}
					if (ev->code == KEY_ENTER ||
					    ev->code == KEY_KPENTER) {
						int n = go_link_num;
						go_link_mode = 0;
						go_link_num = 0;
						if (n > 0)
							go_link_index(n);
						res = 1;
						break;
					}
					if (ev->code == KEY_BACKSPACE) {
						go_link_num /= 10;
						res = 1;
						break;
					}
				}

				/* Ereandel single-letter keys (content only). */
				if (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
				    !(mods & BGTK_MOD_ALT)) {
					if (ev->code == KEY_B) {
						if (hist_back(prev,
							      sizeof(prev)) == 0)
							navigate(prev, 0);
						res = 1;
						break;
					}
					if (ev->code == KEY_U) {
						char u[512];
						snprintf(u, sizeof(u), "%s",
							 current_url);
						url_go_up(u);
						if (strcmp(u, current_url) != 0)
							navigate(u, 1);
						res = 1;
						break;
					}
					if (ev->code == KEY_H &&
					    (mods & BGTK_MOD_SHIFT)) {
						navigate(homepage, 1);
						res = 1;
						break;
					}
					if (ev->code == KEY_G &&
					    !(mods & BGTK_MOD_SHIFT)) {
						go_link_mode = 1;
						go_link_num = 0;
						bgtk_log("go-link mode (type #)");
						res = 1;
						break;
					}
					if (ev->code == KEY_S &&
					    !(mods & BGTK_MOD_SHIFT)) {
						save_page();
						res = 1;
						break;
					}
					if (ev->code == KEY_M &&
					    !(mods & BGTK_MOD_SHIFT)) {
						bookmark_add();
						res = 1;
						break;
					}
					if (ev->code == KEY_M &&
					    (mods & BGTK_MOD_SHIFT)) {
						bookmark_goto_page();
						res = 1;
						break;
					}
					if (ev->code == KEY_K &&
					    (mods & BGTK_MOD_SHIFT)) {
						bookmark_del();
						res = 1;
						break;
					}
				}

				/*
				 * Scroll: PageUp/Down always; j/k/arrows when
				 * not editing URL.
				 */
				if (ev->code == KEY_PAGEUP ||
				    ev->code == KEY_PAGEDOWN)
					scroll_key = 1;
				else if (!addr_focus && !go_link_mode &&
					 (ev->code == KEY_UP ||
					  ev->code == KEY_DOWN ||
					  ev->code == KEY_HOME ||
					  ev->code == KEY_END ||
					  ev->code == KEY_SPACE ||
					  ev->code == KEY_J ||
					  ev->code == KEY_K))
					scroll_key = 1;

				if (scroll_key && content_scroll &&
				    content_scroll->handle_event) {
					gemini_recompute_scroll_height();
					if (content_scroll->handle_event(
						    content_scroll, *ev)) {
						res = 1;
						break;
					}
				}
			}

			res = bgtk_handle_input_event(ctx, *ev);
			break;
		}
		case MSG_FOCUS_CHANGE:
			bgtk_log("focus change state=%d",
				 msg.data.focus_event.state);
			bgtk_set_window_focus(ctx, msg.data.focus_event.state);
			break;
		case MSG_BUFFER_CHANGE:
			bgtk_log("buffer change %ux%u",
				 msg.data.buffer_reply.width,
				 msg.data.buffer_reply.height);
			if (bgtk_handle_buffer_change(
				    ctx, &msg.data.buffer_reply) == 0) {
				/* Refit chrome and reflow gemtext wrap width. */
				gemini_on_resize();
				bgtk_draw_widgets(ctx);
			}
			break;
		default:
			bgtk_log("unhandled msg type=%d bytes=%zd", msg.type,
				 bytes);
			break;
		}
		if (res)
			bgtk_draw_widgets(ctx);
	}
done:
	bgtk_log("shutting down reason='%s' url='%s'", quit_reason,
		 current_url);
	bgtk_destroy(ctx);
	bgtk_log("exit 0");
	return 0;
}
