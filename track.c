/*
Calf Box, an open source musical instrument.
Copyright (C) 2010-2011 Krzysztof Foltman

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

#include "track.h"
#include <malloc.h>

void cbox_track_item_destroy(struct cbox_track_item *item)
{
    free(item);
}

struct cbox_track *cbox_track_new()
{
    struct cbox_track *p = malloc(sizeof(struct cbox_track));
    p->items = NULL;
    return p;
}

#define CBTI(it) ((struct cbox_track_item *)(it)->data)

void cbox_track_add_item(struct cbox_track *track, struct cbox_track_item *item)
{
    GList *it = track->items;
    while(it != NULL && CBTI(it)->time < item->time)
        it = g_list_next(it);
    // all items earlier than the new one -> append
    if (it == NULL)
    {
        track->items = g_list_append(track->items, item);
        return;
    }
    // Here, I don't really care about overlaps - it's more important to preserve
    // all clips as sent by the caller.
    track->items = g_list_insert_before(track->items, it, item);
}

void cbox_track_destroy(struct cbox_track *track)
{
    g_list_free_full(track->items, (GDestroyNotify)cbox_track_item_destroy);
    free(track);
}

