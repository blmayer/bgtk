/*
 * Labyrinth — BGTK web browser (HTML shell).
 *
 * Layout mirrors gemini_browser: content area on top, URL bar at the bottom.
 * Documents are rendered with bgtk_html_parse_inline() (libxml2 → widgets).
 *
 * Document pipeline (current vs planned):
 *
 *   [fetch]  about: | file: | http: | https: (libtls)
 *      ↓
 *   [decode] charset (placeholder: assume UTF-8)
 *      ↓
 *   [css]    <style> + style="" → cascade (css.c)     ← v1 implemented
 *      ↓
 *   [html]   bgtk_html_parse_inline → widget tree     ← implemented
 *      ↓
 *   [style]  applied during HTML convert via css.c
 *      ↓
 *   [js]     run scripts / bind events                (placeholder)
 *      ↓
 *   [wire]   <a href> on text widgets → navigate()  ← implemented
 *
 * Keys (gemini-like): Ctrl+L focus URL · Ctrl+R reload · b back ·
 *   H home · j/k/Page* scroll · Esc blur URL then quit · q quit.
 *
 * Build: make labyrinth
 * Test:  make test_labyrinth && ./test_labyrinth
 */

#include <bgce.h>
#include <bgtk.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

#include "html.h"
#include "internal.h"

#ifndef TLS_WANT_POLLIN
#define TLS_WANT_POLLIN (-2)
#endif
#ifndef TLS_WANT_POLLOUT
#define TLS_WANT_POLLOUT (-3)
#endif

#define LAB_IO_TIMEOUT_MS 20000

/* ------------------------------------------------------------------ */
/* State                                                              */
/* ------------------------------------------------------------------ */

static struct BGTK_Context *ctx;
static struct BGTK_Widget *root_frame;
static struct BGTK_Widget *main_list;
static struct BGTK_Widget *content_host; /* borderless frame → page */
static struct BGTK_Widget *addr_input;

static char current_url[768] = "about:home";
static char *last_html; /* retained for resize reflow */
static int content_w = 400, content_h = 300;

#define HIST_MAX 64
static char hist[HIST_MAX][768];
static int hist_n;

/* ------------------------------------------------------------------ */
/* CSS / JS / link placeholders                                       */
/* ------------------------------------------------------------------ */

/*
 * Future stylesheet representation. Linked and embedded CSS would land
 * here before style application walks the widget tree.
 */
struct Labyrinth_StyleSheet {
	char *origin; /* URL or "inline" */
	char *source; /* raw CSS text */
	/* parsed rules, specificity tables, … */
};

/*
 * Future JS runtime hook. Scripts would be collected during parse and
 * executed against a document/window model bound to the widget tree.
 */
struct Labyrinth_Script {
	char *src_url; /* external, or NULL for inline */
	char *source;
	int deferred;
};

/* Counts found during load — useful when wiring starts. */
static int last_style_blocks;
static int last_script_blocks;
static int last_anchor_count;

/* Scan HTML for future pipeline stages (no side effects yet). */
static void labyrinth_scan_pipeline(const char *html)
{
	const char *p;

	last_style_blocks = last_script_blocks = last_anchor_count = 0;
	if (!html)
		return;
	for (p = html; *p; p++) {
		if (!strncmp(p, "<style", 6) || !strncmp(p, "<STYLE", 6))
			last_style_blocks++;
		else if (!strncmp(p, "<script", 7) || !strncmp(p, "<SCRIPT", 7))
			last_script_blocks++;
		else if (!strncmp(p, "<a ", 3) || !strncmp(p, "<A ", 3) ||
			 !strncmp(p, "<a>", 3) || !strncmp(p, "<A>", 3))
			last_anchor_count++;
	}
	bgtk_log("labyrinth pipeline scan: style=%d script=%d a=%d",
		 last_style_blocks, last_script_blocks, last_anchor_count);
}

/* CSS is applied inside bgtk_html_parse_inline (css.c). Hook kept for
 * future document-level re-style / linked stylesheets. */
static void labyrinth_css_apply(struct BGTK_Widget *page,
				const char *html)
{
	(void)page;
	(void)html;
	/* TODO: fetch <link rel=stylesheet>, media queries, recomputed layout. */
}

/* Boot scripts for the document (no-op). */
static void labyrinth_js_boot(struct BGTK_Widget *page, const char *html)
{
	(void)page;
	(void)html;
	/* TODO: ECMAScript engine, DOM bindings, event loop on input. */
}

static void navigate(const char *url, int push);

