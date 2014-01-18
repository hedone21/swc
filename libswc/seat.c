/* swc: libswc/seat.c
 *
 * Copyright (c) 2013 Michael Forney
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "seat.h"
#include "data_device.h"
#include "evdev_device.h"
#include "event.h"
#include "internal.h"
#include "keyboard.h"
#include "pointer.h"
#include "util.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <libudev.h>

static struct
{
    char * name;
    uint32_t capabilities;

    struct swc_keyboard keyboard;
    struct swc_pointer pointer;
    struct swc_data_device data_device;

    struct wl_global * global;
    struct wl_list resources;
    struct wl_list devices;
} seat;

const struct swc_seat_global seat_global = {
    .pointer = &seat.pointer,
    .keyboard = &seat.keyboard,
    .data_device = &seat.data_device
};

static void handle_key(uint32_t time, uint32_t key, uint32_t state)
{
    swc_keyboard_handle_key(&seat.keyboard, time, key, state);
}

static void handle_button(uint32_t time, uint32_t button, uint32_t state)
{
    swc_pointer_handle_button(&seat.pointer, time, button, state);
}

static void handle_axis(uint32_t time, uint32_t axis, wl_fixed_t amount)
{
    swc_pointer_handle_axis(&seat.pointer, time, axis, amount);
}

static void handle_relative_motion(uint32_t time, wl_fixed_t dx, wl_fixed_t dy)
{
    swc_pointer_handle_relative_motion(&seat.pointer, time, dx, dy);
}

const static struct swc_evdev_device_handler evdev_handler = {
    .key = &handle_key,
    .button = &handle_button,
    .axis = &handle_axis,
    .relative_motion = &handle_relative_motion
};

static void handle_keyboard_focus_event(struct wl_listener * listener,
                                        void * data)
{
    struct swc_event * event = data;
    struct swc_input_focus_event_data * event_data = event->data;

    switch (event->type)
    {
        case SWC_INPUT_FOCUS_EVENT_CHANGED:
            if (event_data->new)
            {
                struct wl_client * client
                    = wl_resource_get_client(event_data->new->resource);

                /* Offer the selection to the new focus. */
                swc_data_device_offer_selection(&seat.data_device, client);
            }
            break;
    }
}

static struct wl_listener keyboard_focus_listener = {
    .notify = &handle_keyboard_focus_event
};

static void handle_data_device_event(struct wl_listener * listener, void * data)
{
    struct swc_event * event = data;

    switch (event->type)
    {
        case SWC_DATA_DEVICE_EVENT_SELECTION_CHANGED:
            if (seat.keyboard.focus.resource)
            {
                struct wl_client * client
                    = wl_resource_get_client(seat.keyboard.focus.resource);
                swc_data_device_offer_selection(&seat.data_device, client);
            }
            break;
    }
}

static struct wl_listener data_device_listener = {
    .notify = &handle_data_device_event
};

/* Wayland Seat Interface */
static void get_pointer(struct wl_client * client, struct wl_resource * resource,
                        uint32_t id)
{
    swc_pointer_bind(&seat.pointer, client, id);
}

static void get_keyboard(struct wl_client * client, struct wl_resource * resource,
                         uint32_t id)
{
    swc_keyboard_bind(&seat.keyboard, client, id);
}

static void get_touch(struct wl_client * client, struct wl_resource * resource,
               uint32_t id)
{
    /* XXX: Implement */
}

static struct wl_seat_interface seat_implementation = {
    .get_pointer = &get_pointer,
    .get_keyboard = &get_keyboard,
    .get_touch = &get_touch
};

static void bind_seat(struct wl_client * client, void * data, uint32_t version,
                      uint32_t id)
{
    struct wl_resource * resource;

    if (version >= 2)
        version = 2;

    resource = wl_resource_create(client, &wl_seat_interface, version, id);
    wl_resource_set_implementation(resource, &seat_implementation, NULL,
                                   &swc_remove_resource);
    wl_list_insert(&seat.resources, wl_resource_get_link(resource));

    if (version >= 2)
        wl_seat_send_name(resource, seat.name);

    wl_seat_send_capabilities(resource, seat.capabilities);
}

