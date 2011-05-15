/*
Calf Box, an open source musical instrument.
Copyright (C) 2010 Krzysztof Foltman

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

#ifndef CBOX_MIDI_H
#define CBOX_MIDI_H

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

struct cbox_midi_event
{
    uint32_t time;
    uint32_t size;
    union {
        uint8_t data_inline[4]; /* up to 4 bytes */
        uint8_t *data_ext; /* if larger than 4 bytes */
    };
};

#define CBOX_MIDI_MAX_EVENTS 256
#define CBOX_MIDI_MAX_LONG_DATA 256

struct cbox_midi_buffer
{
    uint32_t count;
    uint32_t long_data_size;
    struct cbox_midi_event events[CBOX_MIDI_MAX_EVENTS];
    uint8_t long_data[CBOX_MIDI_MAX_LONG_DATA];
};

static inline void cbox_midi_buffer_init(struct cbox_midi_buffer *buffer)
{
    buffer->count = 0;
    buffer->long_data_size = 0;
}

static inline void cbox_midi_buffer_clear(struct cbox_midi_buffer *buffer)
{
    buffer->count = 0;
    buffer->long_data_size = 0;
}

static inline uint32_t cbox_midi_buffer_get_count(struct cbox_midi_buffer *buffer)
{
    return buffer->count;
}

static inline struct cbox_midi_event *cbox_midi_buffer_get_event(struct cbox_midi_buffer *buffer, uint32_t pos)
{
    if (pos >= buffer->count)
        return NULL;
    return &buffer->events[pos];
}

static inline uint8_t *cbox_midi_event_get_data(struct cbox_midi_event *evt)
{
    return evt->size > 4 ? evt->data_ext : evt->data_inline;
}

static inline int cbox_midi_buffer_write_event(struct cbox_midi_buffer *buffer, uint32_t time, uint8_t *data, uint32_t size)
{
    struct cbox_midi_event *evt;
    
    if (buffer->count >= CBOX_MIDI_MAX_EVENTS)
        return 0;
    if (size > 4 && size > CBOX_MIDI_MAX_LONG_DATA - buffer->long_data_size)
        return 0;
    evt = &buffer->events[buffer->count++];
    evt->time = time;
    evt->size = size;
    if (size <= 4)
    {
        memcpy(evt->data_inline, data, size);
    }
    else
    {
        evt->data_ext = buffer->long_data + buffer->long_data_size;
        memcpy(evt->data_ext, data, size);
        buffer->long_data_size += size;
    }
    return 1;
}

static inline int cbox_midi_buffer_copy_event(struct cbox_midi_buffer *buffer, const struct cbox_midi_event *event, int ofs)
{
    struct cbox_midi_event *evt;
    
    if (buffer->count >= CBOX_MIDI_MAX_EVENTS)
        return 0;
    if (event->size > 4 && event->size > CBOX_MIDI_MAX_LONG_DATA - buffer->long_data_size)
        return 0;
    evt = &buffer->events[buffer->count++];
    evt->time = event->time + ofs;
    evt->size = event->size;
    if (event->size <= 4)
    {
        memcpy(evt->data_inline, event->data_inline, event->size);
    }
    else
    {
        evt->data_ext = buffer->long_data + buffer->long_data_size;
        memcpy(evt->data_ext, event->data_ext, event->size);
        buffer->long_data_size += event->size;
    }
    return 1;
}

static inline int note_from_string(const char *note)
{
    static const int semis[] = {9, 11, 0, 2, 4, 5, 7};
    int pos;
    int nn = tolower(note[0]);
    int nv;
    if (nn >= '0' && nn <= '9')
        return atoi(note);
    if (nn < 'a' && nn > 'g')
        return -1;
    nv = semis[nn - 'a'];
    
    for (pos = 1; note[pos] == 'b' || note[pos] == '#'; pos++)
        nv += (note[pos] == 'b') ? -1 : +1;
    
    if ((note[pos] == '-' && note[pos + 1] >= '1' && note[pos + 1] <= '2' && note[pos + 2] == '\0') || (note[pos] >= '0' && note[pos] <= '9' && note[pos + 1] == '\0'))
    {
        return nv + 12 * (2 + atoi(note + pos));
    }
    
    return -1;
}

#endif
