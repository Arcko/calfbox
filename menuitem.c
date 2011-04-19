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

#include "menu.h"
#include "menuitem.h"
#include <malloc.h>

/*******************************************************************/

static void item_measure(struct cbox_menu_item *item, struct cbox_menu_state *state, struct cbox_menu_measure *m)
{
    gchar *value = item->item_class->format_value(item, state);
    
    int len = strlen(item->label);
    int len2 = value ? strlen(value) : 0;
    if (len > m->label_width)
        m->label_width = len;
    if (len2 > m->value_width)
        m->value_width = len2;
    m->height++;
    
    g_free(value);
}

static void item_destroy(struct cbox_menu_item *item)
{
    g_free(item->label);
    g_free(item);
}

/*******************************************************************/

static gchar *command_format(const struct cbox_menu_item *item, struct cbox_menu_state *state)
{
    return g_strdup("*");
}

static int command_on_key(struct cbox_menu_item *item, struct cbox_menu_state *state, int key)
{
    if (key == 10)
    {
        struct cbox_menu_item_command *citem = (struct cbox_menu_item_command *)item;
        return citem->execute(citem, state->context);
    }
    return 0;
}

struct cbox_menu_item_class menu_item_class_command = {
    .measure = item_measure,
    .draw = NULL,
    .format_value = command_format,
    .on_idle = NULL,
    .on_key = command_on_key,
    .destroy = item_destroy
};

/*******************************************************************/

void static_draw(struct cbox_menu_item *item, struct cbox_menu_state *state, int *y, int *x, gchar *value, int hilited)
{
    if (!value)
    {
        wattron(state->window, A_BOLD);
        mvwprintw(state->window, *y, *x, "%-*s", state->label_width + state->value_width, item->label);
        wattroff(state->window, A_BOLD);
    }
    else
        mvwprintw(state->window, *y, *x, "%-*s %*s", state->label_width, item->label, state->value_width, value);
    (*y)++;
}

static gchar *static_format(const struct cbox_menu_item *item, struct cbox_menu_state *state)
{
    struct cbox_menu_item_static *sitem = (struct cbox_menu_item_static *)item;
    if (!sitem->format_value)
        return NULL;
    return sitem->format_value(sitem, state->context);
}

struct cbox_menu_item_class menu_item_class_static = {
    .measure = item_measure,
    .draw = static_draw,
    .format_value = static_format,
    .on_idle = NULL,
    .on_key = NULL,
    .destroy = item_destroy
};

/*******************************************************************/

static gchar *intvalue_format(const struct cbox_menu_item *item, struct cbox_menu_state *state)
{
    struct cbox_menu_item_int *iitem = (struct cbox_menu_item_int *)item;
    
    return g_strdup_printf("%d", *iitem->value);
}

static int intvalue_on_key(struct cbox_menu_item *item, struct cbox_menu_state *state, int key)
{
    struct cbox_menu_item_int *iitem = (struct cbox_menu_item_int *)item;
    int *pv;
    
    switch(key)
    {
    case KEY_LEFT:
        pv = iitem->value;
        if (*pv > iitem->vmin)
        {
            (*pv)--;
            if (iitem->on_change)
                iitem->on_change(iitem, state->context);
            return -1;
        }
        return 0;
    case KEY_RIGHT:
        pv = iitem->value;
        if (*pv < iitem->vmax)
        {
            (*pv)++;
            if (iitem->on_change)
                iitem->on_change(iitem, state->context);
            return -1;
        }
        return 0;
    }
    return 0;
}

struct cbox_menu_item_class menu_item_class_int = {
    .measure = item_measure,
    .draw = NULL,
    .format_value = intvalue_format,
    .on_idle = NULL,
    .on_key = intvalue_on_key,
    .destroy = item_destroy
};


/*******************************************************************/

struct cbox_menu_item *cbox_menu_item_new_command(const char *label, cbox_menu_item_execute_func exec, void *item_context)
{
    struct cbox_menu_item_command *item = malloc(sizeof(struct cbox_menu_item_command));
    item->item.label = g_strdup(label);
    item->item.item_class = &menu_item_class_command;
    item->item.item_context = item_context;
    item->execute = exec;
    return &item->item;
}

struct cbox_menu_item *cbox_menu_item_new_static(const char *label, cbox_menu_item_format_value fmt, void *item_context)
{
    struct cbox_menu_item_static *item = malloc(sizeof(struct cbox_menu_item_static));
    item->item.label = g_strdup(label);
    item->item.item_class = &menu_item_class_static;
    item->item.item_context = item_context;
    item->format_value = fmt;
    return &item->item;
}

struct cbox_menu_item *cbox_menu_item_new_int(const char *label, int *value, int vmin, int vmax, void *item_context)
{
    struct cbox_menu_item_int *item = malloc(sizeof(struct cbox_menu_item_int));
    item->item.label = g_strdup(label);
    item->item.item_class = &menu_item_class_int;
    item->item.item_context = item_context;
    item->value = value;
    item->vmin = vmin;
    item->vmax = vmax;
    item->on_change = NULL;
    return &item->item;
}

void cbox_menu_item_destroy(struct cbox_menu_item *item)
{
    item->item_class->destroy(item);
}
