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
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <ctype.h>
#include <assert.h>
#include <fcntl.h>
#include <sys/poll.h>

#include "wayland-util.h"
#include "wayland-os.h"
#include "wayland-client.h"
#include "wayland-private.h"

struct wl_global_listener {
	wl_display_global_func_t handler;
	void *data;
	struct wl_list link;
};

struct wl_proxy {
	struct wl_object object;
	struct wl_display *display;
	void *user_data;
};

struct wl_global {
	uint32_t id;
	char *interface;
	uint32_t version;
	struct wl_list link;
};

struct wl_display {
	struct wl_proxy proxy;
	struct wl_connection *connection;
	int fd;
	uint32_t mask;
	uint32_t fatal_error;
	struct wl_map objects;
	struct wl_list global_listener_list;
	struct wl_list global_list;

	wl_display_update_func_t update;
	void *update_data;

	wl_display_global_func_t global_handler;
	void *global_handler_data;
};

static int wl_debug = 0;

static int
connection_update(struct wl_connection *connection,
		  uint32_t mask, void *data)
{
	struct wl_display *display = data;

	display->mask = mask;
	if (display->update)
		return display->update(display->mask,
				       display->update_data);

	return 0;
}

WL_EXPORT struct wl_global_listener *
wl_display_add_global_listener(struct wl_display *display,
			       wl_display_global_func_t handler, void *data)
{
	struct wl_global_listener *listener;
	struct wl_global *global;

	listener = malloc(sizeof *listener);
	if (listener == NULL)
		return NULL;

	listener->handler = handler;
	listener->data = data;
	wl_list_insert(display->global_listener_list.prev, &listener->link);

	wl_list_for_each(global, &display->global_list, link)
		(*listener->handler)(display, global->id, global->interface,
				     global->version, listener->data);

	return listener;
}

WL_EXPORT void
wl_display_remove_global_listener(struct wl_display *display,
				  struct wl_global_listener *listener)
{
	wl_list_remove(&listener->link);
	free(listener);
}

WL_EXPORT struct wl_proxy *
wl_proxy_create(struct wl_proxy *factory, const struct wl_interface *interface)
{
	struct wl_proxy *proxy;
	struct wl_display *display = factory->display;

	proxy = malloc(sizeof *proxy);
	if (proxy == NULL)
		return NULL;

	proxy->object.interface = interface;
	proxy->object.implementation = NULL;
	proxy->object.id = wl_map_insert_new(&display->objects,
					     WL_MAP_CLIENT_SIDE, proxy);
	proxy->display = display;

	return proxy;
}

WL_EXPORT struct wl_proxy *
wl_proxy_create_for_id(struct wl_proxy *factory,
		       uint32_t id, const struct wl_interface *interface)
{
	struct wl_proxy *proxy;
	struct wl_display *display = factory->display;
	int ret;

	proxy = malloc(sizeof *proxy);
	if (proxy == NULL)
		return NULL;

	proxy->object.interface = interface;
	proxy->object.implementation = NULL;
	proxy->object.id = id;
	proxy->display = display;

	ret = wl_map_insert_at(&display->objects, id, proxy);
	if (ret) {
		free(proxy);
		return NULL;
	}

	return proxy;
}

WL_EXPORT int
wl_proxy_destroy(struct wl_proxy *proxy)
{
	int ret;
	if (proxy->object.id < WL_SERVER_ID_START)
		ret = wl_map_insert_at(&proxy->display->objects,
		                       proxy->object.id, WL_ZOMBIE_OBJECT);
	else
		ret = wl_map_insert_at(&proxy->display->objects,
		                       proxy->object.id, NULL);
	if (ret)
		wl_log("Could not destroy proxy %u\n", proxy->object.id);
	else
		free(proxy);

	return ret;
}

WL_EXPORT int
wl_proxy_add_listener(struct wl_proxy *proxy,
		      void (**implementation)(void), void *data)
{
	if (proxy->object.implementation) {
		fprintf(stderr, "proxy already has listener\n");
		return -1;
	}

	proxy->object.implementation = implementation;
	proxy->user_data = data;

	return 0;
}

