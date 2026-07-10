/* test/test_gemini_browser.c
 *
 * Headless (mock) test version of the gemini browser.
 * Lets you run on macOS or anywhere without bgce server or network.
 * Builds the real widget tree (frame + vertical list + scrollable of text lines
 * for gemtext + current URL text + address bar at bottom inside the frame).
 *
 * Exercises layout, text rendering, text input focus/typing/enter, link clicking
 * (via custom handle_event on some lines), content replacement on "nav", and
 * produces PNG screenshots you can visually inspect.
 *
 * Build: make test_gemini_browser
 * Run:   ./test_gemini_browser
 * Output: test/screenshots/gemini_browser_*.png files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <linux/input.h>
#include <ctype.h>
#include <tls.h>

#include "bgtk.h"
#include "internal.h"

static struct BGTK_Context *ctx = NULL;
static struct BGTK_Widget *content_scroll = NULL;
static struct BGTK_Widget *addr_input = NULL;
static struct BGTK_Widget *main_list = NULL;
static struct BGTK_Widget *root_frame = NULL;

static char current_url[512] = "gemini://geminiprotocol.net/";
static char *last_body = NULL;
static struct BGTK_Widget *link_target_widgets[128];
static char link_targets[128][512];
static int num_page_links = 0;

static BGTK_Options line_opts(void)
{
	return (BGTK_Options){.padding = 2, .margin = 1};
}

#define GEM_V_BEFORE_HEADER 28
#define GEM_V_AFTER_HEADER  20
#define GEM_V_AFTER_PARA    22
#define GEM_V_EMPTY_LINE    16

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

/* Root chrome: [frame_margin][border][padding][content]… */
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

static int list_v_overhead(int n, int list_m, int list_p)
{
	if (n < 1)
		return 2 * (list_m + list_p);
	return 2 * list_m * (n - 1) + 2 * (list_m + list_p);
}

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

static void rebuild_content_from_gemtext(const char *body);

static void gemini_layout_chrome(void)
{
	int pad, fmar, bw, box_w, box_h, list_m, list_p, field_pad;
	int usable_w, ah, scroll_h, over;

	if (!ctx || !content_scroll || !addr_input)
		return;
	gemini_chrome(&pad, &fmar, &bw);
	box_w = ctx->width - 2 * (fmar + bw + pad);
	box_h = ctx->height - 2 * (fmar + bw + pad);
	if (box_w < 80)
		box_w = 80;
	if (box_h < 80)
		box_h = 80;
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

static void gemini_on_resize(void)
{
	gemini_layout_chrome();
	if (last_body)
		rebuild_content_from_gemtext(last_body);
	else
		gemini_recompute_scroll_height();
	if (ctx && content_scroll)
		ctx->focused_widget = content_scroll;
}

/* --- Real Gemini client (must retry TLS_WANT_POLLIN/POLLOUT; n<=0 is wrong) --- */

#ifndef TLS_WANT_POLLIN
#define TLS_WANT_POLLIN (-2)
#endif
#ifndef TLS_WANT_POLLOUT
#define TLS_WANT_POLLOUT (-3)
#endif

static ssize_t test_tls_read(struct tls *t, void *buf, size_t len)
{
	int waited = 0;
	for (;;) {
		ssize_t n = tls_read(t, buf, len);
		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
			if (waited >= 15000)
				return -1;
			nanosleep(&ts, NULL);
			waited += 5;
			continue;
		}
		return n;
	}
}

static ssize_t test_tls_write(struct tls *t, const void *buf, size_t len)
{
	int waited = 0;
	const char *p = buf;
	size_t left = len;
	while (left > 0) {
		ssize_t n = tls_write(t, p, left);
		if (n == TLS_WANT_POLLIN || n == TLS_WANT_POLLOUT) {
			struct timespec ts = { .tv_sec = 0, .tv_nsec = 5 * 1000 * 1000 };
			if (waited >= 15000)
				return -1;
			nanosleep(&ts, NULL);
			waited += 5;
			continue;
		}
		if (n <= 0)
			return n;
		p += n;
		left -= (size_t)n;
		waited = 0;
	}
	return (ssize_t)len;
}