/* Resolve rel against base (http/https/file/about). Fragments-only keep base. */
static void resolve_page_url(const char *base, const char *rel, char *out,
			     size_t outlen)
{
	char scheme[16] = {0};
	char host[256] = {0};
	char path[512] = {0};
	const char *p, *he;
	char dir[512], full[1024], norm[1024];
	char *tmp, *save, *seg;
	char *segs[64];
	int nseg = 0, i;

	if (!out || outlen < 2)
		return;
	out[0] = '\0';
	if (!rel || !*rel) {
		snprintf(out, outlen, "%s", base ? base : "");
		return;
	}
	/* Absolute schemes / about: */
	if (strstr(rel, "://") || !strncmp(rel, "about:", 6) ||
	    !strncmp(rel, "file:", 5) || !strncmp(rel, "data:", 5)) {
		snprintf(out, outlen, "%s", rel);
		return;
	}
	/* Same-document fragment */
	if (rel[0] == '#') {
		snprintf(out, outlen, "%s", base ? base : rel);
		return;
	}
	if (!base || !*base) {
		snprintf(out, outlen, "%s", rel);
		return;
	}

	/* Parse base into scheme://host/path */
	p = base;
	if (!strncmp(p, "https://", 8)) {
		snprintf(scheme, sizeof(scheme), "https");
		p += 8;
	} else if (!strncmp(p, "http://", 7)) {
		snprintf(scheme, sizeof(scheme), "http");
		p += 7;
	} else if (!strncmp(p, "file://", 7)) {
		snprintf(scheme, sizeof(scheme), "file");
		p += 7;
		/* file has no host; rest is path */
		snprintf(path, sizeof(path), "%s", p[0] ? p : "/");
		host[0] = '\0';
		goto have_base;
	} else if (!strncmp(p, "about:", 6)) {
		/* Relative from about: → treat as absolute path on about */
		if (rel[0] == '/')
			snprintf(out, outlen, "about:%s", rel + 1);
		else
			snprintf(out, outlen, "about:%s", rel);
		return;
	} else {
		snprintf(out, outlen, "%s", rel);
		return;
	}
	he = strchr(p, '/');
	if (he) {
		size_t hl = (size_t)(he - p);
		if (hl >= sizeof(host))
			hl = sizeof(host) - 1;
		memcpy(host, p, hl);
		host[hl] = '\0';
		snprintf(path, sizeof(path), "%s", he);
	} else {
		snprintf(host, sizeof(host), "%s", p);
		snprintf(path, sizeof(path), "/");
	}

have_base:
	if (rel[0] == '/') {
		/* Absolute path on same origin */
		if (scheme[0] && host[0])
			snprintf(out, outlen, "%s://%s%s", scheme, host, rel);
		else if (scheme[0])
			snprintf(out, outlen, "%s://%s", scheme, rel);
		else
			snprintf(out, outlen, "%s", rel);
		return;
	}

	/* Directory of base path + relative */
	snprintf(dir, sizeof(dir), "%s", path[0] ? path : "/");
	{
		char *ls = strrchr(dir, '/');
		if (ls)
			*(ls + 1) = '\0';
		else
			snprintf(dir, sizeof(dir), "/");
	}
	snprintf(full, sizeof(full), "%s%s", dir, rel);

	tmp = strdup(full);
	if (!tmp) {
		snprintf(out, outlen, "%s", full);
		return;
	}
	save = NULL;
	seg = strtok_r(tmp, "/", &save);
	while (seg && nseg < 64) {
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
	norm[0] = '/';
	norm[1] = '\0';
	for (i = 0; i < nseg; i++) {
		if (i)
			strcat(norm, "/");
		strcat(norm, segs[i]);
	}
	free(tmp);

	if (scheme[0] && host[0])
		snprintf(out, outlen, "%s://%s%s", scheme, host, norm);
	else if (scheme[0] && !strcmp(scheme, "file"))
		snprintf(out, outlen, "file://%s", norm);
	else
		snprintf(out, outlen, "%s", norm);
}

static int labyrinth_link_click(struct BGTK_Widget *w, struct InputEvent ev)
{
	if (!w || w->type != BGTK_WIDGET_TEXT)
		return 0;
	if (ev.code != BTN_LEFT || ev.value != 1)
		return 0;
	if (!bgtk_widget_hit(w, ev.x, ev.y))
		return 0;
	if (!w->data.text.href || !w->data.text.href[0])
		return 0;
	navigate(w->data.text.href, 1);
	return 1;
}

static void labyrinth_wire_links_walk(struct BGTK_Widget *w)
{
	int i;

	if (!w)
		return;
	if (w->type == BGTK_WIDGET_TEXT && w->data.text.href &&
	    w->data.text.href[0]) {
		char resolved[768];

		resolve_page_url(current_url, w->data.text.href, resolved,
				 sizeof(resolved));
		if (resolved[0] && strcmp(resolved, w->data.text.href) != 0) {
			free(w->data.text.href);
			w->data.text.href = strdup(resolved);
		}
		w->handle_event = labyrinth_link_click;
	}
	switch (w->type) {
	case BGTK_WIDGET_SCROLLABLE:
		for (i = 0; i < w->data.scrollable.widget_count; i++)
			labyrinth_wire_links_walk(w->data.scrollable.items[i]);
		break;
	case BGTK_WIDGET_LIST:
		for (i = 0; i < w->data.list_widget.widget_count; i++)
			labyrinth_wire_links_walk(w->data.list_widget.items[i]);
		break;
	case BGTK_WIDGET_FRAME:
		labyrinth_wire_links_walk(w->data.frame.child);
		break;
	case BGTK_WIDGET_BUTTON:
		labyrinth_wire_links_walk(w->data.button.label);
		break;
	case BGTK_WIDGET_LABEL:
		labyrinth_wire_links_walk(w->data.label.text);
		break;
	default:
		break;
	}
}

/* Wire <a href> navigation onto link text widgets. */
static void labyrinth_wire_links(struct BGTK_Widget *page, const char *html)
{
	(void)html;
	labyrinth_wire_links_walk(page);
}

/* ------------------------------------------------------------------ */
/* Chrome / layout                                                    */
/* ------------------------------------------------------------------ */

static void labyrinth_chrome(int *pad, int *fmar, int *bw)
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

static int list_v_overhead(int n, int list_m, int list_p)
{
	if (n < 1)
		return 2 * (list_m + list_p);
	return 2 * list_m * (n - 1) + 2 * (list_m + list_p);
}

static void labyrinth_layout_chrome(void)
{
	int pad, fmar, bw, box_w, box_h, field_pad;
	int usable_w, ah, host_h, over;

	if (!ctx || !content_host || !addr_input)
		return;

	labyrinth_chrome(&pad, &fmar, &bw);
	box_w = ctx->width - 2 * (fmar + bw + pad);
	box_h = ctx->height - 2 * (fmar + bw + pad);
	if (box_w < 80)
		box_w = 80;
	if (box_h < 80)
		box_h = 80;

	field_pad = pad > 4 ? pad / 2 : 4;
	addr_input->padding = field_pad;
	addr_input->margin = 0;
	content_host->padding = 0;
	content_host->margin = 0;

	if (main_list) {
		main_list->padding = 0;
		main_list->margin = 0;
		main_list->w = box_w;
		main_list->h = box_h;
	}
	if (root_frame) {
		root_frame->padding = pad;
		root_frame->margin = fmar;
		root_frame->w = ctx->width;
		root_frame->h = ctx->height;
	}

	usable_w = box_w;
	{
		int text_h = ctx->font_size > 0 ? ctx->font_size + 4 : 18;

		addr_input->h =
			text_h + 2 * (addr_input->padding + addr_input->margin);
		if (addr_input->h < 28)
			addr_input->h = 28;
	}
	ah = addr_input->h;
	over = list_v_overhead(2, 0, 0);
	host_h = box_h - ah - over;
	if (host_h < 40)
		host_h = 40;

	content_host->w = usable_w;
	content_host->h = host_h;
	addr_input->w = usable_w;
	content_w = usable_w;
	content_h = host_h;

	/* Resize live page frame if present. */
	if (content_host->data.frame.child) {
		struct BGTK_Widget *page = content_host->data.frame.child;

		page->w = content_w;
		page->h = content_h;
		if (page->type == BGTK_WIDGET_FRAME && page->data.frame.child) {
			struct BGTK_Widget *sc = page->data.frame.child;

			sc->w = content_w;
			sc->h = content_h;
			if (sc->type == BGTK_WIDGET_SCROLLABLE &&
			    sc->data.scrollable.tmp) {
				free(sc->data.scrollable.tmp);
				sc->data.scrollable.tmp = NULL;
				sc->data.scrollable.widget_capacity = 0;
				sc->data.scrollable.tmp_items = NULL;
				sc->data.scrollable.tmp_item0 = NULL;
				sc->data.scrollable.tmp_nitems = 0;
			}
		}
	}
}

/* ------------------------------------------------------------------ */
/* History                                                            */
/* ------------------------------------------------------------------ */

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

static int hist_back(char *out, size_t n)
{
	if (hist_n < 1)
		return 0;
	hist_n--;
	snprintf(out, n, "%s", hist[hist_n]);
	return 1;
}

/* ------------------------------------------------------------------ */
/* Built-in pages & fetch                                             */
/* ------------------------------------------------------------------ */

/* Pure ASCII so any locale/charset build still embeds correctly. */
static const char *HOME_HTML =
	"<!DOCTYPE html><html><head><title>Labyrinth</title></head><body>"
	"<h1>Labyrinth</h1>"
	"<p>Type a URL in the bar below and press <b>Enter</b>.</p>"
	"<h2>Shortcuts</h2>"
	"<table>"
	"<tr><th>Key</th><th>Action</th></tr>"
	"<tr><td><code>Ctrl+L</code></td><td>Focus address bar</td></tr>"
	"<tr><td><code>Ctrl+R</code></td><td>Reload page</td></tr>"
	"<tr><td><code>Enter</code></td><td>Go to URL in bar</td></tr>"
	"<tr><td><code>Esc</code></td><td>Blur bar (again to quit)</td></tr>"
	"<tr><td><code>b</code></td><td>Back</td></tr>"
	"<tr><td><code>H</code></td><td>Home</td></tr>"
	"<tr><td><code>j</code> / <code>k</code></td><td>Scroll down / up</td></tr>"
	"<tr><td><code>Shift+wheel</code></td><td>Horizontal scroll</td></tr>"
	"<tr><td><code>q</code> / <code>Ctrl+C</code></td><td>Quit</td></tr>"
	"</table>"
	"<h2>URLs</h2>"
	"<ul>"
	"<li><code>about:home</code> - this page</li>"
	"<li><code>about:blank</code> - empty document</li>"
	"<li><code>file:///path/to/page.html</code> - local file</li>"
	"<li><code>http://</code> / <code>https://</code> - web (libtls)</li>"
	"</ul>"
	"<p>Try "
	"<a href=\"about:blank\">about:blank</a> · "
	"<a href=\"https://example.com/\">example.com</a>.</p>"
	"</body></html>";

static const char *BLANK_HTML =
	"<!DOCTYPE html><html><body></body></html>";

/* Normalize about: URLs: trim space, accept about://home, trailing / #. */
static int about_page(const char *url, const char *page)
{
	const char *p;
	size_t n;

	if (!url || !page)
		return 0;
	while (*url == ' ' || *url == '\t')
		url++;
	if (strncasecmp(url, "about:", 6) != 0)
		return 0;
	p = url + 6;
	if (p[0] == '/' && p[1] == '/')
		p += 2;
	while (*p == '/')
		p++;
	n = strlen(page);
	if (strncasecmp(p, page, n) != 0)
		return 0;
	p += n;
	if (*p == '/' || *p == '#' || *p == '?' || *p == '\0' ||
	    *p == ' ' || *p == '\t')
		return 1;
	return 0;
}

static char *read_file_all(const char *path, size_t *out_len)
{
	FILE *f;
	char *buf;
	long sz;

	f = fopen(path, "rb");
	if (!f)
		return NULL;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return NULL;
	}
	sz = ftell(f);
	if (sz < 0 || sz > 8 * 1024 * 1024) {
		fclose(f);
		return NULL;
	}
	rewind(f);
	buf = malloc((size_t)sz + 1);
	if (!buf) {
		fclose(f);
		return NULL;
	}
	if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) {
		free(buf);
		fclose(f);
		return NULL;
	}
	fclose(f);
	buf[sz] = '\0';
	if (out_len)
		*out_len = (size_t)sz;
	return buf;
}

