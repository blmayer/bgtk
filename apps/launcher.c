#include <bgce.h>
#include <bgtk.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <unistd.h>

static struct BGTK_Context* ctx = NULL;
static struct BGTK_Widget* text_input = NULL;
static struct BGTK_Widget* matches_scroll = NULL;

static char** all_programs = NULL;
static int num_programs = 0;
static char* match_ptrs[16];
static int num_matches = 0;
static int selected = -1;
/* Set after a successful spawn so main can tear down and exit. */
static int quit_after_launch = 0;

static void load_programs(void)
{
	char* path = getenv("PATH");
	if (!path)
		path = "/usr/bin:/bin:/usr/local/bin";
	char* p = strdup(path);
	if (!p)
		return;
	char* save = NULL;
	char* dir = strtok_r(p, ":", &save);
	while (dir) {
		DIR* d = opendir(dir);
		if (d) {
			struct dirent* e;
			while ((e = readdir(d))) {
				if (e->d_name[0] == '.')
					continue;
				char fp[1024];
				if (snprintf(fp, sizeof(fp), "%s/%s", dir, e->d_name) >= (int)sizeof(fp))
					continue;
				if (access(fp, X_OK) == 0) {
					int have = 0;
					for (int k = 0; k < num_programs; k++) {
						if (strcmp(all_programs[k], e->d_name) == 0) {
							have = 1;
							break;
						}
					}
					if (!have) {
						char** tmp = realloc(all_programs, (num_programs + 1) * sizeof(char*));
						if (tmp) {
							all_programs = tmp;
							all_programs[num_programs++] = strdup(e->d_name);
						}
					}
				}
			}
			closedir(d);
		}
		dir = strtok_r(NULL, ":", &save);
	}
	free(p);
}

static void update_matches(const char* prefix)
{
	num_matches = 0;
	size_t plen = prefix ? strlen(prefix) : 0;
	int maxm = 12;
	if (plen == 0) {
		for (int i = 0; i < num_programs && num_matches < maxm; i++)
			match_ptrs[num_matches++] = all_programs[i];
	} else {
		for (int i = 0; i < num_programs && num_matches < maxm; i++) {
			if (strncasecmp(all_programs[i], prefix, plen) == 0)
				match_ptrs[num_matches++] = all_programs[i];
		}
	}
	selected = num_matches > 0 ? 0 : -1;
}

static void on_enter_pressed(void);

/* Click a match row: select + launch (file-list style). */
static void match_row_cb(void *userdata)
{
	int i = (int)(intptr_t)userdata;
	if (i < 0 || i >= num_matches)
		return;
	selected = i;
	on_enter_pressed();
}

