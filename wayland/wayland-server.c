/*
 * Copyright © 2008 Kristian Høgsberg
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that copyright
 * notice and this permission notice appear in supporting documentation, and
 * that the name of the copyright holders not be used in advertising or
 * publicity pertaining to distribution of the software without specific,
 * written prior permission.  The copyright holders make no representations
 * about the suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * THE COPYRIGHT HOLDERS DISCLAIM ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL THE COPYRIGHT HOLDERS BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE
 * OF THIS SOFTWARE.
 */

#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdarg.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <dlfcn.h>
#include <assert.h>
#include <sys/time.h>
#include <ffi.h>

#include "wayland-server.h"
#include "wayland-server-protocol.h"
#include "connection.h"

struct wl_socket {
	int fd;
	struct sockaddr_un addr;
	struct wl_list link;
};

struct wl_client {
	struct wl_connection *connection;
	struct wl_event_source *source;
	struct wl_display *display;
	struct wl_list resource_list;
	uint32_t id_count;
};

struct wl_display {
	struct wl_object object;
	struct wl_event_loop *loop;
	struct wl_hash_table *objects;
	int run;

	struct wl_list frame_list;
	uint32_t client_id_range;
	uint32_t id;

	struct wl_list global_list;
	struct wl_list socket_list;
};

struct wl_frame_listener {
	struct wl_resource resource;
	struct wl_client *client;
	uint32_t key;
	struct wl_list link;
};

struct wl_global {
	struct wl_object *object;
	wl_client_connect_func_t func;
	struct wl_list link;
};

static int wl_debug = 0;

WL_EXPORT void
wl_client_post_event(struct wl_client *client, struct wl_object *sender,
		     uint32_t opcode, ...)
{
	struct wl_closure *closure;
	va_list ap;
	assert(client != NULL && sender != NULL &&
	       "wl_client_post_event called with NULL value!");
	if (client == NULL || sender == NULL)
		return;

	va_start(ap, opcode);
	closure = wl_connection_vmarshal(client->connection,
					 sender, opcode, ap,
					 &sender->interface->events[opcode]);
	va_end(ap);

	wl_closure_send(closure, client->connection);

	if (wl_debug) {
		fprintf(stderr, " -> ");
		wl_closure_print(closure, sender);
	}

	wl_closure_destroy(closure);
}

static void
wl_client_connection_data(int fd, uint32_t mask, void *data)
{
	struct wl_client *client = data;
	struct wl_connection *connection = client->connection;
	struct wl_object *object;
	struct wl_closure *closure;
	const struct wl_message *message;
	uint32_t p[2], opcode, size;
	uint32_t cmask = 0;
	int len;

	if (mask & WL_EVENT_READABLE)
		cmask |= WL_CONNECTION_READABLE;
	if (mask & WL_EVENT_WRITEABLE)
		cmask |= WL_CONNECTION_WRITABLE;

	len = wl_connection_data(connection, cmask);
	if (len < 0) {
		wl_client_destroy(client);
		return;
	}

	while (len >= sizeof p) {
		wl_connection_copy(connection, p, sizeof p);
		opcode = p[1] & 0xffff;
		size = p[1] >> 16;
		if (len < size)
			break;

		object = wl_hash_table_lookup(client->display->objects, p[0]);
		if (object == NULL) {
			wl_client_post_event(client, &client->display->object,
					     WL_DISPLAY_INVALID_OBJECT, p[0]);
			wl_connection_consume(connection, size);
			len -= size;
			continue;
		}

		if (opcode >= object->interface->method_count) {
			wl_client_post_event(client, &client->display->object,
					     WL_DISPLAY_INVALID_METHOD, p[0], opcode);
			wl_connection_consume(connection, size);
			len -= size;
			continue;
		}

		message = &object->interface->methods[opcode];
		closure = wl_connection_demarshal(client->connection, size,
						  client->display->objects,
						  message);
		len -= size;

		if (closure == NULL && errno == EINVAL) {
			wl_client_post_event(client, &client->display->object,
					     WL_DISPLAY_INVALID_METHOD,
					     p[0], opcode);
			continue;
		} else if (closure == NULL && errno == ENOMEM) {
			wl_client_post_no_memory(client);
			continue;
		}


		if (wl_debug)
			wl_closure_print(closure, object);

		wl_closure_invoke(closure, object,
				  object->implementation[opcode], client);

		wl_closure_destroy(closure);
	}
}

