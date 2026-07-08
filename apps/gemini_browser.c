#include <bgce.h>
#include <bgtk.h>
#include <ctype.h>
#include <errno.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <tls.h>
#include <unistd.h>

static struct BGTK_Context *ctx = NULL;
static struct BGTK_Widget *content_scroll = NULL;
static struct BGTK_Widget *addr_input = NULL;
static struct BGTK_Widget *main_list = NULL;
static struct BGTK_Widget *root_frame = NULL;

static char current_url[512] = "gemini://geminiprotocol.net/";
/* Retained gemtext body so resize can reflow wrap without re-fetch. */
static char *last_body = NULL;

static struct BGTK_Widget *link_target_widgets[128];
static char link_targets[128][512];
static int num_page_links = 0;

static void content_scroll_invalidate_tmp(void);
static void rebuild_content_from_gemtext(const char *body);

/* Line chrome: keep body lines dense; theme spacing is for outer chrome. */
static BGTK_Options line_opts(void)
{
	return (BGTK_Options){.padding = 1, .margin = 1};
}

static int theme_pad(void)
{
	return (ctx && ctx->theme.padding > 0) ? ctx->theme.padding : 6;
}

static int theme_mar(void)
{
	return (ctx && ctx->theme.margin > 0) ? ctx->theme.margin : 4;
}

/* Outer chrome inset: frame border+pad + list pad/margin. */
static int chrome_inset(void)
{
	int pad = theme_pad();
	int mar = theme_mar();
	int half = mar > 1 ? mar / 2 : 1;
	int bw = ctx ? (int)ctx->theme.frame_border_size : 1;
	if (bw < 1)
		bw = 1;
	return 2 * (pad + bw + half);
}

/* Size scroll + URL bar to the current window using theme spacing. */
static void gemini_layout_chrome(void)
{
	int inset, usable_w, ah, scroll_h, half;

	if (!ctx || !content_scroll || !addr_input)
		return;

	half = theme_mar() > 1 ? theme_mar() / 2 : 1;
	inset = chrome_inset();
	usable_w = ctx->width - inset;
	if (usable_w < 80)
		usable_w = 80;

	addr_input->padding = theme_pad() > 2 ? theme_pad() / 2 : 2;
	addr_input->margin = half;
	content_scroll->padding = half;
	content_scroll->margin = half;
	if (main_list) {
		main_list->padding = half;
		main_list->margin = half;
	}
	if (root_frame) {
		root_frame->padding = theme_pad();
		root_frame->margin = 0;
		root_frame->w = ctx->width;
		root_frame->h = ctx->height;
	}

	ah = addr_input->h > 0 ? addr_input->h : 28;
	scroll_h = ctx->height - ah - inset;
	if (scroll_h < 80)
		scroll_h = 80;

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

/* click handler for link text widgets */
static int gemini_link_handler(struct BGTK_Widget *w, struct InputEvent ev)
{
	if (ev.code == BTN_LEFT && ev.value == 1) {
		for (int i = 0; i < num_page_links; i++) {
			if (link_target_widgets[i] == w) {
				load_url(link_targets[i]);
				return 1;
			}
		}
	}
	return 0;
}

/* Simple word-wrap for long paragraphs and link labels. Returns malloc'ed array of lines (caller frees). */
static char **wrap_text(FT_Face face, const char *text, int max_width, int *nlines)
{
	*nlines = 0;
	if (!face || !text || !*text || max_width < 20) return NULL;
	char **lines = NULL;
	int n = 0, cap = 0;
	int len = strlen(text);
	int i = 0;
	while (i < len) {
		int j = i;
		int last_space = -1;
		int w = 0;
		for (; j < len; j++) {
			unsigned char ch = (unsigned char)text[j];
			if (FT_Load_Char(face, ch, FT_LOAD_DEFAULT) == 0)
				w += face->glyph->advance.x >> 6;
			if (w > max_width && j > i) break;
			if (isspace(ch)) last_space = j;
		}
		int end = (j >= len) ? len : (last_space > i ? last_space : j);
		int lnlen = end - i;
		char *ln = malloc(lnlen + 1);
		memcpy(ln, text + i, lnlen);
		ln[lnlen] = 0;
		while (lnlen > 0 && isspace((unsigned char)ln[lnlen-1])) ln[--lnlen] = 0;
		if (n >= cap) {
			cap = cap ? cap * 2 : 8;
			lines = realloc(lines, cap * sizeof(char *));
		}
		lines[n++] = ln;
		i = end;
		while (i < len && isspace((unsigned char)text[i])) i++;
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
			snprintf(vis, sizeof(vis), "\xe2\x80\xa2 %s", line + 2);
			item_color = 10;
		} else {
			snprintf(vis, sizeof(vis), "%s", line);
		}

		char resolved[512] = {0};
		if (is_link)
			resolve_url(current_url, link_to, resolved, sizeof(resolved));

		if (header_level > 0 && cnt > 0) {
			new_items[cnt-1]->h += 5;  /* increase the top spacing a little before headings */
		}

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
						tw->handle_event =
							gemini_link_handler;
					}
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
					tw->handle_event = gemini_link_handler;
				}
				if (s < nsubs - 1) {
					tw->h -= 3;  /* decrease a little for lines in the same paragraph (tighter leading between wraps) */
				}
				if (s == nsubs - 1) {
					tw->h += 5;  /* extra vertical space after this logical block */
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
	bgtk_log("rebuild: %d line widgets, %d links", cnt, num_page_links);
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

	bgtk_draw_widgets(ctx);
	bgtk_log("load_url: draw complete for %s", current_url);
}