static void rebuild_matches_ui(void)
{
	int pad, row_w, i, n;
	struct BGTK_Widget **items;

	if (!matches_scroll || !ctx)
		return;

	/* Free old rows (buttons own their label text widgets). */
	{
		int oldc = matches_scroll->data.scrollable.widget_count;
		for (i = 0; i < oldc; i++) {
			struct BGTK_Widget *row =
				matches_scroll->data.scrollable.items[i];
			if (!row)
				continue;
			if (row->type == BGTK_WIDGET_BUTTON &&
			    row->data.button.label) {
				struct BGTK_Widget *lab = row->data.button.label;
				if (lab->type == BGTK_WIDGET_TEXT &&
				    lab->data.text.text)
					free(lab->data.text.text);
				free(lab);
			} else if (row->type == BGTK_WIDGET_TEXT &&
				   row->data.text.text) {
				free(row->data.text.text);
			}
			free(row);
		}
		free(matches_scroll->data.scrollable.items);
		matches_scroll->data.scrollable.items = NULL;
		matches_scroll->data.scrollable.widget_count = 0;
		if (matches_scroll->data.scrollable.tmp) {
			free(matches_scroll->data.scrollable.tmp);
			matches_scroll->data.scrollable.tmp = NULL;
			matches_scroll->data.scrollable.widget_capacity = 0;
		}
	}

	n = num_matches;
	if (n < 1) {
		bgtk_draw_widgets(ctx);
		return;
	}

	items = calloc((size_t)n, sizeof(struct BGTK_Widget *));
	if (!items)
		return;

	/* Tight list: pad only inside rows; zero margin so rows pack flush. */
	pad = ctx->theme.padding > 0 ? ctx->theme.padding : 6;
	if (pad > 8)
		pad = 8;
	/* Full width of the scrollable content box. */
	row_w = matches_scroll->w - 2 * matches_scroll->padding;
	if (row_w < 40)
		row_w = 400;

	/*
	 * sowm-style file list: full-width borderless rows, tight vertical
	 * packing; selected row uses theme.highlight fill.
	 */
	for (i = 0; i < n; i++) {
		struct BGTK_Widget *lab;
		struct BGTK_Widget *row;
		const char *name = match_ptrs[i];
		int sel = (i == selected);
		int vpad = 3; /* tight file-list rows */
		char lined[160];

		/* Leading spaces = left indent inside the highlight bar. */
		snprintf(lined, sizeof(lined), "  %s", name);
		lab = bgtk_text(ctx, lined,
				(BGTK_Options){.padding = 0, .margin = 0});
		if (!lab)
			continue;
		row = bgtk_button(ctx, lab, match_row_cb,
				  (void *)(intptr_t)i,
				  (BGTK_Options){.padding = vpad,
						 .margin = 0,
						 .text_align = BGTK_ALIGN_LEFT});
		if (!row) {
			free(lab->data.text.text);
			free(lab);
			continue;
		}
		row->w = row_w;
		row->h = lab->h + 2 * vpad;
		row->data.button.border_w = 0;
		if (sel)
			row->data.button.bg_override = ctx->theme.highlight
							       ? ctx->theme.highlight
							       : 0xFFD4B8A0;
		else
			row->data.button.bg_override = ctx->theme.background
							       ? ctx->theme.background
							       : 0xFF0A0A0A;
		items[i] = row;
	}

	matches_scroll->data.scrollable.items = items;
	matches_scroll->data.scrollable.widget_count = n;
	matches_scroll->data.scrollable.scroll_y = 0;

	bgtk_draw_widgets(ctx);
}

static void on_text_change(void)
{
	if (text_input)
		update_matches(text_input->data.text_input.text);
	rebuild_matches_ui();
}

static void on_tab_pressed(void)
{
	if (num_matches < 2)
		return;
	selected = (selected + 1) % num_matches;
	rebuild_matches_ui();
}

/* Drop the compositor connection so BGCE can erase this window and
 * restore the wallpaper. Must run before further draws/teardown. */
static void launcher_detach(void)
{
	if (!ctx || ctx->conn_fd < 0)
		return;
	{
		int c = ctx->conn_fd;
		ctx->conn_fd = -1;
		bgce_disconnect(c);
		bgtk_log("detached from bgce (fd=%d)", c);
	}
}

/* Spawn a program without inheriting the launcher's BGCE socket (or any
 * other fds). CLOEXEC + close-all in the child prevent a half-open
 * connection that would leave a black dead window on screen. */
static int spawn_program(const char *prog)
{
	pid_t pid;

	if (!prog || !*prog)
		return -1;

	/* Belt-and-suspenders: never let the child inherit the BGCE fd. */
	if (ctx && ctx->conn_fd >= 0) {
		int fl = fcntl(ctx->conn_fd, F_GETFD);
		if (fl >= 0)
			(void)fcntl(ctx->conn_fd, F_SETFD, fl | FD_CLOEXEC);
	}

	pid = fork();
	if (pid < 0) {
		bgtk_log_errno("fork to launch '%s'", prog);
		return -1;
	}
	if (pid == 0) {
		/* New session: not a child of the launcher process group. */
		setsid();

		/* Drop every fd the parent had open (BGCE socket, log, …). */
		{
			int maxfd = (int)sysconf(_SC_OPEN_MAX);
			int fd;

			if (maxfd < 0 || maxfd > 4096)
				maxfd = 1024;
			for (fd = 3; fd < maxfd; fd++)
				(void)close(fd);
		}

		/* Quiet stdio so GUI apps don't fight over the TTY. */
		{
			int devnull = open("/dev/null", O_RDWR);
			if (devnull >= 0) {
				dup2(devnull, STDIN_FILENO);
				dup2(devnull, STDOUT_FILENO);
				dup2(devnull, STDERR_FILENO);
				if (devnull > 2)
					close(devnull);
			}
		}

		execlp(prog, prog, (char *)NULL);
		/* exec failed — cannot log (fds closed); hard-exit. */
		_exit(127);
	}
	bgtk_log("spawned '%s' as pid=%ld; launcher will exit", prog, (long)pid);
	return 0;
}

