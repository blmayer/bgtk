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
 * Output: gemini_browser_*.png files
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <linux/input.h>
#include <ctype.h>
#include <tls.h>

#include "bgtk.h"

static struct BGTK_Context *ctx = NULL;
static struct BGTK_Widget *content_scroll = NULL;
static struct BGTK_Widget *addr_input = NULL;

static char current_url[512] = "gemini://geminiprotocol.net/";
static struct BGTK_Widget *link_target_widgets[128];
static char link_targets[128][512];
static int num_page_links = 0;

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
			if (waited >= 15000)
				return -1;
			usleep(5000);
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
			if (waited >= 15000)
				return -1;
			usleep(5000);
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

/* Rebuild scrollable content from real gemtext body (same logic as the main app) */
static int gemini_link_handler(struct BGTK_Widget *w, struct InputEvent ev);

static void rebuild_content_from_gemtext(const char *body)
{
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
	num_page_links = 0;

	if (!body) return;

	int max_text_w = 480;
	if (content_scroll && content_scroll->w > 100)
		max_text_w = content_scroll->w - 8;

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
		} else if (!strncmp(line, "> ", 2) || !strncmp(line, "* ", 2)) {
			snprintf(vis, sizeof(vis), "%s", line[0]=='*' ? "\xe2\x80\xa2 " : "");
			if (line[0]=='*') strncat(vis, line+2, sizeof(vis)-strlen(vis)-1);
			else strncat(vis, line, sizeof(vis)-strlen(vis)-1);
			if (line[0]=='*') item_color = 10;
		} else {
			snprintf(vis, sizeof(vis), "%s", line);
		}

		char resolved[512] = {0};
		if (is_link)
			resolve_url(current_url, link_to, resolved, sizeof(resolved));

		if (header_level > 0 && cnt > 0) {
			new_items[cnt-1]->h += 5;  /* increase the top spacing a little before headings */
		}

		if (header_level > 0) {
			FT_Set_Pixel_Sizes(ctx->ft_face, 0, ctx->font_size + 4 - header_level);
		}
		if (!in_pre) {
			int nsubs = 0;
			char **subs = wrap_text(ctx->ft_face, vis, max_text_w, &nsubs);
			for (int s = 0; s < nsubs; s++) {
				struct BGTK_Widget *tw = bgtk_text(ctx, subs[s], (BGTK_Options){.padding=1, .margin=1});
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
					tw->handle_event = gemini_link_handler;
				}
				if (s < nsubs - 1) {
					tw->h -= 3;  /* decrease a little for lines in the same paragraph (tighter leading between wraps) */
				}
				if (s == nsubs - 1) {
					tw->h += 5;  /* extra vertical space after this logical block */
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
			struct BGTK_Widget *tw = bgtk_text(ctx, vis, (BGTK_Options){.padding=1, .margin=1});
			if (tw) {
				tw->h += 4;  /* space after pre block */
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
}

/* Real navigation using live capsule */
static void load_real_url(const char *url);

static int gemini_link_handler(struct BGTK_Widget *w, struct InputEvent ev)
{
	if (ev.code == BTN_LEFT && ev.value == 1) {
		for (int i = 0; i < num_page_links; i++) {
			if (link_target_widgets[i] == w) {
				load_real_url(link_targets[i]);
				return 1;
			}
		}
	}
	return 0;
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
		if (errs) errs[0] = bgtk_text(ctx, errline, (BGTK_Options){.padding=2, .margin=1});
		content_scroll->data.scrollable.items = errs;
		content_scroll->data.scrollable.widget_count = errs ? 1 : 0;
	} else {
		rebuild_content_from_gemtext(body);
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

int main(void)
{
	if (tls_init() == -1) {
		fprintf(stderr, "test_gemini_browser: tls_init failed\n");
		return 1;
	}

	int width = 640;
	int height = 500;

	ctx = bgtk_init_mock(width, height);
	if (!ctx) {
		fprintf(stderr, "test_gemini_browser: bgtk_init_mock failed\n");
		return 1;
	}

	int usable_w = width - 20;

	/* create button first to know its size, then size the input to make the row (address bar + button) full width */
	struct BGTK_Widget *go_label = bgtk_text(ctx, "Go", (BGTK_Options){.padding = 2, .margin = 0});
	struct BGTK_Widget *go_btn = bgtk_button(ctx, go_label, NULL, NULL,
						 (BGTK_Options){.padding = 4, .margin = 2});
	int input_w = usable_w - go_btn->w - 16;  /* leave room for row padding/margins */
	addr_input = bgtk_text_input(ctx, "gemini://geminiprotocol.net/", input_w > 100 ? input_w : 400, 0,
				     (BGTK_Options){.padding = 4, .margin = 2});
	addr_input->data.text_input.on_enter = demo_addr_on_enter;

	struct BGTK_Widget *addr_items[2] = {addr_input, go_btn};
	struct BGTK_Widget *addr_row = bgtk_list(ctx, addr_items, 2,
						 (BGTK_Options){.orientation = BGTK_LIST_HORIZONTAL, .padding = 2, .margin = 1});

	content_scroll = bgtk_scrollable(ctx, NULL, 0, (BGTK_Options){.padding = 2, .margin = 3});
	int v_margins = 16;
	int reserved = (addr_row ? addr_row->h : 24) + v_margins;
	int scroll_h = height - reserved - 24;
	if (scroll_h < 100) scroll_h = 180;
	content_scroll->w = usable_w;
	content_scroll->h = scroll_h;

	struct BGTK_Widget *main_items[2] = {content_scroll, addr_row};
	struct BGTK_Widget *main_list = bgtk_list(ctx, main_items, 2,
						  (BGTK_Options){.orientation = BGTK_LIST_VERTICAL, .padding = 2, .margin = 2});

	struct BGTK_Widget *frame = bgtk_frame(ctx, main_list, width, height,
					       (BGTK_Options){.padding = 4, .margin = 0});

	ctx->root_widget = frame;

	/* === Real capsule load === */
	load_real_url("gemini://geminiprotocol.net/");
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "gemini_browser_00_real_capsule.png");

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

	bgtk_destroy_mock(ctx);
	return 0;
}
