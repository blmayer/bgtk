/* test/test_sys_status.c — headless screenshots for system status app */
#include <stdio.h>
#include <string.h>
#include <linux/input.h>
#include "bgtk.h"

extern struct BGTK_Widget *sys_status_build_ui(struct BGTK_Context *c);
extern void sys_status_refresh(unsigned flags);

int main(void)
{
	struct BGTK_Context *ctx = bgtk_init_mock(640, 400);
	struct BGTK_Widget *root;
	if (!ctx) {
		fprintf(stderr, "test_sys_status: init failed\n");
		return 1;
	}
	bgtk_log_open("test_sys_status");
	root = sys_status_build_ui(ctx);
	ctx->root_widget = root;
	/* Fit mock buffer to measured content size. */
	if (bgtk_resize_mock(ctx, root->w, root->h) != 0) {
		fprintf(stderr, "test_sys_status: resize to %dx%d failed\n",
			root->w, root->h);
		bgtk_destroy_mock(ctx);
		return 1;
	}
	/* second FAST sample so CPU % is meaningful (build_ui already sampled) */
	sys_status_refresh(1u << 1); /* REFRESH_FAST */
	/* values may have grown (CPU bar); re-fit once */
	/* build_ui already fitted after full refresh */
	bgtk_draw_widgets(ctx);
	take_screenshot(ctx, "sys_status_00_init.png");
	printf("sys_status size %dx%d\n", ctx->width, ctx->height);

	printf("test_sys_status complete.\n");
	bgtk_destroy_mock(ctx);
	return 0;
}