static void on_enter_pressed(void)
{
	if (selected < 0 || selected >= num_matches)
		return;
	if (spawn_program(match_ptrs[selected]) != 0)
		return;
	quit_after_launch = 1;
	/* Disconnect now so the server erases us before any post-enter
	 * redraw (text_input always draws after on_enter) or free path. */
	launcher_detach();
}

/* Size input + match list to fill the current window. */
static void layout_launcher(void)
{
	int pad, mar, bw, inner_w, inner_h, input_h, gap;

	if (!ctx)
		return;
	pad = ctx->theme.padding > 0 ? ctx->theme.padding : 6;
	mar = ctx->theme.margin > 0 ? ctx->theme.margin : 4;
	bw = (int)ctx->theme.frame_border_size;
	if (bw < 0)
		bw = 0;
	/* Content lives inside frame border + frame padding only (no extra mar). */
	inner_w = ctx->width - 2 * (bw + pad);
	inner_h = ctx->height - 2 * (bw + pad);
	if (inner_w < 40)
		inner_w = 40;
	if (inner_h < 40)
		inner_h = 40;
	gap = mar; /* space between search box and list */

	if (text_input) {
		text_input->w = inner_w;
		text_input->margin = 0;
		if (text_input->h < 28)
			text_input->h = 28;
	}
	if (matches_scroll) {
		input_h = text_input ? text_input->h + gap : 36;
		matches_scroll->w = inner_w;
		matches_scroll->h = inner_h - input_h;
		matches_scroll->margin = 0;
		matches_scroll->padding = 0;
		if (matches_scroll->h < 40)
			matches_scroll->h = 40;
	}
	if (ctx->root_widget) {
		ctx->root_widget->w = ctx->width;
		ctx->root_widget->h = ctx->height;
	}
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	setvbuf(stderr, NULL, _IONBF, 0);

	setvbuf(stderr, NULL, _IONBF, 0);
	bgtk_log_open("launcher");
	/* Avoid zombies from spawned apps. */
	signal(SIGCHLD, SIG_IGN);
	bgtk_log("loading PATH programs");
	load_programs();
	bgtk_log("programs scanned: %d", num_programs);

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect (is bgce running?)");
		return -1;
	}
	/* Children must not inherit the compositor socket — if they do,
	 * parent close() is not a full disconnect and the window stays
	 * black until the child exits. */
	{
		int fl = fcntl(conn_fd, F_GETFD);
		if (fl >= 0)
			(void)fcntl(conn_fd, F_SETFD, fl | FD_CLOEXEC);
	}
	bgtk_log("bgce_connect ok fd=%d", conn_fd);

	int width = 480;
	int height = 320;
	struct BufferRequest req = {.width = width, .height = height};
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return -3;
	}
	bgtk_log("bgce_get_buffer ok %p", buffer);

	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed — check fonts / log above");
		return 1;
	}
	bgtk_log("building launcher UI (goldie file-list)");

	{
		int pad = ctx->theme.padding > 0 ? ctx->theme.padding : 6;
		int mar = ctx->theme.margin > 0 ? ctx->theme.margin : 4;

		text_input = bgtk_text_input(
			ctx, "", 440, 0,
			(BGTK_Options){.padding = pad, .margin = 0});
		if (!text_input) {
			bgtk_log("bgtk_text_input failed");
			return 1;
		}
		text_input->data.text_input.on_change = on_text_change;
		text_input->data.text_input.on_tab = on_tab_pressed;
		text_input->data.text_input.on_enter = on_enter_pressed;

		/* Zero pad/margin so list rows can be edge-to-edge. */
		matches_scroll = bgtk_scrollable(
			ctx, NULL, 0,
			(BGTK_Options){.padding = 0, .margin = 0});
		if (!matches_scroll) {
			bgtk_log("bgtk_scrollable failed");
			return 1;
		}

		{
			struct BGTK_Widget *layout_items[2] = { text_input,
							       matches_scroll };
			struct BGTK_Widget *layout = bgtk_list(
				ctx, layout_items, 2,
				(BGTK_Options){.orientation = BGTK_LIST_VERTICAL,
					       .margin = mar,
					       .padding = 0});
			struct BGTK_Widget *frame;

			if (!layout) {
				bgtk_log("bgtk_list failed");
				return 1;
			}
			/* Frame pad = theme.padding so thick border has even inset. */
			frame = bgtk_frame(ctx, layout, width, height,
					  (BGTK_Options){.padding = pad,
							 .margin = 0});
			if (!frame) {
				bgtk_log("bgtk_frame failed");
				return 1;
			}
			ctx->root_widget = frame;
		}
	}

	layout_launcher();
	bgtk_log("first draw / focus");
	bgtk_log_flush();
	/* Draw before set_focus: set_focus also draws; if draw crashes we know. */
	bgtk_draw_widgets(ctx);
	bgtk_log("first draw ok");
	bgtk_set_focus(ctx, text_input);

	update_matches("");
	bgtk_log("matches=%d; rebuilding list", num_matches);
	rebuild_matches_ui();

	bgtk_log("Starting launcher main loop (%dx%d)", ctx->width, ctx->height);
	bgtk_log_flush();

	int quit = 0;
	struct BGCEMessage msg;
	ssize_t bytes;
	while (!quit && !quit_after_launch) {
		bytes = bgce_recv_msg(ctx->conn_fd, &msg);
		if (bytes <= 0) {
			if (bytes == 0)
				bgtk_log("server closed connection");
			else if (errno != EINTR)
				bgtk_log_errno("bgce_recv_msg");
			break;
		}

		int need_draw = 0;
		switch (msg.type) {
		case MSG_INPUT_EVENT: {
			struct InputEvent *ev = &msg.data.input_event;

			/* Allow wheel scroll on the match list. */
			if (ev->type == EV_ABS)
				break;
			if (ev->type == EV_REL && ev->code != REL_WHEEL)
				break;
			bgtk_update_modifiers(ctx, *ev);
			if (bgtk_is_app_quit_event(ctx, *ev)) {
				quit = 1;
				break;
			}
			/* Arrow keys move selection in the file list. */
			if (ev->type == EV_KEY &&
			    (ev->value == 1 || ev->value == 2) &&
			    num_matches > 0) {
				if (ev->code == KEY_DOWN ||
				    ev->code == KEY_J) {
					selected = (selected + 1) % num_matches;
					rebuild_matches_ui();
					break;
				}
				if (ev->code == KEY_UP || ev->code == KEY_K) {
					selected = (selected - 1 + num_matches) %
						   num_matches;
					rebuild_matches_ui();
					break;
				}
			}
			need_draw = bgtk_handle_input_event(ctx, *ev);
			break;
		}
		case MSG_FOCUS_CHANGE:
			bgtk_set_window_focus(ctx, msg.data.focus_event.state);
			need_draw = 1;
			break;
		case MSG_BUFFER_CHANGE:
			/* Server allocated a new shm buffer after a window resize. */
			if (bgtk_handle_buffer_change(ctx, &msg.data.buffer_reply) == 0) {
				layout_launcher();
				need_draw = 1;
			}
			break;
		default:
			break;
		}

		if (need_draw && !quit_after_launch && ctx->conn_fd >= 0)
			bgtk_draw_widgets(ctx);
	}

	/* Disconnect first (if not already after launch), then free local
	 * resources. Order matters: munmap-before-disconnect leaves the
	 * server holding a dead client until close, and a late draw can
	 * paint a black frame over the wallpaper. */
	bgtk_log("launcher shutting down (launch=%d)", quit_after_launch);
	launcher_detach();
	bgtk_destroy(ctx);
	ctx = NULL;
	return 0;
}