WL_EXPORT int
wl_proxy_marshal(struct wl_proxy *proxy, uint32_t opcode, ...)
{
	struct wl_closure *closure;
	va_list ap;
	int ret = 0;

	va_start(ap, opcode);
	closure = wl_closure_vmarshal(&proxy->object, opcode, ap,
				      &proxy->object.interface->methods[opcode]);
	va_end(ap);

	if (closure == NULL) {
		fprintf(stderr, "Error marshalling request\n");
		return -1;
	}

	if (wl_debug)
		wl_closure_print(closure, &proxy->object, true);

	if ((ret = wl_closure_send(closure, proxy->display->connection))) {
		fprintf(stderr, "Error sending request: %m\n");
	}

	wl_closure_destroy(closure);
	return ret;
}

/* Can't do this, there may be more than one instance of an
 * interface... */
WL_EXPORT uint32_t
wl_display_get_global(struct wl_display *display,
		      const char *interface, uint32_t version)
{
	struct wl_global *global;

	wl_list_for_each(global, &display->global_list, link)
		if (strcmp(interface, global->interface) == 0 &&
		    version <= global->version)
			return global->id;

	return 0;
}

static void
display_handle_error(void *data,
		     struct wl_display *display, struct wl_object *object,
		     uint32_t code, const char *message)
{
	wl_log("%s@%u: error %u: %s\n",
	       object->interface->name, object->id, code, message);

	display->fatal_error = true;
}

static void
display_handle_global(void *data,
		      struct wl_display *display,
		      uint32_t id, const char *interface, uint32_t version)
{
	struct wl_global_listener *listener;
	struct wl_global *global;

	global = malloc(sizeof *global);
	global->id = id;
	global->interface = strdup(interface);
	global->version = version;
	wl_list_insert(display->global_list.prev, &global->link);

	wl_list_for_each(listener, &display->global_listener_list, link)
		(*listener->handler)(display,
				     id, interface, version, listener->data);
}

static void
wl_global_destroy(struct wl_global *global)
{
	wl_list_remove(&global->link);
	free(global->interface);
	free(global);
}

static void
display_handle_global_remove(void *data,
                             struct wl_display *display, uint32_t id)
{
	struct wl_global *global;

	wl_list_for_each(global, &display->global_list, link)
		if (global->id == id) {
			wl_global_destroy(global);
			break;
		}
}

static void
display_handle_delete_id(void *data, struct wl_display *display, uint32_t id)
{
	struct wl_proxy *proxy;

	proxy = wl_map_lookup(&display->objects, id);
	if (proxy != WL_ZOMBIE_OBJECT)
		fprintf(stderr, "server sent delete_id for live object\n");
	else
		wl_map_remove(&display->objects, id);
}

static const struct wl_display_listener display_listener = {
	display_handle_error,
	display_handle_global,
	display_handle_global_remove,
	display_handle_delete_id
};

static int
connect_to_socket(struct wl_display *display, const char *name)
{
	struct sockaddr_un addr;
	socklen_t size;
	const char *runtime_dir;
	int name_size;

	runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir) {
		fprintf(stderr,
			"error: XDG_RUNTIME_DIR not set in the environment.\n");

		/* to prevent programs reporting
		 * "failed to create display: Success" */
		errno = ENOENT;
		return -1;
	}

	if (name == NULL)
		name = getenv("WAYLAND_DISPLAY");
	if (name == NULL)
		name = "wayland-0";

	display->fd = wl_os_socket_cloexec(PF_LOCAL, SOCK_STREAM, 0);
	if (display->fd < 0)
		return -1;

	memset(&addr, 0, sizeof addr);
	addr.sun_family = AF_LOCAL;
	name_size =
		snprintf(addr.sun_path, sizeof addr.sun_path,
			 "%s/%s", runtime_dir, name) + 1;

	assert(name_size > 0);
	if (name_size > (int)sizeof addr.sun_path) {
		fprintf(stderr,
		       "error: socket path \"%s/%s\" plus null terminator"
		       " exceeds 108 bytes\n", runtime_dir, name);
		close(display->fd);
		/* to prevent programs reporting
		 * "failed to add socket: Success" */
		errno = ENAMETOOLONG;
		return -1;
	};

	size = offsetof (struct sockaddr_un, sun_path) + name_size;

	if (connect(display->fd, (struct sockaddr *) &addr, size) < 0) {
		close(display->fd);
		return -1;
	}

	return 0;
}