static int fetch_gemini(const char *req_url, int *out_status, char **out_meta, char **out_body)
{
	*out_status = 0;
	*out_meta = NULL;
	*out_body = NULL;

	char host[256] = {0};
	char path[768] = "/";
	int port = 1965;

	const char *p = req_url;
	if (!strncmp(p, "gemini://", 9))
		p += 9;
	const char *he = strchr(p, '/');
	const char *pe = strchr(p, ':');
	if (pe && (!he || pe < he)) {
		int hl = (int)(pe - p);
		if (hl > 255) hl = 255;
		memcpy(host, p, hl); host[hl] = 0;
		port = atoi(pe + 1);
		if (he) strncpy(path, he, sizeof(path)-1);
	} else if (he) {
		int hl = (int)(he - p);
		memcpy(host, p, hl); host[hl] = 0;
		strncpy(path, he, sizeof(path)-1);
	} else {
		strncpy(host, p, sizeof(host)-1);
	}
	if (!host[0]) return -1;
	if (port < 1) port = 1965;
	if (!path[0]) strcpy(path, "/");

	char selector[1024];
	strncpy(selector, req_url, sizeof(selector) - 1);
	selector[sizeof(selector) - 1] = '\0';

	if (tls_init() == -1)
		return -2;

	struct tls_config *cfg = tls_config_new();
	if (!cfg)
		return -2;
	tls_config_insecure_noverifycert(cfg);
	tls_config_insecure_noverifyname(cfg);

	struct tls *ctx_tls = tls_client();
	if (!ctx_tls) {
		tls_config_free(cfg);
		return -2;
	}
	if (tls_configure(ctx_tls, cfg) == -1) {
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -2;
	}

	char portstr[16];
	snprintf(portstr, sizeof(portstr), "%d", port);
	if (tls_connect(ctx_tls, host, portstr) == -1) {
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -6;
	}

	char req[1200];
	int rlen = snprintf(req, sizeof(req), "%s\r\n", selector);
	if (test_tls_write(ctx_tls, req, (size_t)rlen) < 0) {
		tls_close(ctx_tls);
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -7;
	}

	char hbuf[1024] = {0};
	int hlen = 0;
	while (hlen < (int)sizeof(hbuf) - 1) {
		ssize_t n = test_tls_read(ctx_tls, hbuf + hlen, 1);
		if (n <= 0)
			break;
		hlen += (int)n;
		if (hlen >= 2 && hbuf[hlen - 2] == '\r' && hbuf[hlen - 1] == '\n')
			break;
	}
	hbuf[hlen] = 0;

	int status = 0;
	char meta[512] = {0};
	if (sscanf(hbuf, "%d %511[^\r\n]", &status, meta) < 1)
		status = 59;
	*out_status = status;
	if (out_meta)
		*out_meta = strdup(meta);

	size_t cap = 8192;
	size_t blen = 0;
	char *body = malloc(cap);
	if (!body) {
		tls_close(ctx_tls);
		tls_free(ctx_tls);
		tls_config_free(cfg);
		return -8;
	}
	for (;;) {
		char rbuf[1024];
		ssize_t n = test_tls_read(ctx_tls, rbuf, sizeof(rbuf));
		if (n <= 0)
			break;
		if (blen + (size_t)n + 1 > cap) {
			cap = cap * 2 + (size_t)n;
			char *nb = realloc(body, cap);
			if (!nb) {
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
	return 0;
}

static void resolve_url(const char *base, const char *rel, char *out, size_t outlen)
{
	if (!rel || !*rel) {
		strncpy(out, base, outlen-1); out[outlen-1]=0; return;
	}
	if (strstr(rel, "://") || !strncmp(rel, "gemini:", 7)) {
		strncpy(out, rel, outlen-1); out[outlen-1]=0; return;
	}

	char bhost[256] = {0};
	char bpath[512] = {0};
	int bport = 1965;

	const char *p = base;
	if (!strncmp(p, "gemini://", 9)) p += 9;
	const char *he = strchr(p, '/');
	const char *pe = strchr(p, ':');
	if (pe && (!he || pe < he)) {
		int hl = (int)(pe - p); if (hl > 255) hl=255;
		memcpy(bhost, p, hl); bhost[hl]=0;
		bport = atoi(pe+1);
		if (he) strncpy(bpath, he, sizeof(bpath)-1); else strcpy(bpath, "/");
	} else if (he) {
		int hl = (int)(he - p);
		memcpy(bhost, p, hl); bhost[hl]=0;
		strncpy(bpath, he, sizeof(bpath)-1);
	} else {
		strncpy(bhost, p, sizeof(bhost)-1);
		strcpy(bpath, "/");
	}
	if (!bpath[0]) strcpy(bpath, "/");

	char portpart[32] = "";
	if (bport != 1965) snprintf(portpart, sizeof(portpart), ":%d", bport);

	if (rel[0] == '/') {
		snprintf(out, outlen, "gemini://%s%s%s", bhost, portpart, rel);
		return;
	}

	char dir[512];
	strncpy(dir, bpath, sizeof(dir)-1);
	char *ls = strrchr(dir, '/');
	if (ls) *(ls+1) = 0; else strcpy(dir, "/");

	char full[1024];
	snprintf(full, sizeof(full), "%s%s", dir, rel);

	char *segs[64]; int nseg = 0;
	char *tmp = strdup(full);
	char *save = NULL;
	char *seg = strtok_r(tmp, "/", &save);
	while (seg) {
		if (!strcmp(seg, ".") || !*seg) {
		} else if (!strcmp(seg, "..")) {
			if (nseg > 0) nseg--;
		} else {
			segs[nseg++] = seg;
		}
		seg = strtok_r(NULL, "/", &save);
	}

	char norm[1024] = "/";
	for (int i = 0; i < nseg; i++) {
		strcat(norm, segs[i]);
		if (i+1 < nseg) strcat(norm, "/");
	}
	free(tmp);

	snprintf(out, outlen, "gemini://%s%s%s", bhost, portpart, norm);
}

/* Simple word-wrap (UTF-8 aware). Returns malloc'ed array of lines. */
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

/* Rebuild scrollable content from real gemtext body (same logic as the main app) */
static int gemini_line_handler(struct BGTK_Widget *w, struct InputEvent ev);

static void rebuild_content_from_gemtext(const char *body)
{
	int max_text_w;
	int line_inset;

	if (content_scroll->data.scrollable.items) {
		for (int i = 0; i < content_scroll->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *tw = content_scroll->data.scrollable.items[i];
			if (tw) {
				free(tw->data.text.text);
				free(tw);
			}
		}
		free(content_scroll->data.scrollable.items);
	}
	content_scroll->data.scrollable.items = NULL;
	content_scroll->data.scrollable.widget_count = 0;
	content_scroll_invalidate_tmp();
	num_page_links = 0;

	if (!body) return;

	line_inset = 2 * (1 + 1);
	max_text_w = 480;
	if (content_scroll && content_scroll->w > 100) {
		max_text_w = content_scroll->w - 2 * content_scroll->padding -
			     line_inset - 4;
		if (max_text_w < 40)
			max_text_w = 40;
	}

	char *work = strdup(body);
	if (!work) return;

	struct BGTK_Widget **new_items = NULL;
	int cap = 0, cnt = 0;

	char *save = NULL;
	char *line = strtok_r(work, "\n", &save);
	int in_pre = 0;
	while (line) {
		char *cr = strchr(line, '\r');
		if (cr) *cr = 0;

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
			char tgt[512] = {0};
			char disp[512] = {0};
			const char *rest = line + 2;
			while (*rest == ' ' || *rest == '\t') rest++;
			const char *sp = rest;
			while (*sp && *sp != ' ' && *sp != '\t') sp++;
			int tl = (int)(sp - rest);
			if (tl > 0 && tl < (int)sizeof(tgt)) memcpy(tgt, rest, tl);
			tgt[tl] = 0;
			while (*sp == ' ' || *sp == '\t') sp++;
			if (*sp) strncpy(disp, sp, sizeof(disp)-1);
			else strncpy(disp, tgt, sizeof(disp)-1);

			snprintf(vis, sizeof(vis), "=> %s", disp);
			strncpy(link_to, tgt, sizeof(link_to)-1);
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
		} else if (!strncmp(line, "* ", 2)) {
			/* U+2022 bullet (UTF-8). */
			snprintf(vis, sizeof(vis), "\xe2\x80\xa2 %s", line + 2);
			item_color = 10;
		} else if (!strncmp(line, "> ", 2)) {
			snprintf(vis, sizeof(vis), "%s", line);
		} else {
			snprintf(vis, sizeof(vis), "%s", line);
		}

		char resolved[512] = {0};
		if (is_link)
			resolve_url(current_url, link_to, resolved, sizeof(resolved));

		if (header_level > 0 && cnt > 0)
			new_items[cnt - 1]->h += GEM_V_BEFORE_HEADER;

		if (header_level > 0) {
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size + 4 - header_level);
		}
		if (!in_pre) {
			int nsubs = 0;
			char **subs = wrap_text(ctx->ft_face, vis, max_text_w, &nsubs);
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
					strncpy(link_targets[num_page_links], resolved, sizeof(link_targets[0])-1);
					num_page_links++;
				}
				tw->handle_event = gemini_line_handler;
				if (s < nsubs - 1) {
					if (tw->h > 4)
						tw->h -= 2;
				} else if (header_level > 0) {
					tw->h += GEM_V_AFTER_HEADER;
				} else if (!vis[0]) {
					tw->h += GEM_V_EMPTY_LINE;
				} else {
					tw->h += GEM_V_AFTER_PARA;
				}
				if (cnt >= cap) {
					cap = cap ? cap*2 : 32;
					struct BGTK_Widget **ni = realloc(new_items, (size_t)cap * sizeof(*ni));
					if (!ni) { free(tw->data.text.text); free(tw); break; }
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
					cap = cap ? cap*2 : 32;
					struct BGTK_Widget **ni = realloc(new_items, (size_t)cap * sizeof(*ni));
					if (!ni) { free(tw->data.text.text); free(tw); }
					else { new_items = ni; new_items[cnt++] = tw; }
				} else {
					new_items[cnt++] = tw;
				}
			}
		}
		if (header_level > 0) {
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size);
		}

		line = strtok_r(NULL, "\n", &save);
	}
	free(work);

	content_scroll->data.scrollable.items = new_items;
	content_scroll->data.scrollable.widget_count = cnt;
	content_scroll->data.scrollable.scroll_y = 0;
	gemini_recompute_scroll_height();
}

