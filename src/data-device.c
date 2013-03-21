/*
 * Copyright © 2011 Kristian Høgsberg
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
#include <string.h>
#include <unistd.h>
#include <stdio.h>

#include "wayland-private.h"
#include "wayland-server.h"

static void
data_offer_accept(struct wl_client *client, struct wl_resource *resource,
		  uint32_t serial, const char *mime_type)
{
	struct wl_data_offer *offer = wl_resource_get_data(resource);

	/* FIXME: Check that client is currently focused by the input
	 * device that is currently dragging this data source.  Should
	 * this be a wl_data_device request? */

	if (offer->source)
		offer->source->accept(offer->source, serial, mime_type);
}

static void
data_offer_receive(struct wl_client *client, struct wl_resource *resource,
		   const char *mime_type, int32_t fd)
{
	struct wl_data_offer *offer = wl_resource_get_data(resource);

	if (offer->source)
		offer->source->send(offer->source, mime_type, fd);
	else
		close(fd);
}

static void
data_offer_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static const struct wl_data_offer_interface data_offer_interface = {
	data_offer_accept,
	data_offer_receive,
	data_offer_destroy,
};

static void
destroy_data_offer(struct wl_resource *resource)
{
	struct wl_data_offer *offer = wl_resource_get_data(resource);

	if (offer->source)
		wl_list_remove(&offer->source_destroy_listener.link);
	free(offer);
}

static void
destroy_offer_data_source(struct wl_listener *listener, void *data)
{
	struct wl_data_offer *offer;

	offer = container_of(listener, struct wl_data_offer,
			     source_destroy_listener);

	offer->source = NULL;
}

static struct wl_resource *
wl_data_source_send_offer(struct wl_data_source *source,
			  struct wl_resource *target)
{
	struct wl_data_offer *offer;
	char **p;

	offer = malloc(sizeof *offer);
	if (offer == NULL)
		return NULL;

	offer->source = source;
	offer->source_destroy_listener.notify = destroy_offer_data_source;

	offer->resource = wl_client_new_object(wl_resource_get_client(target),
					       &wl_data_offer_interface,
					       &data_offer_interface, offer);
	wl_resource_set_destructor(offer->resource, destroy_data_offer);

	wl_resource_add_destroy_listener(source->resource,
					 &offer->source_destroy_listener);

	wl_data_device_send_data_offer(target, offer->resource);

	wl_array_for_each(p, &source->mime_types)
		wl_data_offer_send_offer(offer->resource, *p);

	return offer->resource;
}

static void
data_source_offer(struct wl_client *client,
		  struct wl_resource *resource,
		  const char *type)
{
	struct wl_data_source *source = wl_resource_get_data(resource);
	char **p;

	p = wl_array_add(&source->mime_types, sizeof *p);
	if (p)
		*p = strdup(type);
	if (!p || !*p)
		wl_resource_post_no_memory(resource);
}

static void
data_source_destroy(struct wl_client *client, struct wl_resource *resource)
{
	wl_resource_destroy(resource);
}

static struct wl_data_source_interface data_source_interface = {
	data_source_offer,
	data_source_destroy
};

static struct wl_data_device *
find_data_device(struct wl_list *list, struct wl_client *client)
{
	struct wl_data_device *device;

	wl_list_for_each(device, list, link) {
		if (wl_resource_get_client(device->resource) == client)
			return device;
	}

	return NULL;
}

static void
destroy_drag_focus(struct wl_listener *listener, void *data)
{
	struct wl_seat *seat =
		container_of(listener, struct wl_seat, drag_focus_listener);

	seat->drag_focus_device = NULL;
}

static void
drag_grab_focus(struct wl_pointer_grab *grab,
		struct wl_surface *surface, wl_fixed_t x, wl_fixed_t y)
{
	struct wl_seat *seat = container_of(grab, struct wl_seat, drag_grab);
	struct wl_resource *offer = NULL;
	struct wl_data_device *device;
	struct wl_display *display;
	uint32_t serial;

	if (seat->drag_focus_device) {
		wl_data_device_send_leave(seat->drag_focus_device->resource);
		wl_list_remove(&seat->drag_focus_listener.link);
		seat->drag_focus_device = NULL;
		seat->drag_focus = NULL;
	}

	if (!surface)
		return;

	if (!seat->drag_data_source &&
	    wl_resource_get_client(&surface->resource) != seat->drag_client)
		return;

	device = find_data_device(&seat->drag_data_device_list,
				  wl_resource_get_client(&surface->resource));
	if (!device)
		return;

	display = wl_client_get_display(wl_resource_get_client(device->resource));
	serial = wl_display_next_serial(display);

	if (seat->drag_data_source)
		offer = wl_data_source_send_offer(seat->drag_data_source,
						  device->resource);

	wl_data_device_send_enter(device->resource, serial, &surface->resource,
				  x, y, offer);