static int
wl_client_connection_update(struct wl_connection *connection,
			    uint32_t mask, void *data)
{
	struct wl_client *client = data;
	uint32_t emask = 0;

	if (mask & WL_CONNECTION_READABLE)
		emask |= WL_EVENT_READABLE;
	if (mask & WL_CONNECTION_WRITABLE)
		emask |= WL_EVENT_WRITEABLE;

	return wl_event_source_fd_update(client->source, emask);
}

WL_EXPORT struct wl_display *
wl_client_get_display(struct wl_client *client)
{
	assert(client != NULL &&
	       "wl_client_get_display called with NULL value!");
	if (client == NULL)
		return NULL;

	return client->display;
}

static void
wl_display_post_range(struct wl_display *display, struct wl_client *client)
{
	wl_client_post_event(client, &client->display->object,
			     WL_DISPLAY_RANGE, display->client_id_range);
	display->client_id_range += 256;
	client->id_count += 256;
}

static struct wl_client *
wl_client_create(struct wl_display *display, int fd)
{
	struct wl_client *client;
	struct wl_global *global;

	client = malloc(sizeof *client);
	if (client == NULL)
		return NULL;

	memset(client, 0, sizeof *client);
	client->display = display;
	client->source = wl_event_loop_add_fd(display->loop, fd,
					      WL_EVENT_READABLE,
					      wl_client_connection_data, client);
	client->connection =
		wl_connection_create(fd, wl_client_connection_update, client);

	wl_list_init(&client->resource_list);

	wl_display_post_range(display, client);

	wl_list_for_each(global, &display->global_list, link)
		wl_client_post_event(client, &client->display->object,
				     WL_DISPLAY_GLOBAL,
				     global->object,
				     global->object->interface->name,
				     global->object->interface->version);

	wl_list_for_each(global, &display->global_list, link)
		if (global->func)
			global->func(client, global->object);

	return client;
}

WL_EXPORT void
wl_client_add_resource(struct wl_client *client,
		       struct wl_resource *resource)
{
	struct wl_display *display;

	assert(client != NULL && resource != NULL &&
	       "wl_client_add_resource called with NULL value!");
	if (client == NULL || resource == NULL)
		return;

	display = client->display;

	if (client->id_count-- < 64)
		wl_display_post_range(display, client);

	wl_hash_table_insert(client->display->objects,
			     resource->object.id, resource);
	wl_list_insert(client->resource_list.prev, &resource->link);
}

WL_EXPORT void
wl_client_post_no_memory(struct wl_client *client)
{
	assert(client != NULL &&
	       "wl_client_post_no_memory called with NULL value!");
	if (client == NULL )
		return;

	wl_client_post_event(client,
			     &client->display->object,
			     WL_DISPLAY_NO_MEMORY);
}

WL_EXPORT void
wl_client_post_global(struct wl_client *client, struct wl_object *object)
{
	assert(client != NULL && object != NULL &&
	       "wl_client_post_global called with NULL value!");
	if (client == NULL || object == NULL)
		return;

	wl_client_post_event(client,
			     &client->display->object,
			     WL_DISPLAY_GLOBAL,
			     object,
			     object->interface->name,
			     object->interface->version);
}

WL_EXPORT void
wl_resource_destroy(struct wl_resource *resource, struct wl_client *client)
{
	struct wl_display *display;

	assert(client != NULL && resource != NULL &&
	       "wl_resource_destroy called with NULL value!");
	if (client == NULL || resource == NULL)
		return;

	display = client->display;

	wl_list_remove(&resource->link);
	if (resource->object.id > 0)
		wl_hash_table_remove(display->objects, resource->object.id);
	resource->destroy(resource, client);
}