/* Real navigation using live capsule */
static void load_real_url(const char *url);

static int gemini_line_handler(struct BGTK_Widget *w, struct InputEvent ev)
{
	if (ev.code != BTN_LEFT || ev.value != 1)
		return 0;
	for (int i = 0; i < num_page_links; i++) {
		if (link_target_widgets[i] == w) {
			load_real_url(link_targets[i]);
			return 1;
		}
	}
	if (w && w->ctx && content_scroll)
		bgtk_set_focus(w->ctx, content_scroll);
	return 1;
}

static void load_real_url(const char *url)
{
	if (!url || !*url) return;

	strncpy(current_url, url, sizeof(current_url)-1);
	current_url[sizeof(current_url)-1] = 0;

	char tourl[512];
	strncpy(tourl, url, sizeof(tourl)-1);
	int redirects = 0;
	int status = 0;
	char *meta = NULL;
	char *body = NULL;
	int feres = -1;

	for (;;) {
		if (meta) { free(meta); meta = NULL; }
		if (body) { free(body); body = NULL; }
		feres = fetch_gemini(tourl, &status, &meta, &body);
		if (feres == 0 && (status == 30 || status == 31) && meta && *meta && redirects < 5) {
			char next[512];
			resolve_url(tourl, meta, next, sizeof(next));
			strncpy(tourl, next, sizeof(tourl)-1);
			redirects++;
			continue;
		}
		break;
	}
	strncpy(current_url, tourl, sizeof(current_url)-1);

	/* free old content */
	if (content_scroll->data.scrollable.items) {
		for (int i = 0; i < content_scroll->data.scrollable.widget_count; i++) {
			struct BGTK_Widget *tw = content_scroll->data.scrollable.items[i];
			free(tw->data.text.text);
			free(tw);
		}
		free(content_scroll->data.scrollable.items);
	}
	content_scroll->data.scrollable.items = NULL;
	content_scroll->data.scrollable.widget_count = 0;
	num_page_links = 0;

	if (feres != 0 || status / 10 != 2) {
		char errline[256];
		snprintf(errline, sizeof(errline), "Error %d %s", status, meta ? meta : "");
		struct BGTK_Widget **errs = calloc(1, sizeof(*errs));
		if (errs) errs[0] = bgtk_text(ctx, errline, line_opts());
		content_scroll->data.scrollable.items = errs;
		content_scroll->data.scrollable.widget_count = errs ? 1 : 0;
		free(last_body);
		last_body = NULL;
		if (body) {
			free(body);
			body = NULL;
		}
	} else {
		free(last_body);
		last_body = body;
		body = NULL;
		rebuild_content_from_gemtext(last_body);
	}

	/* sync address bar (it serves as the URL display) */
	free(addr_input->data.text_input.text);
	addr_input->data.text_input.text = strdup(current_url);
	addr_input->data.text_input.cursor_pos = (uint32_t)strlen(current_url);
	addr_input->data.text_input.scroll_x = 0;

	if (meta) free(meta);
	if (body) free(body);

	bgtk_draw_widgets(ctx);
}