	seat->drag_focus = surface;
	seat->drag_focus_listener.notify = destroy_drag_focus;
	wl_resource_add_destroy_listener(device->resource,
					 &seat->drag_focus_listener);
	seat->drag_focus_device = device;
	grab->focus = surface;
}

static void
drag_grab_motion(struct wl_pointer_grab *grab,
		 uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	struct wl_seat *seat = container_of(grab, struct wl_seat, drag_grab);

	if (seat->drag_focus_device)
		wl_data_device_send_motion(seat->drag_focus_device->resource,
					   time, x, y);
}

static void
data_device_end_drag_grab(struct wl_seat *seat)
{
	if (seat->drag_surface) {
		seat->drag_surface = NULL;
		wl_signal_emit(&seat->drag_icon_signal, NULL);
		wl_list_remove(&seat->drag_icon_listener.link);
	}

	drag_grab_focus(&seat->drag_grab, NULL,
	                wl_fixed_from_int(0), wl_fixed_from_int(0));

	wl_pointer_end_grab(seat->pointer);

	seat->drag_data_source = NULL;
	seat->drag_client = NULL;
}

static void
drag_grab_button(struct wl_pointer_grab *grab,
		 uint32_t time, uint32_t button, uint32_t state_w)
{
	struct wl_seat *seat = container_of(grab, struct wl_seat, drag_grab);
	enum wl_pointer_button_state state = state_w;

	if (seat->drag_focus_device &&
	    seat->pointer->grab_button == button &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED)
		wl_data_device_send_drop(seat->drag_focus_device->resource);

	if (seat->pointer->button_count == 0 &&
	    state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (seat->drag_data_source)
			wl_list_remove(&seat->drag_data_source_listener.link);
		data_device_end_drag_grab(seat);
	}
}

static const struct wl_pointer_grab_interface drag_grab_interface = {
	drag_grab_focus,
	drag_grab_motion,
	drag_grab_button,
};

static void
destroy_data_device_source(struct wl_listener *listener, void *data)
{
	struct wl_seat *seat = container_of(listener, struct wl_seat,
					    drag_data_source_listener);

	data_device_end_drag_grab(seat);
}

static void
destroy_data_device_icon(struct wl_listener *listener, void *data)
{
	struct wl_seat *seat = container_of(listener, struct wl_seat,
					    drag_icon_listener);

	seat->drag_surface = NULL;
}

static void
data_device_start_drag(struct wl_client *client, struct wl_resource *resource,
		       struct wl_resource *source_resource,
		       struct wl_resource *origin_resource,
		       struct wl_resource *icon_resource, uint32_t serial)
{
	struct wl_data_device *device = wl_resource_get_data(resource);
	struct wl_seat *seat = device->seat;

	/* FIXME: Check that client has implicit grab on the origin
	 * surface that matches the given time. */

	/* FIXME: Check that the data source type array isn't empty. */

	seat->drag_grab.interface = &drag_grab_interface;

	seat->drag_client = client;
	seat->drag_data_source = NULL;

	if (source_resource) {
		seat->drag_data_source = wl_resource_get_data(source_resource);
		seat->drag_data_source_listener.notify =
			destroy_data_device_source;
		wl_resource_add_destroy_listener(source_resource,
						 &seat->drag_data_source_listener);
	}

	if (icon_resource) {
		seat->drag_surface = wl_resource_get_data(icon_resource);
		seat->drag_icon_listener.notify = destroy_data_device_icon;
		wl_resource_add_destroy_listener(icon_resource,
						 &seat->drag_icon_listener);
		wl_signal_emit(&seat->drag_icon_signal, icon_resource);
	}

	wl_pointer_set_focus(seat->pointer, NULL,
			     wl_fixed_from_int(0), wl_fixed_from_int(0));
	wl_pointer_start_grab(seat->pointer, &seat->drag_grab);
}

static void
destroy_selection_data_source(struct wl_listener *listener, void *data)
{
	struct wl_seat *seat = container_of(listener, struct wl_seat,
					    selection_data_source_listener);
	struct wl_data_device *data_device;
	struct wl_resource *focus = NULL;

	seat->selection_data_source = NULL;

	if (seat->keyboard)
		focus = seat->keyboard->focus_resource;
	if (focus) {
		data_device = find_data_device(&seat->drag_data_device_list,
					       wl_resource_get_client(focus));
		if (data_device)
			wl_data_device_send_selection(data_device->resource,
						      NULL);
	}

	wl_signal_emit(&seat->selection_signal, seat);
}

