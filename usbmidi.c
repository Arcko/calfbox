/*
Calf Box, an open source musical instrument.
Copyright (C) 2010-2013 Krzysztof Foltman

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "config.h"
#include "config-api.h"
#include "errors.h"
#include "hwcfg.h"
#include "io.h"
#include "meter.h"
#include "midi.h"
#include "recsrc.h"
#include "usbio_impl.h"

#include <unistd.h>

static void midi_transfer_cb(struct libusb_transfer *transfer)
{
    struct usbio_transfer *xf = transfer->user_data;
    struct cbox_usb_midi_input *umi = xf->user_data;
    xf->pending = FALSE;

    if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
    {
        xf->cancel_confirm = 1;
        return;
    }
    if (transfer->status == LIBUSB_TRANSFER_TIMED_OUT || transfer->status == LIBUSB_TRANSFER_ERROR || transfer->status == LIBUSB_TRANSFER_STALL)
    {
        if (transfer->status != LIBUSB_TRANSFER_TIMED_OUT)
            g_warning("USB error on device %03d:%03d: transfer status %d", umi->busdevadr >> 8, umi->busdevadr & 255, transfer->status);
        if (umi->uii->no_resubmit)
            return;
        int res = usbio_transfer_submit(xf);
        if (res != 0)
            g_warning("Error submitting URB to MIDI endpoint: error code %d", res);
        return;
    }
    if (transfer->status == LIBUSB_TRANSFER_NO_DEVICE)
    {
        g_debug("No device %03d:%03d, unlinking", umi->busdevadr >> 8, umi->busdevadr & 255);
        umi->uii->rt_midi_input_ports = g_list_remove(umi->uii->rt_midi_input_ports, umi);
        return;
    }

    const struct cbox_usb_device_info *udi = umi->devinfo;
    if (udi->vid == 0x09e8 && udi->pid == 0x0062)
    {
        for (int i = 0; i < transfer->actual_length;)
        {
            uint8_t *data = &transfer->buffer[i];
            uint8_t len = data[0] & 15;
            if (!len || i + len >= transfer->actual_length)
                break;
            cbox_midi_buffer_write_inline(&umi->midi_buffer, 0, data[1], len > 1 ? data[2] : 0, len > 2 ? data[3] : 0);
            i += len + 1;
        }
    }
    else
    {
        for (int i = 0; i + 3 < transfer->actual_length; i += 4)
        {
            uint8_t *data = &transfer->buffer[i];
            if ((data[0] & 15) >= 0x08)
            {
                // normalise: note on with vel 0 -> note off
                if ((data[1] & 0xF0) == 0x90 && data[3] == 0)
                    cbox_midi_buffer_write_inline(&umi->midi_buffer, 0, data[1] - 0x10, data[2], data[3]);
                else
                    cbox_midi_buffer_write_event(&umi->midi_buffer, 0, data + 1, midi_cmd_size(data[1]));
            }
        }
    }
    if (umi->uii->no_resubmit)
        return;
    usbio_transfer_submit(xf);
}

void usbio_start_midi_capture(struct cbox_usb_io_impl *uii)
{
    uii->rt_midi_input_ports = g_list_copy(uii->midi_input_ports);
    uii->midi_input_port_count = 0;

    for(GList *p = uii->rt_midi_input_ports; p; p = p->next)
        uii->midi_input_port_count++;
    uii->midi_input_port_buffers = calloc(uii->midi_input_port_count, sizeof(struct cbox_midi_buffer *));
    uii->midi_input_port_pos = calloc(uii->midi_input_port_count, sizeof(int));
    int pn = 0;
    for(GList *p = uii->rt_midi_input_ports; p; p = p->next)
    {
        struct cbox_usb_midi_input *umi = p->data;
        cbox_midi_buffer_clear(&umi->midi_buffer);
        umi->transfer = usbio_transfer_new(uii->usbctx,  "MIDI In", 0, 0, umi);
        libusb_fill_bulk_transfer(umi->transfer->transfer, umi->handle, umi->endpoint, umi->midi_recv_data, umi->max_packet_size, midi_transfer_cb, umi->transfer, 0);
        uii->midi_input_port_buffers[pn++] = &umi->midi_buffer;
    }
    for(GList *p = uii->rt_midi_input_ports; p; p = p->next)
    {
        struct cbox_usb_midi_input *umi = p->data;
        int res = usbio_transfer_submit(umi->transfer);
        if (res != 0)
        {
            usbio_transfer_destroy(umi->transfer);
            umi->transfer = NULL;
        }
    }
}

void usbio_stop_midi_capture(struct cbox_usb_io_impl *uii)
{
    for(GList *p = uii->rt_midi_input_ports; p; p = p->next)
    {
        struct cbox_usb_midi_input *umi = p->data;
        
        if (!umi->transfer)
            continue;

        usbio_transfer_shutdown(umi->transfer);
        usbio_transfer_destroy(umi->transfer);
        umi->transfer = NULL;
        cbox_midi_buffer_clear(&umi->midi_buffer);
    }
    free(uii->midi_input_port_buffers);
    free(uii->midi_input_port_pos);
    g_list_free(uii->rt_midi_input_ports);
}

void cbox_usb_midi_info_init(struct cbox_usb_midi_info *umi, struct cbox_usb_device_info *udi)
{
    umi->udi = udi;
    umi->intf = -1;
    umi->alt_setting = -1;
    umi->epdesc.found = FALSE;
}

struct cbox_usb_midi_input *usbio_open_midi_interface(struct cbox_usb_io_impl *uii, const struct cbox_usb_midi_info *uminf, struct libusb_device_handle *handle)
{
    struct cbox_usb_device_info *devinfo = uminf->udi;
    int bus = devinfo->bus;
    int devadr = devinfo->devadr;
    GError *error = NULL;
    // printf("Has MIDI port\n");
    // printf("Output endpoint address = %02x\n", ep->bEndpointAddress);
    if (!configure_usb_interface(handle, bus, devadr, uminf->intf, uminf->alt_setting, &error))
    {
        g_warning("%s", error->message);
        g_error_free(error);
        return NULL;
    }
    
    struct cbox_usb_midi_input *umi = malloc(sizeof(struct cbox_usb_midi_input));
    umi->uii = uii;
    umi->devinfo = devinfo;
    umi->handle = handle;
    umi->busdevadr = devinfo->busdevadr;
    umi->endpoint = uminf->epdesc.bEndpointAddress;
    cbox_midi_buffer_init(&umi->midi_buffer);
    uii->midi_input_ports = g_list_prepend(uii->midi_input_ports, umi);
    int len = uminf->epdesc.wMaxPacketSize;
    if (len > 256)
        len = 256;
    umi->max_packet_size = len;
    
    // Drain the output buffer of the device - otherwise playing a few notes and running the program will cause
    // those notes to play.
    char flushbuf[256];
    int transferred = 0;
    while(0 == libusb_bulk_transfer(handle, umi->endpoint, flushbuf, umi->max_packet_size, &transferred, 10) && transferred > 0)
        usleep(1000);
    devinfo->status = CBOX_DEVICE_STATUS_OPENED;
    
    return umi;
error:
    return NULL;
}