static void add_device(struct udev_device * udev_device)
{
    const char * device_seat;
    const char * device_path;
    struct swc_evdev_device * device;

    device_seat = udev_device_get_property_value(udev_device, "ID_SEAT");

    /* If the ID_SEAT property is not set, the device belongs to seat0. */
    if (!device_seat)
        device_seat = "seat0";

    if (strcmp(device_seat, seat.name) != 0)
        return;

    device_path = udev_device_get_devnode(udev_device);
    device = swc_evdev_device_new(device_path, &evdev_handler);

    if (!device)
    {
        ERROR("Could not create evdev device\n");
        return;
    }

    if (~seat.capabilities & device->capabilities)
    {
        struct wl_resource * resource;

        seat.capabilities |= device->capabilities;
        wl_list_for_each(resource, &seat.resources, link)
            wl_seat_send_capabilities(resource, seat.capabilities);
    }

    wl_list_insert(&seat.devices, &device->link);
    swc_evdev_device_add_event_sources(device, swc.event_loop);
}

static void add_devices()
{
    struct udev_enumerate * enumerate;
    struct udev_list_entry * entry;
    const char * path;
    struct udev_device * device;

    enumerate = udev_enumerate_new(swc.udev);
    udev_enumerate_add_match_subsystem(enumerate, "input");
    udev_enumerate_add_match_sysname(enumerate, "event[0-9]*");

    udev_enumerate_scan_devices(enumerate);

    udev_list_entry_foreach(entry, udev_enumerate_get_list_entry(enumerate))
    {
        path = udev_list_entry_get_name(entry);
        device = udev_device_new_from_syspath(swc.udev, path);
        add_device(device);
        udev_device_unref(device);
    }

    udev_enumerate_unref(enumerate);
}

bool swc_seat_initialize(const char * seat_name)
{
    if (!(seat.name = strdup(seat_name)))
    {
        ERROR("Could not allocate seat name string\n");
        goto error0;
    }

    seat.capabilities = 0;
    wl_list_init(&seat.resources);
    wl_list_init(&seat.devices);

    seat.global = wl_global_create(swc.display, &wl_seat_interface, 2,
                                       NULL, &bind_seat);

    if (!seat.global)
        goto error1;

    if (!swc_data_device_initialize(&seat.data_device))
    {
        ERROR("Could not initialize data device\n");
        goto error2;
    }

    wl_signal_add(&seat.data_device.event_signal, &data_device_listener);

    if (!swc_keyboard_initialize(&seat.keyboard))
    {
        ERROR("Could not initialize keyboard\n");
        goto error3;
    }

    wl_signal_add(&seat.keyboard.focus.event_signal, &keyboard_focus_listener);

    if (!swc_pointer_initialize(&seat.pointer))
    {
        ERROR("Could not initialize pointer\n");
        goto error4;
    }

    add_devices();

    return true;

  error4:
    swc_keyboard_finish(&seat.keyboard);
  error3:
    swc_data_device_finish(&seat.data_device);
  error2:
    wl_global_destroy(seat.global);
  error1:
    free(seat.name);
  error0:
    return false;
}

void swc_seat_finalize()
{
    struct swc_evdev_device * device, * tmp;

    swc_pointer_finish(&seat.pointer);
    swc_keyboard_finish(&seat.keyboard);

    wl_list_for_each_safe(device, tmp, &seat.devices, link)
        swc_evdev_device_destroy(device);

    wl_global_destroy(seat.global);
    free(seat.name);
}

void swc_seat_reopen_devices()
{
    struct swc_evdev_device * device;

    wl_list_for_each(device, &seat.devices, link)
        swc_evdev_device_reopen(device);
}