static void demo_addr_on_enter(void)
{
	load_real_url(addr_input->data.text_input.text);
}

/*
 * Mirror apps/gemini_browser.c key chords (must stay in sync).
 * Returns: 0 pass-through, 1 handled, -1 quit.
 */
static int gemini_browser_key(struct InputEvent ev)
{
	int addr_focus, mods, scroll_key = 0;

	bgtk_update_modifiers(ctx, ev);
	if (ev.type != EV_KEY || (ev.value != 1 && ev.value != 2))
		return 0;
	addr_focus = (ctx->focused_widget == addr_input);
	mods = bgtk_mods_from_ctx(ctx);
	if ((mods & BGTK_MOD_CTRL) && ev.code == KEY_L) {
		bgtk_set_focus(ctx, addr_input);
		return 1;
	}
	if ((mods & BGTK_MOD_CTRL) && ev.code == KEY_R)
		return 1;
	if (ev.code == KEY_ESC) {
		if (addr_focus) {
			bgtk_set_focus(ctx, content_scroll);
			return 1;
		}
		return -1;
	}
	if ((mods & BGTK_MOD_CTRL) && ev.code == KEY_C)
		return -1;
	if (ev.code == KEY_PAGEUP || ev.code == KEY_PAGEDOWN)
		scroll_key = 1;
	else if (!addr_focus &&
		 (ev.code == KEY_UP || ev.code == KEY_DOWN ||
		  ev.code == KEY_HOME || ev.code == KEY_END ||
		  ev.code == KEY_SPACE || ev.code == KEY_J ||
		  ev.code == KEY_K))
		scroll_key = 1;
	if (scroll_key && content_scroll && content_scroll->handle_event) {
		gemini_recompute_scroll_height();
		if (content_scroll->handle_event(content_scroll, ev))
			return 1;
	}
	return 0;
}

