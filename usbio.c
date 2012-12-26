/*
Calf Box, an open source musical instrument.
Copyright (C) 2010-2012 Krzysztof Foltman

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

// Note: this is a silly experimental driver for Lexicon Omega audio/MIDI
// interface. It's more a proof of concept than anything useful for now.
// It only handles audio output - audio input and MIDI input will be
// added at some point in future. MIDI output, too, perhaps.
// Any error handling (including handling of disconnect and reconnect)
// will also be added later.
// Eventually, I might make it compatible with more of class-compliant audio
// and MIDI interfaces, plus some non-compliant ones like Alesis Multimix 8.

#include "config.h"
#include "config-api.h"
#include "errors.h"
#include "hwcfg.h"
#include "io.h"
#include "meter.h"
#include "midi.h"
#include "recsrc.h"

#include <errno.h>
#include <glib.h>
#include <libusb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// wMaxPacketSize from the Omega audio endpoints
#define MAX_EP_PACKET_SIZE_BYTES 192

#define OMEGA_TIMEOUT 2000

#define OMEGA_EP_PLAYBACK 0x01
#define OMEGA_EP_CAPTURE 0x83
#define OMEGA_EP_MIDI_PLAYBACK 0x04
#define OMEGA_EP_MIDI_CAPTURE 0x84

struct cbox_usb_io_impl
{
    struct cbox_io_impl ioi;

    struct libusb_context *usbctx;
    struct libusb_device_handle *handle_omega;
    int sample_rate, buffer_size;
    unsigned int buffers, iso_packets;

    pthread_t thr_engine;
    volatile gboolean stop_engine;
    
    int desync;
    uint64_t samples_played;
    int cancel_confirm;
    int playback_counter;
    struct libusb_transfer **playback_transfers;
    int read_ptr;
    struct cbox_midi_buffer midi_buffer;
    
    uint8_t midi_recv_data[16];
};

///////////////////////////////////////////////////////////////////////////////

int cbox_usbio_get_sample_rate(struct cbox_io_impl *impl)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;
    
    return uii->sample_rate;
}

gboolean cbox_usbio_get_status(struct cbox_io_impl *impl, GError **error)
{
    // XXXKF: needs a flag that would indicate whether device is present
    // XXXKF: needs to return that flag with appropriate message
    return TRUE;
}

static struct libusb_transfer *play_stuff(struct cbox_usb_io_impl *uii, int index);

static void play_callback(struct libusb_transfer *transfer)
{
    struct cbox_usb_io_impl *uii = transfer->user_data;
    
    int i, j, b;
    if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
    {
        uii->cancel_confirm = 1;
        return;
    }
    gboolean init_finished = uii->playback_counter == uii->buffers;
    if (uii->playback_counter < uii->buffers)
    {
        // send another URB for the next transfer before re-submitting
        // this one
        uii->playback_transfers[uii->playback_counter] = play_stuff(uii, uii->playback_counter);
        uii->playback_counter++;
    }
    // printf("Play Callback! %d %p status %d\n", (int)transfer->length, transfer->buffer, (int)transfer->status);

    int tlen = 0;
    for (i = 0; i < transfer->num_iso_packets; i++)
    {
        tlen += transfer->iso_packet_desc[i].actual_length;
        if (transfer->iso_packet_desc[i].status)
            printf("ISO error: index = %d i = %d status = %d\n", (int)transfer->user_data, i, transfer->iso_packet_desc[i].status);
    }
    uii->samples_played += transfer->length / 4;
    int nsamps = uii->sample_rate / 1000;
    // If time elapsed is greater than 
    int lag = uii->desync / (1000 * transfer->num_iso_packets);
    if (lag > 0 && nsamps < MAX_EP_PACKET_SIZE_BYTES / 4)
    {
        nsamps++;
        lag--;
    }

    transfer->length = nsamps * transfer->num_iso_packets * 4;
    libusb_set_iso_packet_lengths(transfer, nsamps * 4);

    //printf("desync %d num_iso_packets %d srate %d tlen %d\n", uii->desync, transfer->num_iso_packets, uii->sample_rate, tlen);
    //printf("+ %d - %d ptlen %d\n", transfer->num_iso_packets * uii->sample_rate, tlen / 4 * 1000, transfer->length / 4);

    if (init_finished)
    {
        static double phase = 0;
        static int phase2 = 0;
        struct cbox_io *io = uii->ioi.pio;
        int16_t *data = (int16_t*)transfer->buffer;
        int rptr = uii->read_ptr;
        int nframes = transfer->length / 4;
        for (i = 0; i < nframes; )
        {
            if (rptr == io->buffer_size)
            {
                memset(io->output_buffers[0], 0, io->buffer_size * sizeof(float));
                memset(io->output_buffers[1], 0, io->buffer_size * sizeof(float));
                io->cb->process(io->cb->user_data, io, io->buffer_size);
                cbox_midi_buffer_clear(&uii->midi_buffer);
                rptr = 0;
            }
            int left1 = nframes - i;
            int left2 = io->buffer_size - rptr;
            if (left1 > left2)
                left1 = left2;

            for (b = 0; b < 2; b++)
            {
                float *obuf = io->output_buffers[b] + rptr;
                int16_t *tbuf = data + 2 * i + b;
                for (j = 0; j < left1; j++)
                {
                    float v = 32767 * obuf[j];
                    if (v < -32768)
                        v = -32768;
                    if (v > +32767)
                        v = +32767;
                    tbuf[2 * j] = (int16_t)v;
                }
            }
            i += left1;
            rptr += left1;
        }
        uii->read_ptr = rptr;
    }
    // desync value is expressed in milli-frames, i.e. desync of 1000 means 1 frame of lag
    // It takes 1ms for each iso packet to be transmitted. Each transfer consists of
    // num_iso_packets packets. So, this transfer took uii->sample_rate milli-frames.
    uii->desync += transfer->num_iso_packets * uii->sample_rate;
    // ... but during that time, tlen/4 samples == tlen/4*1000 millisamples have been
    // transmitted.
    uii->desync -= transfer->num_iso_packets * nsamps * 1000;

    libusb_submit_transfer(transfer);
}

struct libusb_transfer *play_stuff(struct cbox_usb_io_impl *uii, int index)
{
    struct libusb_transfer *t;
    int i;
    t = libusb_alloc_transfer(uii->iso_packets);
    int tsize = uii->sample_rate * 4 / 1000;
    uint8_t *buf = (uint8_t *)malloc(MAX_EP_PACKET_SIZE_BYTES * uii->iso_packets);
    //int ssf = 0;
    
    for (i = 0; i < MAX_EP_PACKET_SIZE_BYTES * uii->iso_packets; i++)
        buf[i] = 0;

    libusb_fill_iso_transfer(t, uii->handle_omega, OMEGA_EP_PLAYBACK, buf, tsize * uii->iso_packets, uii->iso_packets, play_callback, uii, 20000);
    libusb_set_iso_packet_lengths(t, tsize);
    libusb_submit_transfer(t);
    return t;
}

static void midi_transfer_cb(struct libusb_transfer *transfer)
{
    struct cbox_usb_io_impl *uii = transfer->user_data;

    if (transfer->status == LIBUSB_TRANSFER_CANCELLED)
    {
        uii->cancel_confirm = 1;
        return;
    }
    for (int i = 0; i + 3 < transfer->actual_length; i += 4)
    {
        uint8_t *data = &transfer->buffer[i];
        if ((data[0] & 15) >= 0x08)
        {
            // normalise: note on with vel 0 -> note off
            if ((data[1] & 0x90) == 0x90 && data[3] == 0)
                cbox_midi_buffer_write_inline(&uii->midi_buffer, 0, data[1] - 0x10, data[2], data[3]);
            else
                cbox_midi_buffer_write_event(&uii->midi_buffer, 0, data + 1, midi_cmd_size(data[1]));
        }
    }
    libusb_submit_transfer(transfer);
}

static void *engine_thread(void *user_data)
{
    struct cbox_usb_io_impl *uii = user_data;
    
    struct sched_param p;
    p.sched_priority = 10;
    sched_setscheduler(0, SCHED_FIFO, &p);

    cbox_midi_buffer_init(&uii->midi_buffer);
    struct libusb_transfer *midi_transfer = libusb_alloc_transfer(0);
    libusb_fill_bulk_transfer(midi_transfer, uii->handle_omega, OMEGA_EP_MIDI_CAPTURE, uii->midi_recv_data, sizeof(uii->midi_recv_data), midi_transfer_cb, uii, 1000);
    libusb_submit_transfer(midi_transfer);
    
    uii->desync = 0;
    uii->samples_played = 0;
    uii->read_ptr = uii->ioi.pio->buffer_size;
    
    uii->playback_transfers = malloc(sizeof(struct libusb_transfer *) * uii->buffers);
    
    uii->playback_counter = 1;
    uii->playback_transfers[0] = play_stuff(uii, 0);
    while(uii->playback_counter < uii->buffers)
        libusb_handle_events(uii->usbctx);
    
    while(!uii->stop_engine) {
	struct timeval tv = {
            .tv_sec = 0,
            .tv_usec = 100
        };
	libusb_handle_events_timeout(uii->usbctx, &tv);
    }

    for (int i = 0; i < uii->buffers; i++)
    {
        uii->cancel_confirm = FALSE;
        libusb_cancel_transfer(uii->playback_transfers[i]);
        while (!uii->cancel_confirm)
            libusb_handle_events(uii->usbctx);
        libusb_free_transfer(uii->playback_transfers[i]);
    }
    uii->cancel_confirm = FALSE;
    libusb_cancel_transfer(midi_transfer);
    while (!uii->cancel_confirm)
        libusb_handle_events(uii->usbctx);
    libusb_free_transfer(midi_transfer);
    
}

gboolean cbox_usbio_start(struct cbox_io_impl *impl, GError **error)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;

    // XXXKF: needs to queue the playback and capture transfers

    uii->stop_engine = FALSE;
    
    if (pthread_create(&uii->thr_engine, NULL, engine_thread, uii))
    {
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "cannot create engine thread: %s", strerror(errno));
        return FALSE;
    }

    return TRUE;
}

gboolean cbox_usbio_stop(struct cbox_io_impl *impl, GError **error)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;

    // XXXKF: needs to kill the playback and capture transfers, and
    // wait for them to be killed

    uii->stop_engine = TRUE;
    pthread_join(uii->thr_engine, NULL);
    return TRUE;
}

void cbox_usbio_poll_ports(struct cbox_io_impl *impl)
{
    struct cbox_jack_io_impl *jii = (struct cbox_jack_io_impl *)impl;
    // XXXKF: this is for autodetection of newly connected devices,
    // not that I plan any at the moment.
}

gboolean cbox_usbio_cycle(struct cbox_io_impl *impl, GError **error)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;
    // XXXKF: this is for restarting the thing; not implemented for now,
    // the implementation will be something like in case of JACK - close and
    // reopen.
    return TRUE;
}

int cbox_usbio_get_midi_data(struct cbox_io_impl *impl, struct cbox_midi_buffer *destination)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;
    cbox_midi_buffer_clear(destination);
    cbox_midi_buffer_copy(destination, &uii->midi_buffer);
    return 0;
}

void cbox_usbio_destroy(struct cbox_io_impl *impl)
{
    struct cbox_usb_io_impl *uii = (struct cbox_usb_io_impl *)impl;
    if (uii->handle_omega)
        libusb_close(uii->handle_omega);
    libusb_exit(uii->usbctx);
}


static gboolean set_endpoint_sample_rate(struct libusb_device_handle *h, int sample_rate)
{
    uint8_t freq_data[3];
    freq_data[0] = sample_rate & 0xFF;
    freq_data[1] = (sample_rate & 0xFF00) >> 8;
    freq_data[2] = (sample_rate & 0xFF0000) >> 16;
    if (libusb_control_transfer(h, 0x22, 0x01, 256, OMEGA_EP_PLAYBACK, freq_data, 3, OMEGA_TIMEOUT) != 3)
        return FALSE;
    if (libusb_control_transfer(h, 0x22, 0x01, 256, OMEGA_EP_CAPTURE, freq_data, 3, OMEGA_TIMEOUT) != 3)
        return FALSE;
//    libusb_control_transfer(dev, 0x22, 0x01, 
    return TRUE;
}

static gboolean claim_omega_interfaces(struct libusb_device_handle *handle, GError **error)
{
    static int interfaces[] = { 1, 2, 7 };
    for (int i = 0; i < sizeof(interfaces) / sizeof(int); i++)
    {
        int ifno = interfaces[i];
        int err = libusb_kernel_driver_active(handle, ifno);
        if (err)
        {
            if (libusb_detach_kernel_driver(handle, ifno))
            {
                g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Cannot detach kernel driver from Lexicon Omega interface %d: %s. Please rmmod snd-usb-audio as root.", ifno, libusb_error_name(err));
                return FALSE;
            }            
        }
        err = libusb_claim_interface(handle, ifno);
        if (err)
        {
            g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Cannot claim interface %d on Lexicon Omega: %s", ifno, libusb_error_name(err));
            return FALSE;
        }
        if (ifno != 7)
        {
            err = libusb_set_interface_alt_setting(handle, ifno, 1);
            if (err)
            {
                g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Cannot set alternate setting on interface %d on Lexicon Omega: %s", ifno, libusb_error_name(err));
                return FALSE;
            }
        }
    }
    return TRUE;
}

///////////////////////////////////////////////////////////////////////////////

static gboolean open_omega(struct cbox_usb_io_impl *uii, GError **error)
{
    uii->handle_omega = libusb_open_device_with_vid_pid(uii->usbctx, 0x1210, 0x0002);
    
    if (!uii->handle_omega)
    {
        libusb_exit(uii->usbctx);
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Lexicon Omega not found or cannot be opened.");
        free(uii);
        return FALSE;
    }
    if (!claim_omega_interfaces(uii->handle_omega, error))
    {
        libusb_close(uii->handle_omega);
        libusb_exit(uii->usbctx);
        free(uii);
        return FALSE;
    }
    if (!set_endpoint_sample_rate(uii->handle_omega, uii->sample_rate))
    {
        libusb_close(uii->handle_omega);
        libusb_exit(uii->usbctx);
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Cannot set sample rate on Lexicon Omega.");
        free(uii);
        return FALSE;
    }
    return TRUE;
}

gboolean cbox_io_init_usb(struct cbox_io *io, struct cbox_open_params *const params, GError **error)
{
    struct cbox_usb_io_impl *uii = malloc(sizeof(struct cbox_usb_io_impl));
    libusb_init(&uii->usbctx);
    libusb_set_debug(uii->usbctx, 3);

    uii->sample_rate = 44100;
    uii->buffers = cbox_config_get_int(cbox_io_section, "usb_buffers", 2);
    // shouldn't be more than 4, otherwise it will crackle due to limitations of
    // 
    uii->iso_packets = cbox_config_get_int(cbox_io_section, "iso_packets", 1);
    
    if (!open_omega(uii, error))
        return FALSE;

    // fixed processing buffer size, as we have to deal with packetisation anyway
    io->buffer_size = 64;
    io->cb = NULL;
    // input and output count is hardcoded for simplicity - in future, it may be
    // necessary to add support for the extra inputs (needs to be figured out)
    io->input_count = 2; //cbox_config_get_int("io", "inputs", 0);
    io->input_buffers = malloc(sizeof(float *) * io->input_count);
    for (int i = 0; i < io->input_count; i++)
        io->input_buffers[i] = calloc(io->buffer_size, sizeof(float));
    io->output_count = 2; // cbox_config_get_int("io", "outputs", 2);
    io->output_buffers = malloc(sizeof(float *) * io->output_count);
    for (int i = 0; i < io->output_count; i++)
        io->output_buffers[i] = calloc(io->buffer_size, sizeof(float));
    io->impl = &uii->ioi;

    uii->ioi.pio = io;
    uii->ioi.getsampleratefunc = cbox_usbio_get_sample_rate;
    uii->ioi.startfunc = cbox_usbio_start;
    uii->ioi.stopfunc = cbox_usbio_stop;
    uii->ioi.getstatusfunc = cbox_usbio_get_status;
    uii->ioi.pollfunc = cbox_usbio_poll_ports;
    uii->ioi.cyclefunc = cbox_usbio_cycle;
    uii->ioi.getmidifunc = cbox_usbio_get_midi_data;
    uii->ioi.destroyfunc = cbox_usbio_destroy;

    return TRUE;
    
}