WL_EXPORT void
wl_seat_set_selection(struct wl_seat *seat, struct wl_data_source *source,
		      uint32_t serial)
{
	struct wl_resource *offer, *focus = NULL;
	struct wl_data_device *data_device;

	if (seat->selection_data_source &&
	    seat->selection_serial - serial < UINT32_MAX / 2)
		return;

	if (seat->selection_data_source) {
		seat->selection_data_source->cancel(seat->selection_data_source);
		wl_list_remove(&seat->selection_data_source_listener.link);
		seat->selection_data_source = NULL;
	}

	seat->selection_data_source = source;
	seat->selection_serial = serial;

	if (seat->keyboard)
		focus = seat->keyboard->focus_resource;
	if (focus) {
		data_device = find_data_device(&seat->drag_data_device_list,
					       wl_resource_get_client(focus));
		if (data_device && source) {
			offer = wl_data_source_send_offer(seat->selection_data_source,
							  data_device->resource);
			wl_data_device_send_selection(data_device->resource,
						      offer);
		} else if (data_device) {
			wl_data_device_send_selection(data_device->resource,
						      NULL);
		}
	}

	wl_signal_emit(&seat->selection_signal, seat);

	if (source) {
		seat->selection_data_source_listener.notify =
			destroy_selection_data_source;
		wl_resource_add_destroy_listener(source->resource,
						 &seat->selection_data_source_listener);
	}
}

static void
data_device_set_selection(struct wl_client *client,
			  struct wl_resource *resource,
			  struct wl_resource *source_resource, uint32_t serial)
{
	struct wl_data_device *device = wl_resource_get_data(resource);

	if (!source_resource)
		return;

	/* FIXME: Store serial and check against incoming serial here. */
	wl_seat_set_selection(device->seat,
			      wl_resource_get_data(source_resource),
			      serial);
}

static const struct wl_data_device_interface data_device_interface = {
	data_device_start_drag,
	data_device_set_selection,
};

static void
destroy_data_source(struct wl_resource *resource)
{
	struct wl_data_source *source = wl_resource_get_data(resource);
	char **p;

	wl_array_for_each(p, &source->mime_types)
		free(*p);

	wl_array_release(&source->mime_types);
}

static void
client_source_accept(struct wl_data_source *source,
		     uint32_t time, const char *mime_type)
{
	wl_data_source_send_target(source->resource, mime_type);
}

static void
client_source_send(struct wl_data_source *source,
		   const char *mime_type, int32_t fd)
{
	wl_data_source_send_send(source->resource, mime_type, fd);
	close(fd);
}

static void
client_source_cancel(struct wl_data_source *source)
{
	wl_data_source_send_cancelled(source->resource);
}

static void
create_data_source(struct wl_client *client,
		   struct wl_resource *resource, uint32_t id)
{
	struct wl_data_source *source;

	source = malloc(sizeof *source);
	if (source == NULL) {
		wl_resource_post_no_memory(resource);
		return;
	}

	source->accept = client_source_accept;
	source->send = client_source_send;
	source->cancel = client_source_cancel;

	wl_array_init(&source->mime_types);
	source->resource = wl_client_add_object(client,
						&wl_data_source_interface,
						&data_source_interface,
						id, source);
	wl_resource_set_destructor(source->resource, destroy_data_source);
}

static void unbind_data_device(struct wl_resource *resource)
{
	struct wl_data_device *device = wl_resource_get_data(resource);

	wl_list_remove(&device->link);
	free(device);
}

static void
get_data_device(struct wl_client *client,
		struct wl_resource *manager_resource,
		uint32_t id, struct wl_resource *seat_resource)
{
	struct wl_seat *seat = wl_resource_get_data(seat_resource);
	struct wl_data_device *device;

	device = malloc(sizeof *device);
	if (device == NULL) {
		wl_resource_post_no_memory(manager_resource);
		return;
	}

	device->seat = seat;
	device->resource = wl_client_add_object(client,
						&wl_data_device_interface,
						&data_device_interface,
						id, device);
	wl_resource_set_destructor(device->resource, unbind_data_device);

	wl_list_insert(&seat->drag_data_device_list, &device->link);
}

static const struct wl_data_device_manager_interface manager_interface = {
	create_data_source,
	get_data_device
};

static void
bind_manager(struct wl_client *client,
	     void *data, uint32_t version, uint32_t id)
{
	wl_client_add_object(client, &wl_data_device_manager_interface,
			     &manager_interface, id, NULL);
}

WL_EXPORT void
wl_data_device_set_keyboard_focus(struct wl_seat *seat)
{
	struct wl_resource *focus, *offer;
	struct wl_data_source *source;
	struct wl_data_device *data_device;

	if (!seat->keyboard)
		return;

	focus = seat->keyboard->focus_resource;
	if (!focus)
		return;

	data_device = find_data_device(&seat->drag_data_device_list,
				       wl_resource_get_client(focus));
	if (!data_device)
		return;

	source = seat->selection_data_source;
	if (source) {
		offer = wl_data_source_send_offer(source, data_device->resource);
		wl_data_device_send_selection(data_device->resource, offer);
	}
}

WL_EXPORT int
wl_data_device_manager_init(struct wl_display *display)
{
	if (wl_display_add_global(display,
				  &wl_data_device_manager_interface,
				  NULL, bind_manager) == NULL)
		return -1;

	return 0;
}