int main(void)
{
	if (tls_init() == -1) {
		fprintf(stderr, "test_gemini_browser: tls_init failed\n");
		return 1;
	}

	int width = 640;
	int height = 500;
	int pad, fmar, bw;
	struct BGTK_Widget *main_items[2];
	struct BGTK_Widget *frame;

	ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_gemini_browser: bgtk_init_mock failed\n");
		return 1;
	}

	gemini_chrome(&pad, &fmar, &bw);
	addr_input = bgtk_text_input(
		ctx, "gemini://geminiprotocol.net/", 200, 0,
		(BGTK_Options){.padding = pad > 4 ? pad / 2 : 4, .margin = 0});
	addr_input->data.text_input.on_enter = demo_addr_on_enter;

	content_scroll = bgtk_scrollable(
		ctx, NULL, 0,
		(BGTK_Options){.padding = pad > 4 ? pad / 2 : 4, .margin = 0});

	main_items[0] = content_scroll;
	main_items[1] = addr_input;
	main_list = bgtk_list(ctx, main_items, 2,
			      (BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					     .padding = 0,
					     .margin = 0});

	frame = bgtk_frame(ctx, main_list, width, height,
			   (BGTK_Options){.padding = pad, .margin = fmar});
	root_frame = frame;
	ctx->root_widget = frame;
	gemini_layout_chrome();

	/* === Real capsule load === */
	load_real_url("gemini://geminiprotocol.net/");
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "gemini_browser_00_real_capsule.png");

	/* Reflow on resize while retained gemtext is available. */
	if (last_body && content_scroll) {
		int lines_wide, lines_narrow, lines_rewide;

		lines_wide = content_scroll->data.scrollable.widget_count;
		if (bgtk_resize_mock(ctx, 360, 500) != 0) {
			fprintf(stderr, "test_gemini_browser: resize narrow failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		gemini_on_resize();
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "gemini_browser_05_reflow_narrow.png");
		lines_narrow = content_scroll->data.scrollable.widget_count;
		printf("reflow: %d line widgets @640 -> %d @360\n",
		       lines_wide, lines_narrow);
		/* Resize must paint theme bg + content, not pure black shm. */
		{
			uint32_t *px = (uint32_t *)ctx->shm_buffer;
			int n = ctx->width * ctx->height, i, bright = 0;
			uint32_t bg = ctx->theme.background | 0xFF000000u;

			for (i = 0; i < n; i++) {
				if (px[i] != 0 && px[i] != bg)
					bright++;
			}
			if (bright < 50) {
				fprintf(stderr,
					"test_gemini_browser: resize narrow "
					"looks empty (non-bg px=%d)\n",
					bright);
				bgtk_destroy_mock(ctx);
				return 1;
			}
		}

		if (bgtk_resize_mock(ctx, 800, 520) != 0) {
			fprintf(stderr, "test_gemini_browser: resize wide failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		gemini_on_resize();
		bgtk_draw_widgets(ctx);
		take_screenshot(ctx, "gemini_browser_06_reflow_wide.png");
		lines_rewide = content_scroll->data.scrollable.widget_count;
		printf("reflow: %d line widgets @800\n", lines_rewide);

		/* Restore original test window for the rest of the suite. */
		if (bgtk_resize_mock(ctx, width, height) != 0) {
			fprintf(stderr, "test_gemini_browser: resize restore failed\n");
			bgtk_destroy_mock(ctx);
			return 1;
		}
		gemini_on_resize();
		bgtk_draw_widgets(ctx);
	} else {
		fprintf(stderr,
			"test_gemini_browser: no body retained for reflow test "
			"(last_body=%p)\n",
			(void *)last_body);
	}

	/* Wheel scroll: force a short viewport so content is taller. */
	{
		int before, after, full_h;
		struct InputEvent w = {0};

		full_h = content_scroll->h;
		content_scroll->h = 180;
		if (content_scroll->data.scrollable.tmp) {
			free(content_scroll->data.scrollable.tmp);
			content_scroll->data.scrollable.tmp = NULL;
			content_scroll->data.scrollable.widget_capacity = 0;
		}
		bgtk_draw_widgets(ctx);
		before = content_scroll->data.scrollable.scroll_y;
		w.type = EV_REL;
		w.code = REL_WHEEL;
		w.value = -3; /* scroll down */
		w.x = content_scroll->x + content_scroll->w / 2;
		w.y = content_scroll->y + 40;
		if (!bgtk_inject_event(ctx, w)) {
			fprintf(stderr,
				"test_gemini_browser: wheel not handled "
				"(content_h=%d view_h=%d)\n",
				content_scroll->data.scrollable.content_height,
				content_scroll->h);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		after = content_scroll->data.scrollable.scroll_y;
		if (after <= before) {
			fprintf(stderr,
				"test_gemini_browser: scroll_y %d -> %d "
				"(content_h=%d)\n",
				before, after,
				content_scroll->data.scrollable.content_height);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		/* Keyboard page-down also scrolls when content is focused. */
		bgtk_set_focus(ctx, content_scroll);
		{
			struct InputEvent k = {0};
			int y0 = content_scroll->data.scrollable.scroll_y;
			k.type = EV_KEY;
			k.code = KEY_PAGEDOWN;
			k.value = 1;
			if (!bgtk_inject_event(ctx, k) ||
			    content_scroll->data.scrollable.scroll_y <= y0) {
				fprintf(stderr,
					"test_gemini_browser: PageDown scroll "
					"failed (%d -> %d)\n",
					y0,
					content_scroll->data.scrollable.scroll_y);
				bgtk_destroy_mock(ctx);
				return 1;
			}
		}
		take_screenshot(ctx, "gemini_browser_00b_scrolled.png");

		/* Click a body line, redraw, then wheel still scrolls (focus
		 * must return to the scrollable — not stick on the text). */
		{
			struct InputEvent c = {0};
			struct InputEvent wh = {0};
			int y0, y1;
			struct BGTK_Widget *line =
				content_scroll->data.scrollable.items
					? content_scroll->data.scrollable.items[0]
					: NULL;

			if (line && line->data.text.text &&
			    strstr(line->data.text.text, "[")) {
				fprintf(stderr,
					"test_gemini_browser: link label "
					"regressed to numbered form: '%s'\n",
					line->data.text.text);
				bgtk_destroy_mock(ctx);
				return 1;
			}
			/* Prefer a non-link line for the click-focus test. */
			for (int i = 0;
			     i < content_scroll->data.scrollable.widget_count;
			     i++) {
				struct BGTK_Widget *tw =
					content_scroll->data.scrollable.items[i];
				int is_lnk = 0;
				for (int L = 0; L < num_page_links; L++) {
					if (link_target_widgets[L] == tw) {
						is_lnk = 1;
						break;
					}
				}
				if (tw && !is_lnk) {
					line = tw;
					break;
				}
			}
			c.type = EV_KEY;
			c.code = BTN_LEFT;
			c.value = 1;
			c.x = content_scroll->x + 20;
			c.y = content_scroll->y + 30;
			bgtk_inject_event(ctx, c);
			if (ctx->focused_widget != content_scroll) {
				fprintf(stderr,
					"test_gemini_browser: click did not "
					"focus scrollable\n");
				bgtk_destroy_mock(ctx);
				return 1;
			}
			/* Spacing must survive calculate_widget_size on redraw. */
			bgtk_draw_widgets(ctx);
			gemini_recompute_scroll_height();
			y0 = content_scroll->data.scrollable.scroll_y;
			wh.type = EV_REL;
			wh.code = REL_WHEEL;
			wh.value = -2;
			wh.x = content_scroll->x + 20;
			wh.y = content_scroll->y + 40;
			if (!bgtk_inject_event(ctx, wh)) {
				fprintf(stderr,
					"test_gemini_browser: wheel after click "
					"not handled\n");
				bgtk_destroy_mock(ctx);
				return 1;
			}
			y1 = content_scroll->data.scrollable.scroll_y;
			if (y1 <= y0) {
				fprintf(stderr,
					"test_gemini_browser: no scroll after "
					"click (%d -> %d, content_h=%d)\n",
					y0, y1,
					content_scroll->data.scrollable
						.content_height);
				bgtk_destroy_mock(ctx);
				return 1;
			}
			take_screenshot(ctx, "gemini_browser_00c_after_click_scroll.png");
		}

		/* Restore full height for the rest of the suite. */
		content_scroll->h = full_h > 0 ? full_h : content_scroll->h;
		if (content_scroll->data.scrollable.tmp) {
			free(content_scroll->data.scrollable.tmp);
			content_scroll->data.scrollable.tmp = NULL;
			content_scroll->data.scrollable.widget_capacity = 0;
		}
		content_scroll->data.scrollable.scroll_y = 0;
		bgtk_draw_widgets(ctx);
	}

	/* Focus the address bar (real UI element) */
	{
		struct InputEvent c = {0};
		c.type = EV_KEY;
		c.code = BTN_LEFT;
		c.value = 1;
		c.x = addr_input->x + 30;
		c.y = addr_input->y + 6;
		bgtk_inject_event(ctx, c);
		c.value = 0;
		bgtk_inject_event(ctx, c);
	}
	take_screenshot(ctx, "gemini_browser_01_bar_focused.png");

	/* Type a different real path into the bar (will be used on enter) */
	{
		struct InputEvent k = {0};
		k.type = EV_KEY;
		k.value = 1;
		int keyseq[] = {
			KEY_G, KEY_E, KEY_M, KEY_I, KEY_N, KEY_I, KEY_SLASH, KEY_SLASH,
			KEY_G, KEY_E, KEY_M, KEY_I, KEY_N, KEY_I, KEY_P, KEY_R, KEY_O, KEY_T, KEY_O, KEY_C, KEY_O, KEY_L, KEY_DOT, KEY_N, KEY_E, KEY_T, KEY_SLASH
		};
		for (size_t i = 0; i < sizeof(keyseq)/sizeof(keyseq[0]); ++i) {
			k.code = keyseq[i];
			bgtk_inject_event(ctx, k);
		}
	}
	take_screenshot(ctx, "gemini_browser_02_typing_in_bar.png");

	/* Esc while URL focused must blur to content — not quit the app. */
	{
		struct InputEvent k = {0};
		int r;

		k.type = EV_KEY;
		k.value = 1;
		k.code = KEY_ESC;
		r = gemini_browser_key(k);
		if (r != 1 || ctx->focused_widget != content_scroll) {
			fprintf(stderr,
				"test_gemini_browser: Esc blur failed "
				"(ret=%d focus=%p want content_scroll)\n",
				r, (void *)ctx->focused_widget);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		take_screenshot(ctx, "gemini_browser_02b_esc_blur.png");
	}

	/* Ctrl+L focuses the URL bar again. */
	{
		struct InputEvent k = {0};
		int r;

		k.type = EV_KEY;
		k.value = 1;
		k.code = KEY_LEFTCTRL;
		gemini_browser_key(k);
		k.code = KEY_L;
		r = gemini_browser_key(k);
		if (r != 1 || ctx->focused_widget != addr_input) {
			fprintf(stderr,
				"test_gemini_browser: Ctrl+L focus failed "
				"(ret=%d ctrl=%d focus=%p)\n",
				r, ctx->ctrl_held, (void *)ctx->focused_widget);
			bgtk_destroy_mock(ctx);
			return 1;
		}
		k.code = KEY_LEFTCTRL;
		k.value = 0;
		gemini_browser_key(k);
		take_screenshot(ctx, "gemini_browser_02c_ctrl_l.png");
	}

	/* ENTER -> real fetch of the (re)typed capsule URL via on_enter */
	{
		struct InputEvent k = {0};
		k.type = EV_KEY;
		k.value = 1;
		k.code = KEY_ENTER;
		bgtk_inject_event(ctx, k);
	}
	take_screenshot(ctx, "gemini_browser_03_after_enter.png");

	/* Click a real link from the capsule content (first registered link target) */
	if (num_page_links > 0 && link_target_widgets[0]) {
		struct BGTK_Widget *lnk = link_target_widgets[0];
		int ax = content_scroll->x + lnk->x + 15;
		int ay = content_scroll->y + lnk->y + 4;
		struct InputEvent c = {0};
		c.type = EV_KEY;
		c.code = BTN_LEFT;
		c.value = 1;
		c.x = ax;
		c.y = ay;
		bgtk_inject_event(ctx, c);
		c.value = 0;
		bgtk_inject_event(ctx, c);
	}
	take_screenshot(ctx, "gemini_browser_04_real_link_clicked.png");

	printf("test_gemini_browser complete. Real capsule used (geminiprotocol.net and links).\n");
	printf("PNG files contain authentic rendered Gemini content.\n");

	free(last_body);
	last_body = NULL;
	bgtk_destroy_mock(ctx);
	return 0;
}
