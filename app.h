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

#ifndef CBOX_APP_H
#define CBOX_APP_H

#include "cmd.h"
#include "io.h"
#include "procmain.h"
#include <glib.h>

struct cbox_app
{
    struct cbox_io io;
    struct cbox_rt *rt;
    struct cbox_command_target cmd_target;
    struct cbox_command_target config_cmd_target;
    struct cbox_command_target waves_cmd_target;
    gchar *current_scene_name;
};

struct cbox_menu;
struct cbox_menu_item_menu;

extern struct cbox_app app;

struct cbox_menu *create_main_menu();

extern void cbox_app_on_idle();

#endif