/* Parse http(s)://host[:port]/path into host/path/port. Returns 0 on success. */
static int parse_http_url(const char *url, int is_https, char *host, size_t hostlen,
			  char *path, size_t pathlen, int *port_out)
{
	const char *p, *slash;
	int port = is_https ? 443 : 80;

	if (!url || !host || !path || hostlen < 2 || pathlen < 2)
		return -1;
	if (is_https) {
		if (strncmp(url, "https://", 8) != 0)
			return -1;
		p = url + 8;
	} else {
		if (strncmp(url, "http://", 7) != 0)
			return -1;
		p = url + 7;
	}
	slash = strchr(p, '/');
	if (slash) {
		size_t hl = (size_t)(slash - p);

		if (hl >= hostlen)
			hl = hostlen - 1;
		memcpy(host, p, hl);
		host[hl] = '\0';
		snprintf(path, pathlen, "%s", slash);
	} else {
		snprintf(host, hostlen, "%s", p);
		snprintf(path, pathlen, "/");
	}
	{
		char *col = strchr(host, ':');

		if (col) {
			*col = '\0';
			port = atoi(col + 1);
			if (port < 1)
				port = is_https ? 443 : 80;
		}
	}
	if (!host[0])
		return -1;
	if (port_out)
		*port_out = port;
	return 0;
}

