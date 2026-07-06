/* compat/bgce_stub.c
 *
 * No-op implementations of the bgce client API.
 * Linked only for "headless" / mock test binaries so that you don't
 * need the real libbgce (or a running server) just to exercise BGTK
 * widgets and dump PNG frames.
 *
 * On Linux with a real build you link the genuine objects instead.
 */

#include "bgce.h"

ssize_t bgce_send_msg(int conn, struct BGCEMessage *msg)
{
	(void)conn; (void)msg;
	return -1;
}

ssize_t bgce_recv_msg(int conn, struct BGCEMessage *msg)
{
	(void)conn; (void)msg;
	return 0;  /* simulate clean EOF so real loops would exit */
}

int bgce_connect(void)
{
	return -1;
}

int bgce_get_server_info(int fd, struct ServerInfo *out_info)
{
	(void)fd; (void)out_info;
	return -1;
}

void *bgce_get_buffer(int conn, struct BufferRequest req)
{
	(void)conn; (void)req;
	return NULL;
}

int bgce_buf_open(const char *name)
{
	(void)name;
	return -1;
}

void bgce_buf_unlink(const char *name)
{
	(void)name;
}

int bgce_move(int fd, int x, int y)
{
	(void)fd; (void)x; (void)y;
	return -1;
}

int bgce_draw(int fd)
{
	(void)fd;
	return 0;  /* success, but does nothing in mock */
}

void bgce_disconnect(int fd)
{
	(void)fd;
}