WL_EXPORT struct wl_display *
wl_display_connect(const char *name)
{
	struct wl_display *display;
	const char *debug;
	char *connection, *end;
	int flags;

	debug = getenv("WAYLAND_DEBUG");
	if (debug)
		wl_debug = 1;

	display = malloc(sizeof *display);
	if (display == NULL)
		return NULL;

	memset(display, 0, sizeof *display);
	connection = getenv("WAYLAND_SOCKET");
	if (connection) {
		display->fd = strtol(connection, &end, 0);
		if (*end != '\0') {
			free(display);
			return NULL;
		}
		flags = fcntl(display->fd, F_GETFD);
		if (flags != -1)
			fcntl(display->fd, F_SETFD, flags | FD_CLOEXEC);
		unsetenv("WAYLAND_SOCKET");
	} else if (connect_to_socket(display, name) < 0) {
		free(display);
		return NULL;
	}

	wl_map_init(&display->objects);
	wl_list_init(&display->global_listener_list);
	wl_list_init(&display->global_list);

	wl_map_insert_new(&display->objects, WL_MAP_CLIENT_SIDE, NULL);

	display->proxy.object.interface = &wl_display_interface;
	display->proxy.object.id =
		wl_map_insert_new(&display->objects,
				  WL_MAP_CLIENT_SIDE, display);
	display->proxy.display = display;
	display->proxy.object.implementation = (void(**)(void)) &display_listener;
	display->proxy.user_data = display;

	display->connection = wl_connection_create(display->fd,
						   connection_update, display);
	if (display->connection == NULL) {
		wl_map_release(&display->objects);
		close(display->fd);
		free(display);
		return NULL;
	}

	display->fatal_error = false;

	return display;
}

WL_EXPORT void
wl_display_disconnect(struct wl_display *display)
{
	struct wl_global *global, *gnext;
	struct wl_global_listener *listener, *lnext;

	wl_connection_destroy(display->connection);
	wl_map_release(&display->objects);
	wl_list_for_each_safe(global, gnext,
			      &display->global_list, link)
		wl_global_destroy(global);
	wl_list_for_each_safe(listener, lnext,
			      &display->global_listener_list, link)
		free(listener);

	close(display->fd);
	free(display);
}

WL_EXPORT int
wl_display_get_fd(struct wl_display *display,
		  wl_display_update_func_t update, void *data)
{
	display->update = update;
	display->update_data = data;

	if (display->update)
		display->update(display->mask,
		                display->update_data);

	return display->fd;
}

static void
sync_callback(void *data, struct wl_callback *callback, uint32_t serial)
{
   int *done = data;

   *done = 1;
   wl_callback_destroy(callback);
}

static const struct wl_callback_listener sync_listener = {
	sync_callback
};

WL_EXPORT int
wl_display_roundtrip(struct wl_display *display)
{
	struct wl_callback *callback;
	int ret, done;

	callback = wl_display_sync(display);
	if (!callback)
		return -1;

	done = 0;
	ret = wl_callback_add_listener(callback, &sync_listener, &done);
	if (ret)
		return ret;

	ret = wl_display_flush(display);
	if (ret)
		return ret;

	while (!done && ret == 0)
		ret = wl_display_iterate(display, WL_DISPLAY_READABLE);

	return ret;
}

static int
create_proxies(struct wl_display *display, struct wl_closure *closure)
{
	struct wl_proxy *proxy;
	const char *signature;
	struct argument_details arg;
	uint32_t id;
	int i;
	int count;

	signature = closure->message->signature;
	count = arg_count_for_signature(signature) + 2;
	for (i = 2; i < count; i++) {
		signature = get_next_argument(signature, &arg);
		switch (arg.type) {
		case 'n':
			id = **(uint32_t **) closure->args[i];
			if (id == 0) {
				*(void **) closure->args[i] = NULL;
				break;
			}
			proxy = wl_proxy_create_for_id(&display->proxy, id,
						       closure->message->types[i - 2]);
			if (proxy == NULL)
				return -1;
			*(void **) closure->args[i] = proxy;
			break;
		default:
			break;
		}
	}

	return 0;
}