static char *http_strip_headers(char *raw, size_t len, int *out_status)
{
	char *hdr_end, *payload, *out;
	size_t blen;
	int status = 0;

	if (!raw || len < 12) {
		free(raw);
		return NULL;
	}
	if (sscanf(raw, "HTTP/%*s %d", &status) == 1 && out_status)
		*out_status = status;
	hdr_end = strstr(raw, "\r\n\r\n");
	if (!hdr_end) {
		if (out_status && !*out_status)
			*out_status = status;
		return raw;
	}
	payload = hdr_end + 4;
	blen = len - (size_t)(payload - raw);
	out = malloc(blen + 1);
	if (!out) {
		free(raw);
		return NULL;
	}
	memcpy(out, payload, blen);
	out[blen] = '\0';
	free(raw);
	return out;
}

/* Minimal HTTP/1.0 GET over plain TCP. */
static char *http_get(const char *url, int *out_status, char *err, size_t errlen)
{
	char host[256], path[512], portstr[16];
	int port = 80, fd = -1;
	struct addrinfo hints, *res = NULL, *rp;
	char req[1024], *body = NULL;
	size_t cap = 0, len = 0;
	ssize_t n;
	char chunk[4096];

	if (err && errlen)
		err[0] = '\0';
	if (out_status)
		*out_status = 0;
	if (parse_http_url(url, 0, host, sizeof(host), path, sizeof(path),
			   &port) != 0) {
		if (err)
			snprintf(err, errlen, "bad http URL");
		return NULL;
	}
	snprintf(portstr, sizeof(portstr), "%d", port);
	memset(&hints, 0, sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_family = AF_UNSPEC;
	if (getaddrinfo(host, portstr, &hints, &res) != 0) {
		if (err)
			snprintf(err, errlen, "DNS failed for %s", host);
		return NULL;
	}
	for (rp = res; rp; rp = rp->ai_next) {
		fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
		if (fd < 0)
			continue;
		if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0)
			break;
		close(fd);
		fd = -1;
	}
	freeaddrinfo(res);
	if (fd < 0) {
		if (err)
			snprintf(err, errlen, "connect failed");
		return NULL;
	}

	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Labyrinth/0.1 "
		 "(BGTK)\r\nAccept: text/html,*/*\r\nConnection: close\r\n\r\n",
		 path, host);
	if (write(fd, req, strlen(req)) < 0) {
		if (err)
			snprintf(err, errlen, "write failed");
		close(fd);
		return NULL;
	}

	while ((n = read(fd, chunk, sizeof(chunk))) > 0) {
		if (len + (size_t)n + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 8192;
			char *nb;

			while (ncap < len + (size_t)n + 1)
				ncap *= 2;
			if (ncap > 8 * 1024 * 1024)
				break;
			nb = realloc(body, ncap);
			if (!nb)
				break;
			body = nb;
			cap = ncap;
		}
		memcpy(body + len, chunk, (size_t)n);
		len += (size_t)n;
		body[len] = '\0';
	}
	close(fd);
	if (!body || len < 12) {
		free(body);
		if (err)
			snprintf(err, errlen, "empty response");
		return NULL;
	}
	return http_strip_headers(body, len, out_status);
}

/* Retry tls_read/write through WANT_POLLIN/WANT_POLLOUT (same as gemini). */
static ssize_t lab_tls_read(struct tls *t, void *buf, size_t len)
{
	int waited = 0;

	for (;;) {
		ssize_t n = tls_read(t, buf, len);

		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0,
					      .tv_nsec = 5 * 1000 * 1000 };

			if (waited >= LAB_IO_TIMEOUT_MS)
				return -1;
			nanosleep(&ts, NULL);
			waited += 5;
			continue;
		}
		return n;
	}
}