static void addr_on_enter(void)
{
	if (addr_input)
		load_url(addr_input->data.text_input.text);
}

int main(void)
{
	int width = 640;
	int height = 480;
	int conn_fd;
	struct BufferRequest req;
	void *buffer;
	int usable_w;
	int reserved;
	int scroll_h;
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
		int pad = theme_pad();
		int mar = theme_mar();
		int half = mar > 1 ? mar / 2 : 1;
		int addr_pad = pad > 2 ? pad / 2 : 2;

		usable_w = width - chrome_inset();
		if (usable_w < 80)
			usable_w = 80;

		/* Full-width URL bar; Enter navigates (no Go button). */
		addr_input = bgtk_text_input(ctx, current_url, usable_w, 0,
					     (BGTK_Options){.padding = addr_pad,
							    .margin = half});
		addr_input->data.text_input.on_enter = addr_on_enter;

		content_scroll = bgtk_scrollable(
			ctx, NULL, 0,
			(BGTK_Options){.padding = half, .margin = half});

		reserved = (addr_input ? addr_input->h : 28) + chrome_inset();
		scroll_h = height - reserved;
		if (scroll_h < 80)
			scroll_h = 80;
		content_scroll->w = usable_w;
		content_scroll->h = scroll_h;

		/* Placeholder so the window is visible before the network fetch. */
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
					      .padding = half,
					      .margin = half});

		frame = bgtk_frame(ctx, main_list, width, height,
				   (BGTK_Options){.padding = pad, .margin = 0});
		root_frame = frame;
	}

	ctx->root_widget = frame;
	bgtk_set_focus(ctx, addr_input);
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
			if (bgtk_is_app_quit_event(ctx, *ev)) {
				bgtk_log("quit key type=%d code=%d value=%d "
					 "mods shift=%d ctrl=%d alt=%d",
					 ev->type, ev->code, ev->value,
					 ctx->shift_held, ctx->ctrl_held,
					 ctx->alt_held);
				quit_reason = "app quit key (Esc or Ctrl+C)";
				goto done;
			}

			/* Browser chords (before text-input eats the key). */
			if (ev->type == EV_KEY &&
			    (ev->value == 1 || ev->value == 2)) {
				if (ctx->ctrl_held && ev->code == KEY_L) {
					bgtk_set_focus(ctx, addr_input);
					res = 1;
					break;
				}
				if (ctx->ctrl_held && ev->code == KEY_R) {
					load_url(current_url);
					res = 1;
					break;
				}
				if (ev->code == KEY_ESC) {
					if (ctx->focused_widget == addr_input) {
						bgtk_set_focus(ctx,
							       content_scroll);
						res = 1;
						break;
					}
				}
				/* Scroll page when focus is not the URL field. */
				if (content_scroll &&
				    ctx->focused_widget != addr_input &&
				    content_scroll->handle_event &&
				    (ev->code == KEY_UP ||
				     ev->code == KEY_DOWN ||
				     ev->code == KEY_PAGEUP ||
				     ev->code == KEY_PAGEDOWN ||
				     ev->code == KEY_HOME ||
				     ev->code == KEY_END ||
				     ev->code == KEY_SPACE ||
				     ev->code == KEY_J ||
				     ev->code == KEY_K)) {
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