WL_EXPORT void
wl_client_destroy(struct wl_client *client)
{
	struct wl_resource *resource, *tmp;

	printf("disconnect from client %p\n", client);

	assert(client != NULL &&
	       "wl_client_destroy called with NULL value!");
	if (client == NULL)
		return;

	wl_list_for_each_safe(resource, tmp, &client->resource_list, link)
		wl_resource_destroy(resource, client);

	wl_event_source_remove(client->source);
	wl_connection_destroy(client->connection);
	free(client);
}

static void
lose_pointer_focus(struct wl_listener *listener,
		   struct wl_surface *surface, uint32_t time)
{
	struct wl_input_device *device =
		container_of(listener, struct wl_input_device,
			     pointer_focus_listener);

	wl_input_device_set_pointer_focus(device, NULL, time, 0, 0, 0, 0);
}

static void
lose_keyboard_focus(struct wl_listener *listener,
		    struct wl_surface *surface, uint32_t time)
{
	struct wl_input_device *device =
		container_of(listener, struct wl_input_device,
			     keyboard_focus_listener);

	wl_input_device_set_keyboard_focus(device, NULL, time);
}

WL_EXPORT void
wl_input_device_init(struct wl_input_device *device,
		     struct wl_compositor *compositor)
{
	assert(device != NULL && compositor != NULL &&
	       "wl_input_device_init called with NULL value!");
	if (device == NULL || compositor == NULL)
		return;

	wl_list_init(&device->pointer_focus_listener.link);
	device->pointer_focus_listener.func = lose_pointer_focus;
	wl_list_init(&device->keyboard_focus_listener.link);
	device->keyboard_focus_listener.func = lose_keyboard_focus;

	device->x = 100;
	device->y = 100;
	device->compositor = compositor;
}

WL_EXPORT void
wl_input_device_set_pointer_focus(struct wl_input_device *device,
				  struct wl_surface *surface,
				  uint32_t time,
				  int32_t x, int32_t y,
				  int32_t sx, int32_t sy)
{
	assert(device != NULL &&
	       "wl_input_device_set_pointer_focus called with NULL value!");
	if (device == NULL)
		return;

	if (device->pointer_focus == surface)
		return;

	if (device->pointer_focus &&
	    (!surface || device->pointer_focus->client != surface->client))
		wl_client_post_event(device->pointer_focus->client,
				     &device->object,
				     WL_INPUT_DEVICE_POINTER_FOCUS,
				     time, NULL, 0, 0, 0, 0);
	if (surface)
		wl_client_post_event(surface->client,
				     &device->object,
				     WL_INPUT_DEVICE_POINTER_FOCUS,
				     time, surface, x, y, sx, sy);

	device->pointer_focus = surface;
	device->pointer_focus_time = time;

	wl_list_remove(&device->pointer_focus_listener.link);
	if (surface)
		wl_list_insert(surface->destroy_listener_list.prev,
			       &device->pointer_focus_listener.link);
}

WL_EXPORT void
wl_input_device_set_keyboard_focus(struct wl_input_device *device,
				   struct wl_surface *surface,
				   uint32_t time)
{
	assert(device != NULL &&
	       "wl_input_device_set_keyboard_focus called with NULL value!");
	if (device == NULL)
		return;

	if (device->keyboard_focus == surface)
		return;

	if (device->keyboard_focus &&
	    (!surface || device->keyboard_focus->client != surface->client))
		wl_client_post_event(device->keyboard_focus->client,
				     &device->object,
				     WL_INPUT_DEVICE_KEYBOARD_FOCUS,
				     time, NULL, &device->keys);

	if (surface)
		wl_client_post_event(surface->client,
				     &device->object,
				     WL_INPUT_DEVICE_KEYBOARD_FOCUS,
				     time, surface, &device->keys);

	device->keyboard_focus = surface;
	device->keyboard_focus_time = time;

	wl_list_remove(&device->keyboard_focus_listener.link);
	if (surface)
		wl_list_insert(surface->destroy_listener_list.prev,
			       &device->keyboard_focus_listener.link);
}

static void
display_sync(struct wl_client *client,
	       struct wl_display *display, uint32_t key)
{
	wl_client_post_event(client, &display->object, WL_DISPLAY_KEY, key, 0);
}