static ssize_t lab_tls_write(struct tls *t, const void *buf, size_t len)
{
	int waited = 0;
	const char *p = buf;
	size_t left = len;

	while (left > 0) {
		ssize_t n = tls_write(t, p, left);

		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0,
					      .tv_nsec = 5 * 1000 * 1000 };

			if (waited >= LAB_IO_TIMEOUT_MS)
				return -1;
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

/* HTTPS GET via libtls. Returns malloc body or NULL. */
static char *https_get(const char *url, int *out_status, char *err, size_t errlen)
{
	char host[256], path[512], portstr[16], req[1024];
	int port = 443;
	struct tls_config *cfg = NULL;
	struct tls *ctx_tls = NULL;
	char *body = NULL;
	size_t cap = 0, len = 0;
	char chunk[4096];
	ssize_t n;

	if (err && errlen)
		err[0] = '\0';
	if (out_status)
		*out_status = 0;
	if (parse_http_url(url, 1, host, sizeof(host), path, sizeof(path),
			   &port) != 0) {
		if (err)
			snprintf(err, errlen, "bad https URL");
		return NULL;
	}
	if (tls_init() == -1) {
		if (err)
			snprintf(err, errlen, "tls_init failed");
		return NULL;
	}
	cfg = tls_config_new();
	if (!cfg) {
		if (err)
			snprintf(err, errlen, "tls_config_new failed");
		return NULL;
	}
	/* Match gemini: allow typical self-signed / incomplete chains for now. */
	tls_config_insecure_noverifycert(cfg);
	tls_config_insecure_noverifyname(cfg);

	ctx_tls = tls_client();
	if (!ctx_tls) {
		if (err)
			snprintf(err, errlen, "tls_client failed");
		tls_config_free(cfg);
		return NULL;
	}
	if (tls_configure(ctx_tls, cfg) == -1) {
		if (err)
			snprintf(err, errlen, "tls_configure: %s",
				 tls_error(ctx_tls) ? tls_error(ctx_tls) : "?");
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return NULL;
	}
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (tls_connect(ctx_tls, host, portstr) == -1) {
		if (err)
			snprintf(err, errlen, "tls_connect %s:%s: %s", host,
				 portstr,
				 tls_error(ctx_tls) ? tls_error(ctx_tls) : "?");
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return NULL;
	}

	snprintf(req, sizeof(req),
		 "GET %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: Labyrinth/0.1 "
		 "(BGTK)\r\nAccept: text/html,*/*\r\nConnection: close\r\n\r\n",
		 path, host);
	if (lab_tls_write(ctx_tls, req, strlen(req)) < 0) {
		if (err)
			snprintf(err, errlen, "tls_write: %s",
				 tls_error(ctx_tls) ? tls_error(ctx_tls) : "?");
		tls_close(ctx_tls);
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return NULL;
	}

	for (;;) {
		n = lab_tls_read(ctx_tls, chunk, sizeof(chunk));
		if (n < 0) {
			if (err && !body)
				snprintf(err, errlen, "tls_read: %s",
					 tls_error(ctx_tls) ? tls_error(ctx_tls)
							    : "?");
			break;
		}
		if (n == 0)
			break;
		if (len + (size_t)n + 1 > cap) {
			size_t ncap = cap ? cap * 2 : 8192;
			char *nb;

			while (ncap < len + (size_t)n + 1)
				ncap *= 2;
			if (ncap > 8 * 1024 * 1024)
				break;
			nb = realloc(body, ncap);
			if (!nb)
				break;
			body = nb;
			cap = ncap;
		}
		memcpy(body + len, chunk, (size_t)n);
		len += (size_t)n;
		body[len] = '\0';
	}
	tls_close(ctx_tls);
	tls_free(ctx_tls);
	tls_config_free(cfg);

	if (!body || len < 12) {
		free(body);
		if (err && !err[0])
			snprintf(err, errlen, "empty HTTPS response");
		return NULL;
	}
	return http_strip_headers(body, len, out_status);
}

static char *wrap_http_error(int st, char *body)
{
	char *wrap;
	size_t n;

	if (!body)
		return NULL;
	if (st < 400)
		return body;
	n = strlen(body) + 128;
	wrap = malloc(n);
	if (!wrap)
		return body;
	snprintf(wrap, n,
		 "<html><body><h1>HTTP %d</h1><pre>%s</pre></body></html>", st,
		 body);
	free(body);
	return wrap;
}

/* Builtin pages: pointer to static HTML (not malloc). NULL if not builtin. */
static const char *builtin_document(const char *url)
{
	if (about_page(url, "home") || (url && !strcmp(url, "about:")))
		return HOME_HTML;
	if (about_page(url, "blank"))
		return BLANK_HTML;
	return NULL;
}

static char *fetch_document(const char *url, char *err, size_t errlen)
{
	const char *builtin;

	if (!url || !url[0]) {
		if (err)
			snprintf(err, errlen, "empty URL");
		return NULL;
	}
	/* Skip leading spaces (address bar paste). */
	while (*url == ' ' || *url == '\t')
		url++;
	builtin = builtin_document(url);
	if (builtin) {
		char *copy = strdup(builtin);

		if (!copy && err)
			snprintf(err, errlen, "out of memory");
		return copy;
	}
	if (!strncasecmp(url, "about:", 6)) {
		if (err)
			snprintf(err, errlen, "unknown about: page");
		return NULL;
	}
	if (!strncmp(url, "file://", 7)) {
		const char *path = url + 7;
		char *body;

		/* file:///path → /path */
		if (path[0] == '/' && path[1] == '/')
			path += 1;
		body = read_file_all(path, NULL);
		if (!body && err)
			snprintf(err, errlen, "cannot read %s", path);
		return body;
	}
	if (!strncmp(url, "https://", 8)) {
		int st = 0;
		char *body = https_get(url, &st, err, errlen);

		return wrap_http_error(st, body);
	}
	if (!strncmp(url, "http://", 7)) {
		int st = 0;
		char *body = http_get(url, &st, err, errlen);

		return wrap_http_error(st, body);
	}
	if (err)
		snprintf(err, errlen,
			 "unsupported scheme (try https: http: file: about:)");
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Document load / display                                            */
/* ------------------------------------------------------------------ */

static void set_addr_bar(const char *url)
{
	if (!addr_input)
		return;
	free(addr_input->data.text_input.text);
	addr_input->data.text_input.text = strdup(url ? url : "");
	addr_input->data.text_input.cursor_pos =
		(uint32_t)strlen(addr_input->data.text_input.text);
	addr_input->data.text_input.scroll_x = 0;
}

static void show_message_page(const char *title, const char *detail)
{
	char html[2048];
	struct BGTK_Widget *page, *old;

	snprintf(html, sizeof(html),
		 "<html><body><h1>%s</h1><p>%s</p>"
		 "<p>Enter another URL in the bar below.</p></body></html>",
		 title ? title : "Error", detail ? detail : "");
	labyrinth_layout_chrome();
	page = bgtk_html_parse_inline(ctx, html, content_w, content_h);
	old = content_host->data.frame.child;
	content_host->data.frame.child = page;
	if (page)
		bgtk_widget_set_parent(page, content_host);
	if (old)
		bgtk_widget_destroy(old);
	free(last_html);
	last_html = strdup(html);
}

static void load_html_string(const char *html)
{
	struct BGTK_Widget *page, *old;

	if (!html)
		html = BLANK_HTML;

	labyrinth_scan_pipeline(html);
	labyrinth_layout_chrome();

	page = bgtk_html_parse_inline(ctx, html, content_w, content_h);
	if (!page) {
		show_message_page("Parse error", "Could not build widget tree from HTML.");
		return;
	}

	/* Planned stages — currently no-ops. */
	labyrinth_css_apply(page, html);
	labyrinth_js_boot(page, html);
	labyrinth_wire_links(page, html);

	old = content_host->data.frame.child;
	content_host->data.frame.child = page;
	bgtk_widget_set_parent(page, content_host);
	if (old)
		bgtk_widget_destroy(old);

	free(last_html);
	last_html = strdup(html);
}

static void load_url(const char *url)
{
	char err[256];
	char *html;
	const char *builtin;

	if (!url || !url[0])
		return;
	while (*url == ' ' || *url == '\t')
		url++;
	snprintf(current_url, sizeof(current_url), "%s", url);
	set_addr_bar(current_url);
	bgtk_log("labyrinth load %s", current_url);

	/*
	 * Builtins never go through fetch/strdup failure as a hard error —
	 * pass the static string so about:home always paints the shortcuts.
	 */
	builtin = builtin_document(current_url);
	if (builtin) {
		load_html_string(builtin);
	} else {
		err[0] = '\0';
		html = fetch_document(current_url, err, sizeof(err));
		if (!html) {
			bgtk_log("labyrinth load failed: %s",
				 err[0] ? err : "unknown error");
			show_message_page("Load failed",
					  err[0] ? err : "unknown error");
			if (ctx)
				bgtk_draw_widgets(ctx);
			return;
		}
		load_html_string(html);
		free(html);
	}
	if (ctx) {
		/* Prefer focusing content so j/k scroll the page. */
		if (content_host && content_host->data.frame.child) {
			struct BGTK_Widget *page =
				content_host->data.frame.child;
			struct BGTK_Widget *sc =
				(page->type == BGTK_WIDGET_FRAME)
					? page->data.frame.child
					: page;

			if (sc && sc->type == BGTK_WIDGET_SCROLLABLE)
				bgtk_set_focus(ctx, sc);
			else
				bgtk_set_focus(ctx, content_host);
		}
		bgtk_draw_widgets(ctx);
	}
}

static void navigate(const char *url, int push)
{
	char full[768];
	char resolved[768];

	if (!url || !url[0])
		return;
	if (strncmp(url, "http://", 7) == 0 ||
	    strncmp(url, "https://", 8) == 0 ||
	    strncmp(url, "file://", 7) == 0 ||
	    strncmp(url, "about:", 6) == 0) {
		/* absolute — fall through */
	} else if (current_url[0] &&
		   (url[0] == '/' || strchr(url, ':') == NULL)) {
		/* Relative path/file against current page (link wiring). */
		resolve_page_url(current_url, url, resolved, sizeof(resolved));
		if (strstr(resolved, "://") || !strncmp(resolved, "about:", 6) ||
		    !strncmp(resolved, "file:", 5)) {
			url = resolved;
		} else {
			snprintf(full, sizeof(full), "http://%s", url);
			url = full;
		}
	} else {
		/* Bare host typed in the URL bar → http:// */
		snprintf(full, sizeof(full), "http://%s", url);
		url = full;
	}
	if (push && current_url[0])
		hist_push(current_url);
	load_url(url);
}

static void addr_on_enter(void)
{
	if (!addr_input || !addr_input->data.text_input.text)
		return;
	navigate(addr_input->data.text_input.text, 1);
}

/* ------------------------------------------------------------------ */
/* Focus / app keys (Ctrl+L, Ctrl+R, …)                               */
/* ------------------------------------------------------------------ */

static struct BGTK_Widget *content_scroll_widget(void)
{
	struct BGTK_Widget *page;

	if (!content_host)
		return NULL;
	page = content_host->data.frame.child;
	if (!page)
		return NULL;
	if (page->type == BGTK_WIDGET_FRAME && page->data.frame.child &&
	    page->data.frame.child->type == BGTK_WIDGET_SCROLLABLE)
		return page->data.frame.child;
	if (page->type == BGTK_WIDGET_SCROLLABLE)
		return page;
	return NULL;
}

/* Focus the URL bar and select all (browser-style Ctrl+L). */
static void focus_addr_bar(void)
{
	int len;

	if (!ctx || !addr_input)
		return;
	bgtk_set_focus(ctx, addr_input);
	len = addr_input->data.text_input.text
		      ? (int)strlen(addr_input->data.text_input.text)
		      : 0;
	addr_input->data.text_input.cursor_pos = (uint32_t)len;
	addr_input->data.text_input.selection_start = 0;
	addr_input->data.text_input.selection_end = len;
	addr_input->data.text_input.scroll_x = 0;
}

/*
 * App-level shortcuts. Call after bgtk_update_modifiers.
 * Returns: 0 = not handled, 1 = handled (redraw), 2 = quit app.
 */
static int labyrinth_app_key(struct InputEvent ev)
{
	int addr_focus, mods, scroll_key;
	char prev[768];
	struct BGTK_Widget *sc;

	if (!ctx || ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
		return 0;

	addr_focus = (ctx->focused_widget == addr_input);
	mods = bgtk_mods_from_ctx(ctx);
	scroll_key = 0;

	/* Ctrl+L (or bare o): focus address bar */
	if (((mods & BGTK_MOD_CTRL) && ev.code == KEY_L) ||
	    (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
	     !(mods & BGTK_MOD_SHIFT) && ev.code == KEY_O)) {
		focus_addr_bar();
		return 1;
	}
	/* Ctrl+R (or bare r when not typing URL): reload */
	if (((mods & BGTK_MOD_CTRL) && ev.code == KEY_R) ||
	    (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
	     !(mods & BGTK_MOD_SHIFT) && ev.code == KEY_R)) {
		load_url(current_url);
		return 1;
	}
	if (ev.code == KEY_ESC) {
		if (addr_focus) {
			sc = content_scroll_widget();
			bgtk_set_focus(ctx, sc ? sc : content_host);
			return 1;
		}
		return 2; /* quit */
	}
	if (((mods & BGTK_MOD_CTRL) && ev.code == KEY_C) ||
	    (!addr_focus && !(mods & BGTK_MOD_CTRL) && ev.code == KEY_Q))
		return 2;
	if (!addr_focus && !(mods & BGTK_MOD_CTRL) && ev.code == KEY_B) {
		if (hist_back(prev, sizeof(prev)))
			load_url(prev);
		return 1;
	}
	if (!addr_focus && !(mods & BGTK_MOD_CTRL) &&
	    !(mods & BGTK_MOD_SHIFT) && ev.code == KEY_H) {
		navigate("about:home", 1);
		return 1;
	}
	if (ev.code == KEY_PAGEUP || ev.code == KEY_PAGEDOWN)
		scroll_key = 1;
	else if (!addr_focus &&
		 (ev.code == KEY_UP || ev.code == KEY_DOWN ||
		  ev.code == KEY_HOME || ev.code == KEY_END ||
		  ev.code == KEY_SPACE || ev.code == KEY_J ||
		  ev.code == KEY_K))
		scroll_key = 1;
	if (scroll_key) {
		sc = content_scroll_widget();
		if (sc && sc->handle_event && sc->handle_event(sc, ev))
			return 1;
		return 0;
	}
	return 0;
}

#ifndef LABYRINTH_TEST_MODE
static void labyrinth_on_resize(void)
{
	labyrinth_layout_chrome();
	if (last_html)
		load_html_string(last_html);
	else if (content_host && content_host->data.frame.child) {
		content_host->data.frame.child->w = content_w;
		content_host->data.frame.child->h = content_h;
	}
}

/* ------------------------------------------------------------------ */
/* Main                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
	int width = 720, height = 520;
	int conn_fd;
	struct BufferRequest req;
	void *buffer;
	struct BGTK_Widget *main_items[2];
	struct BGTK_Widget *placeholder;
	struct BGCEMessage msg;
	ssize_t bytes;
	const char *quit_reason = "unknown";
	int pad, fmar, bw;

	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("labyrinth");
	signal(SIGPIPE, SIG_IGN);
	bgtk_log("labyrinth starting pid=%ld", (long)getpid());

	if (tls_init() == -1) {
		bgtk_log("tls_init failed — is libtls/libretls installed?");
		return 1;
	}
	bgtk_log("tls_init ok");

	conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return 1;
	}
	req.width = (uint32_t)width;
	req.height = (uint32_t)height;
	buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return 1;
	}
	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed");
		bgce_disconnect(conn_fd);
		return 1;
	}

	labyrinth_chrome(&pad, &fmar, &bw);
	addr_input = bgtk_text_input(
		ctx, current_url, 200, 0,
		(BGTK_Options){.padding = pad > 4 ? pad / 2 : 4, .margin = 0});
	addr_input->data.text_input.on_enter = addr_on_enter;

	placeholder = bgtk_text(ctx, "Loading…",
				(BGTK_Options){.padding = 8, .margin = 4});
	content_host = bgtk_frame(ctx, placeholder, 100, 100,
				  (BGTK_Options){.padding = 0, .margin = 0});
	if (content_host)
		content_host->data.frame.border_w = 0;

	main_items[0] = content_host;
	main_items[1] = addr_input;
	main_list = bgtk_list(ctx, main_items, 2,
			      (BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					     .padding = 0,
					     .margin = 0});
	root_frame = bgtk_frame(ctx, main_list, width, height,
				(BGTK_Options){.padding = pad, .margin = fmar});
	ctx->root_widget = root_frame;
	labyrinth_layout_chrome();

	/*
	 * Always paint builtin home first (shortcuts table). Do not depend on
	 * fetch/network/strdup — Load failed at startup was a user-facing bug
	 * when about: matching or allocation failed.
	 */
	snprintf(current_url, sizeof(current_url), "about:home");
	set_addr_bar(current_url);
	labyrinth_layout_chrome();
	load_html_string(HOME_HTML);
	focus_addr_bar();
	bgtk_draw_widgets(ctx);

	bgtk_log("entering main loop %dx%d url=%s", width, height, current_url);
	while (1) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0)
				quit_reason = "server closed connection";
			else if (errno == EINTR)
				continue;
			else
				quit_reason = "recv error";
			break;
		}
		switch (msg.type) {
		case MSG_INPUT_EVENT: {
			struct InputEvent *ev = &msg.data.input_event;
			int res = 0;

			if (ev->type == EV_ABS)
				break;
			/* Wheel / h-wheel (and Shift+wheel → horizontal in
			 * scrollable). Drop other EV_REL (pointer deltas). */
			if (ev->type == EV_REL && ev->code != REL_WHEEL &&
			    ev->code != REL_HWHEEL)
				break;

			bgtk_update_modifiers(ctx, *ev);

			{
				int app = labyrinth_app_key(*ev);

				if (app == 2) {
					quit_reason = "quit key";
					goto done;
				}
				if (app == 1)
					res = 1;
			}

			if (!res)
				res = bgtk_handle_input_event(ctx, *ev);
			if (res)
				bgtk_draw_widgets(ctx);
			break;
		}
		case MSG_FOCUS_CHANGE:
			bgtk_set_window_focus(ctx, msg.data.focus_event.state);
			break;
		case MSG_BUFFER_CHANGE:
			if (bgtk_handle_buffer_change(
				    ctx, &msg.data.buffer_reply) == 0) {
				labyrinth_on_resize();
				bgtk_draw_widgets(ctx);
			}
			break;
		default:
			break;
		}
	}