static int
handle_event(struct wl_display *display,
	     uint32_t id, uint32_t opcode, uint32_t size)
{
	struct wl_proxy *proxy;
	struct wl_closure *closure;
	const struct wl_message *message;

	proxy = wl_map_lookup(&display->objects, id);

	if (proxy == WL_ZOMBIE_OBJECT) {
		wl_connection_consume(display->connection, size);
		return 0;
	} else if (proxy == NULL || proxy->object.implementation == NULL) {
		wl_connection_consume(display->connection, size);
		return 0;
	}

	message = &proxy->object.interface->events[opcode];
	closure = wl_connection_demarshal(display->connection, size,
					  &display->objects, message);

	if (closure == NULL || create_proxies(display, closure) < 0) {
		wl_log("Error demarshalling event\n");
		return -1;
	}

	if (wl_debug)
		wl_closure_print(closure, &proxy->object, false);

	wl_closure_invoke(closure, &proxy->object,
			  proxy->object.implementation[opcode],
			  proxy->user_data);

	wl_closure_destroy(closure);

	return 0;
}

WL_EXPORT int
wl_display_iterate(struct wl_display *display, uint32_t mask)
{
	uint32_t p[2], object;
	int len, opcode, size;

	if (display->fatal_error) {
		wl_log("Fatal error on wl_display %p: Call wl_display_destroy()"
		       " and create a replacement display\n", (void*)display);
		errno = EPROTO;
		return -1;
	}

	mask &= display->mask;
	if (mask == 0) {
		fprintf(stderr,
			"wl_display_iterate called with unsolicited flags\n");
		errno = EINVAL;
		return -1;
	}

	len = wl_connection_data(display->connection, mask);

	if (len < 0) {
		wl_log("read error: %m\n");
		return len;
	}

	while (len > 0) {
		int ret;

		if ((size_t) len < sizeof p)
			break;
		
		wl_connection_copy(display->connection, p, sizeof p);
		object = p[0];
		opcode = p[1] & 0xffff;
		size = p[1] >> 16;
		if (len < size)
			break;

		ret = handle_event(display, object, opcode, size);
		if (ret)
			return ret;

		len -= size;
	}

	return len;
}

WL_EXPORT int
wl_display_flush(struct wl_display *display)
{
	int ret;

	while (display->mask & WL_DISPLAY_WRITABLE) {
		ret = wl_display_iterate (display, WL_DISPLAY_WRITABLE);
		if (ret)
			break;
	}

	return ret;
}

WL_EXPORT void *
wl_display_bind(struct wl_display *display,
		uint32_t name, const struct wl_interface *interface)
{
	struct wl_proxy *proxy;

	proxy = wl_proxy_create(&display->proxy, interface);
	if (proxy == NULL)
		return NULL;

	wl_proxy_marshal(&display->proxy, WL_DISPLAY_BIND,
			 name, interface->name, interface->version, proxy);

	return proxy;
}

WL_EXPORT struct wl_callback *
wl_display_sync(struct wl_display *display)
{
	struct wl_proxy *proxy;

	proxy = wl_proxy_create(&display->proxy, &wl_callback_interface);

	if (!proxy)
		return NULL;

	wl_proxy_marshal(&display->proxy, WL_DISPLAY_SYNC, proxy);

	return (struct wl_callback *) proxy;
}

WL_EXPORT void
wl_proxy_set_user_data(struct wl_proxy *proxy, void *user_data)
{
	proxy->user_data = user_data;
}

WL_EXPORT void *
wl_proxy_get_user_data(struct wl_proxy *proxy)
{
	return proxy->user_data;
}

WL_EXPORT uint32_t
wl_proxy_get_id(struct wl_proxy *proxy)
{
	return proxy->object.id;
}

WL_EXPORT void
wl_log_set_handler_client(wl_log_func_t handler)
{
	wl_log_handler = handler;
}