static void
destroy_frame_listener(struct wl_resource *resource, struct wl_client *client)
{
	struct wl_frame_listener *listener =
		container_of(resource, struct wl_frame_listener, resource);

	wl_list_remove(&listener->link);
	free(listener);
}

static void
display_frame(struct wl_client *client,
	      struct wl_display *display, uint32_t key)
{
	struct wl_frame_listener *listener;

	listener = malloc(sizeof *listener);
	if (listener == NULL) {
		wl_client_post_no_memory(client);
		return;
	}

	/* The listener is a resource so we destroy it when the client
	 * goes away. */
	listener->resource.destroy = destroy_frame_listener;
	listener->resource.object.id = 0;
	listener->client = client;
	listener->key = key;
	wl_list_insert(client->resource_list.prev, &listener->resource.link);
	wl_list_insert(display->frame_list.prev, &listener->link);
}

struct wl_display_interface display_interface = {
	display_sync,
	display_frame
};


WL_EXPORT struct wl_display *
wl_display_create(void)
{
	struct wl_display *display;
	const char *debug;

	debug = getenv("WAYLAND_DEBUG");
	if (debug)
		wl_debug = 1;

	display = malloc(sizeof *display);
	if (display == NULL)
		return NULL;

	display->loop = wl_event_loop_create();
	if (display->loop == NULL) {
		free(display);
		return NULL;
	}

	display->objects = wl_hash_table_create();
	if (display->objects == NULL) {
		free(display);
		return NULL;
	}

	wl_list_init(&display->frame_list);
	wl_list_init(&display->global_list);
	wl_list_init(&display->socket_list);

	display->client_id_range = 256; /* Gah, arbitrary... */

	display->id = 1;
	display->object.interface = &wl_display_interface;
	display->object.implementation = (void (**)(void)) &display_interface;
	wl_display_add_object(display, &display->object);
	if (wl_display_add_global(display, &display->object, NULL)) {
		wl_event_loop_destroy(display->loop);
		free(display);
		return NULL;
	}

	return display;
}

WL_EXPORT void
wl_display_destroy(struct wl_display *display)
{
	struct wl_socket *s, *next;

	assert(display != NULL &&
	       "wl_display_destroy called with NULL value!");
	if (display == NULL)
		return;

	wl_event_loop_destroy(display->loop);
	wl_hash_table_destroy(display->objects);
	
	wl_list_for_each_safe(s, next, &display->socket_list, link) {
		close(s->fd);
		unlink(s->addr.sun_path);
		free(s);
	}

	free(display);
}

WL_EXPORT void
wl_display_add_object(struct wl_display *display, struct wl_object *object)
{
	assert(display != NULL && object != NULL &&
	       "wl_display_add_object called with NULL value!");
	if (display == NULL || object == NULL)
		return;

	object->id = display->id++;
	wl_hash_table_insert(display->objects, object->id, object);
}

WL_EXPORT int
wl_display_add_global(struct wl_display *display,
		      struct wl_object *object, wl_client_connect_func_t func)
{
	struct wl_global *global;

	assert(display != NULL && object != NULL && func != NULL &&
	       "wl_display_add_global called with NULL value!");
	if (display == NULL || object == NULL || func != NULL)
		return -1;

	global = malloc(sizeof *global);
	if (global == NULL)
		return -1;

	global->object = object;
	global->func = func;
	wl_list_insert(display->global_list.prev, &global->link);

	return 0;	
}

WL_EXPORT void
wl_display_post_frame(struct wl_display *display, uint32_t time)
{
	struct wl_frame_listener *listener, *next;
	assert(display != NULL &&
	       "wl_display_post_frame called with NULL value!");
	if (display == NULL)
		return;

	wl_list_for_each_safe(listener, next, &display->frame_list, link) {
		wl_client_post_event(listener->client, &display->object,
				     WL_DISPLAY_KEY, listener->key, time);
		wl_resource_destroy(&listener->resource, listener->client);
	}
}

WL_EXPORT struct wl_event_loop *
wl_display_get_event_loop(struct wl_display *display)
{
	assert(display != NULL &&
	       "wl_display_get_event_loop called with NULL value!");
	if (display == NULL)
		return NULL;

	return display->loop;
}

