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

static void rebuild_matches_ui(void)
{
	if (!matches_scroll)
		return;

	int oldc = matches_scroll->data.scrollable.widget_count;
	for (int i = 0; i < oldc; i++) {
		struct BGTK_Widget* tw = matches_scroll->data.scrollable.items[i];
		if (tw) {
			free(tw->data.text.text);
			free(tw);
		}
	}
	free(matches_scroll->data.scrollable.items);
	matches_scroll->data.scrollable.items = NULL;
	matches_scroll->data.scrollable.widget_count = 0;

	int n = num_matches;
	if (n < 1) {
		bgtk_draw_widgets(ctx);
		return;
	}

	struct BGTK_Widget** items = calloc(n, sizeof(struct BGTK_Widget*));
	if (!items)
		return;

	for (int i = 0; i < n; i++) {
		char label[128];
		const char* name = match_ptrs[i];
		if (i == selected)
			snprintf(label, sizeof(label), "\xe2\x96\xb6 %s", name);
		else
			snprintf(label, sizeof(label), "  %s", name);
		items[i] = bgtk_text(ctx, label, (BGTK_Options){.padding = 1, .margin = 0});
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

/* Spawn a program without inheriting the launcher's BGCE socket (or any
 * other fds). Inheriting the compositor connection and then exit()'ing the
 * parent was crashing the session when launching apps. */
static void spawn_program(const char *prog)
{
	pid_t pid = fork();
	if (pid < 0) {
		bgtk_log_errno("fork to launch '%s'", prog);
		return;
	}
	if (pid == 0) {
		/* New session: not a child of the launcher process group. */
		setsid();

		/* Drop every fd the parent had open (BGCE socket, shm, …). */
		int maxfd = (int)sysconf(_SC_OPEN_MAX);
		if (maxfd < 0 || maxfd > 1024)
			maxfd = 256;
		for (int fd = 3; fd < maxfd; fd++)
			(void)close(fd);

		/* Quiet stdio so GUI apps don't fight over the TTY. */
		int devnull = open("/dev/null", O_RDWR);
		if (devnull >= 0) {
			dup2(devnull, STDIN_FILENO);
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > 2)
				close(devnull);
		}

		execlp(prog, prog, (char *)NULL);
		/* exec failed — cannot log (fds closed); hard-exit. */
		_exit(127);
	}
	/* Parent keeps running as the launcher. */
	bgtk_log("spawned '%s' as pid=%ld", prog, (long)pid);
}

static void on_enter_pressed(void)
{
	if (selected < 0 || selected >= num_matches)
		return;
	spawn_program(match_ptrs[selected]);
}

/* Size input + match list to fill the current window. */
static void layout_launcher(void)
{
	if (!ctx)
		return;
	int pad = 8;
	int inner_w = ctx->width - 2 * pad;
	int inner_h = ctx->height - 2 * pad;
	if (inner_w < 40)
		inner_w = 40;
	if (inner_h < 40)
		inner_h = 40;

	if (text_input) {
		text_input->w = inner_w;
		/* height stays content-sized; force a sensible min */
		if (text_input->h < 28)
			text_input->h = 28;
	}
	if (matches_scroll) {
		int input_h = text_input ? text_input->h + 8 : 36;
		matches_scroll->w = inner_w;
		matches_scroll->h = inner_h - input_h;
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

	bgtk_log_open("launcher");
	/* Avoid zombies from spawned apps. */
	signal(SIGCHLD, SIG_IGN);
	load_programs();

	int conn_fd = bgce_connect();
	if (conn_fd < 0) {
		bgtk_log_errno("bgce_connect");
		return -1;
	}

	int width = 480;
	int height = 320;
	struct BufferRequest req = {.width = width, .height = height};
	void* buffer = bgce_get_buffer(conn_fd, req);
	if (!buffer) {
		bgtk_log("bgce_get_buffer %dx%d failed", width, height);
		bgce_disconnect(conn_fd);
		return -3;
	}

	ctx = bgtk_init(conn_fd, buffer, width, height);
	if (!ctx) {
		bgtk_log("bgtk_init failed");
		return 1;
	}

	text_input = bgtk_text_input(ctx, "", 440, 0, (BGTK_Options){.padding = 6, .margin = 4});
	text_input->data.text_input.on_change = on_text_change;
	text_input->data.text_input.on_tab = on_tab_pressed;
	text_input->data.text_input.on_enter = on_enter_pressed;

	matches_scroll = bgtk_scrollable(ctx, NULL, 0, (BGTK_Options){.padding = 4});

	struct BGTK_Widget* layout_items[2] = {text_input, matches_scroll};
	struct BGTK_Widget* layout = bgtk_list(ctx, layout_items, 2, (BGTK_Options){.orientation = BGTK_LIST_VERTICAL});

	struct BGTK_Widget* frame = bgtk_frame(ctx, layout, width, height, (BGTK_Options){.padding = 8});

	ctx->root_widget = frame;
	layout_launcher();
	bgtk_set_focus(ctx, text_input);

	update_matches("");
	rebuild_matches_ui();

	bgtk_log("Starting launcher main loop (%dx%d)", ctx->width, ctx->height);

	int quit = 0;
	struct BGCEMessage msg;
	ssize_t bytes;
	while (!quit) {
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
		case MSG_INPUT_EVENT:
			if (msg.data.input_event.type == EV_KEY &&
			    msg.data.input_event.code == KEY_ESC &&
			    msg.data.input_event.value == 1) {
				quit = 1;
				break;
			}
			if (msg.data.input_event.type != EV_REL &&
			    msg.data.input_event.type != EV_ABS) {
				need_draw = bgtk_handle_input_event(ctx, msg.data.input_event);
			}
			break;
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

		if (need_draw)
			bgtk_draw_widgets(ctx);
	}

	bgtk_destroy(ctx);
	return 0;
}
