#include <wayland-client.h>
#ifndef WAYLAND_H
#define WAYLAND_H

#ifdef __cplusplus
extern "C"
{
#endif

	int wayland_backend();
	void wayland_dispatch();
	void frame_done(void* data, struct wl_callback* cb, uint32_t time);

#ifdef __cplusplus
}
#endif

#endif // WAYLAND_H