WL_EXPORT void
wl_display_terminate(struct wl_display *display)
{
	assert(display != NULL &&
	       "wl_display_terminate called with NULL value!");
	if (display == NULL)
		return;

	display->run = 0;
}

WL_EXPORT void
wl_display_run(struct wl_display *display)
{
	assert(display != NULL &&
	       "wl_display_run called with NULL value!");
	if (display == NULL)
		return;

	display->run = 1;

	while (display->run)
		wl_event_loop_dispatch(display->loop, -1);
}

static void
socket_data(int fd, uint32_t mask, void *data)
{
	struct wl_display *display = data;
	struct sockaddr_un name;
	socklen_t length;
	int client_fd;

	length = sizeof name;
	client_fd = accept (fd, (struct sockaddr *) &name, &length);
	if (client_fd < 0)
		fprintf(stderr, "failed to accept\n");

	wl_client_create(display, client_fd);
}

WL_EXPORT int
wl_display_add_socket(struct wl_display *display, const char *name)
{
	struct wl_socket *s;
	socklen_t size, name_size;
	const char *runtime_dir;

	assert(display != NULL &&
	       "wl_display_add_socket called with NULL value!");
	if (display == NULL)
		return -1;

	s = malloc(sizeof *s);
	if (socket == NULL)
		return -1;

	s->fd = socket(PF_LOCAL, SOCK_STREAM, 0);
	if (s->fd < 0)
		return -1;

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (runtime_dir == NULL) {
		runtime_dir = ".";
		fprintf(stderr,
			"XDG_RUNTIME_DIR not set, falling back to %s\n",
			runtime_dir);
	}

	if (name == NULL)
		name = getenv("WAYLAND_DISPLAY");
	if (name == NULL)
		name = "wayland-0";

	memset(&s->addr, 0, sizeof s->addr);
	s->addr.sun_family = AF_LOCAL;
	name_size = snprintf(s->addr.sun_path, sizeof s->addr.sun_path,
			     "%s/%s", runtime_dir, name) + 1;
	fprintf(stderr, "using socket %s\n", s->addr.sun_path);

	size = offsetof (struct sockaddr_un, sun_path) + name_size;
	if (bind(s->fd, (struct sockaddr *) &s->addr, size) < 0)
		return -1;

	if (listen(s->fd, 1) < 0)
		return -1;

	wl_event_loop_add_fd(display->loop, s->fd,
			     WL_EVENT_READABLE,
			     socket_data, display);
	wl_list_insert(display->socket_list.prev, &s->link);

	return 0;
}

WL_EXPORT int
wl_compositor_init(struct wl_compositor *compositor,
		   const struct wl_compositor_interface *interface,
		   struct wl_display *display)
{
	assert(compositor != NULL && interface != NULL && display != NULL &&
	       "wl_compositor_init called with NULL value!");
	if (!(compositor != NULL && interface != NULL && display != NULL))
		return -1;

	compositor->object.interface = &wl_compositor_interface;
	compositor->object.implementation = (void (**)(void)) interface;
	wl_display_add_object(display, &compositor->object);
	if (wl_display_add_global(display, &compositor->object, NULL))
		return -1;

	compositor->argb_visual.object.interface = &wl_visual_interface;
	compositor->argb_visual.object.implementation = NULL;
	wl_display_add_object(display, &compositor->argb_visual.object);
	wl_display_add_global(display, &compositor->argb_visual.object, NULL);

	compositor->premultiplied_argb_visual.object.interface =
		&wl_visual_interface;
	compositor->premultiplied_argb_visual.object.implementation = NULL;
	wl_display_add_object(display,
			      &compositor->premultiplied_argb_visual.object);
	wl_display_add_global(display,
			      &compositor->premultiplied_argb_visual.object,
			      NULL);

	compositor->rgb_visual.object.interface = &wl_visual_interface;
	compositor->rgb_visual.object.implementation = NULL;
	wl_display_add_object(display,
			      &compositor->rgb_visual.object);
	wl_display_add_global(display,
			      &compositor->rgb_visual.object, NULL);

	return 0;
}