done:
	bgtk_log("labyrinth exit: %s", quit_reason);
	free(last_html);
	bgtk_destroy(ctx);
	bgce_disconnect(conn_fd);
	return 0;
}

#endif /* LABYRINTH_TEST_MODE */

/* Test hooks (SETTINGS-style) */
#ifdef LABYRINTH_TEST_MODE
void labyrinth_test_init(struct BGTK_Context *c, int w, int h);
void labyrinth_test_load(const char *url);
void labyrinth_test_load_html(const char *html);
const char *labyrinth_test_url(void);
/* Inject app key after mods updated; returns 0/1/2 like labyrinth_app_key. */
int labyrinth_test_app_key(struct InputEvent ev);
struct BGTK_Widget *labyrinth_test_addr(void);
void labyrinth_test_focus_addr(void);

void labyrinth_test_init(struct BGTK_Context *c, int w, int h)
{
	int pad, fmar, bw;
	struct BGTK_Widget *main_items[2];
	struct BGTK_Widget *placeholder;

	ctx = c;
	labyrinth_chrome(&pad, &fmar, &bw);
	addr_input = bgtk_text_input(
		ctx, current_url, 200, 0,
		(BGTK_Options){.padding = pad > 4 ? pad / 2 : 4, .margin = 0});
	addr_input->data.text_input.on_enter = addr_on_enter;
	placeholder = bgtk_text(ctx, "…",
				(BGTK_Options){.padding = 8, .margin = 4});
	content_host = bgtk_frame(ctx, placeholder, 100, 100,
				  (BGTK_Options){.padding = 0, .margin = 0});
	if (content_host)
		content_host->data.frame.border_w = 0;
	main_items[0] = content_host;
	main_items[1] = addr_input;
	main_list = bgtk_list(ctx, main_items, 2,
			      (BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					     .padding = 0,
					     .margin = 0});
	root_frame = bgtk_frame(ctx, main_list, w, h,
				(BGTK_Options){.padding = pad, .margin = fmar});
	ctx->root_widget = root_frame;
	labyrinth_layout_chrome();
}

void labyrinth_test_load(const char *url)
{
	navigate(url, 0);
}

void labyrinth_test_load_html(const char *html)
{
	snprintf(current_url, sizeof(current_url), "about:test");
	set_addr_bar(current_url);
	load_html_string(html);
	if (ctx)
		bgtk_draw_widgets(ctx);
}

const char *labyrinth_test_url(void)
{
	return current_url;
}

int labyrinth_test_app_key(struct InputEvent ev)
{
	if (!ctx)
		return 0;
	bgtk_update_modifiers(ctx, ev);
	return labyrinth_app_key(ev);
}

struct BGTK_Widget *labyrinth_test_addr(void)
{
	return addr_input;
}

void labyrinth_test_focus_addr(void)
{
	focus_addr_bar();
}
#endif
