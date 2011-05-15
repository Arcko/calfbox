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

#include "app.h"
#include "config-api.h"
#include "instr.h"
#include "io.h"
#include "layer.h"
#include "menu.h"
#include "menuitem.h"
#include "midi.h"
#include "module.h"
#include "procmain.h"
#include "scene.h"
#include "ui.h"

#include <assert.h>
#include <glib.h>
#include <glob.h>
#include <getopt.h>
#include <math.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

int cmd_quit(struct cbox_menu_item_command *item, void *context)
{
    return 1;
}

void switch_scene(struct cbox_menu_item_command *item, struct cbox_scene *new_scene, const char *prefix)
{
    struct cbox_scene *old = cbox_rt_set_scene(app.rt, new_scene);
    if (old)
    {
        cbox_scene_destroy(old);
        g_free(app.current_scene_name);
    }
    app.current_scene_name = g_strdup_printf("%s:%s", prefix, (char *)item->item.item_context);
}

int cmd_load_scene(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_scene *scene = cbox_scene_load(item->item.item_context);
    switch_scene(item, scene, "scene");
    return 0;
}

int cmd_load_instrument(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_scene *scene = cbox_scene_new();
    struct cbox_layer *layer = cbox_layer_new((char *)item->item.item_context);
    
    if (layer)
    {
        cbox_scene_add_layer(scene, layer);
        switch_scene(item, scene, "instrument");
    }
    else
        cbox_scene_destroy(scene);
    return 0;
}

int cmd_load_layer(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_scene *scene = cbox_scene_new();
    struct cbox_layer *layer = cbox_layer_load((char *)item->item.item_context);
    
    if (layer)
    {
        cbox_scene_add_layer(scene, layer);
        switch_scene(item, scene, "layer");
    }
    else
        cbox_scene_destroy(scene);
    return 0;
}

gchar *scene_format_value(const struct cbox_menu_item_static *item, void *context)
{
    return strdup(app.current_scene_name);
}

gchar *transport_format_value(const struct cbox_menu_item_static *item, void *context)
{
    struct cbox_bbt bbt;
    cbox_master_to_bbt(app.rt->master, &bbt);
    if (!strcmp((const char *)item->item.item_context, "pos"))
        return g_strdup_printf("%d", (int)app.rt->master->song_pos_samples);
    else
        return g_strdup_printf("%d:%d:%02d", bbt.bar, bbt.beat, bbt.tick);
}

struct cbox_config_section_cb_data
{
    struct cbox_menu *menu;
    cbox_menu_item_execute_func func;
    const char *prefix;
};

static void config_key_process(void *user_data, const char *key)
{
    struct cbox_config_section_cb_data *data = user_data;
    
    if (!strncmp(key, data->prefix, strlen(data->prefix)))
    {
        char *title = cbox_config_get_string(key, "title");
        if (title)
            cbox_menu_add_item(data->menu, cbox_menu_item_new_command(title, data->func, strdup(key + strlen(data->prefix))));
        else
            cbox_menu_add_item(data->menu, cbox_menu_item_new_command(key, data->func, strdup(key + strlen(data->prefix))));
    }
}

struct cbox_menu *create_scene_menu(struct cbox_menu_item_menu *item, void *menu_context)
{
    struct cbox_menu *scene_menu = cbox_menu_new();
    struct cbox_config_section_cb_data cb = { .menu = scene_menu };

    cbox_menu_add_item(scene_menu, cbox_menu_item_new_static("Scenes", NULL, NULL));
    cb.prefix = "scene:";
    cb.func = cmd_load_scene;
    cbox_config_foreach_section(config_key_process, &cb);
    cbox_menu_add_item(scene_menu, cbox_menu_item_new_static("Layers", NULL, NULL));
    cb.prefix = "layer:";
    cb.func = cmd_load_layer;
    cbox_config_foreach_section(config_key_process, &cb);
    cbox_menu_add_item(scene_menu, cbox_menu_item_new_static("Instruments", NULL, NULL));
    cb.prefix = "instrument:";
    cb.func = cmd_load_instrument;
    cbox_config_foreach_section(config_key_process, &cb);
    
    cbox_menu_add_item(scene_menu, cbox_menu_item_new_menu("OK", NULL, NULL));    
    
    return scene_menu;
}

///////////////////////////////////////////////////////////////////////////////

static struct cbox_command_target *find_module_target(const char *type)
{
    struct cbox_scene *scene = app.rt->scene;
    for (int i = 0; i < scene->instrument_count; i++)
    {
        if (!strcmp(scene->instruments[i]->engine_name, type))
            return &scene->instruments[i]->module->cmd_target;
    }
    return NULL;
    
}

int cmd_stream_rewind(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_command_target *target = find_module_target("stream_player");
    if (target)
        cbox_execute_on(target, "/seek", "i", 0);
    return 0;
}

int cmd_stream_play(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_command_target *target = find_module_target("stream_player");
    if (target)
        cbox_execute_on(target, "/play", "");
    return 0;
}

int cmd_stream_stop(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_command_target *target = find_module_target("stream_player");
    if (target)
        cbox_execute_on(target, "/stop", "");
    return 0;
}

int cmd_stream_load(struct cbox_menu_item_command *item, void *context)
{
    struct cbox_command_target *target = find_module_target("stream_player");
    if (target)
        cbox_execute_on(target, "/load", "si", (gchar *)item->item.item_context, 0);
    return 0;
}

struct cbox_menu *create_stream_menu(struct cbox_menu_item_menu *item, void *menu_context)
{
    struct cbox_menu *menu = cbox_menu_new();
    struct cbox_config_section_cb_data cb = { .menu = menu };

    cbox_menu_add_item(menu, cbox_menu_item_new_static("Module commands", NULL, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("Play stream", cmd_stream_play, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("Stop stream", cmd_stream_stop, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("Rewind stream", cmd_stream_rewind, NULL));

    glob_t g;
    if (glob("*.wav", GLOB_TILDE_CHECK, NULL, &g) == 0)
    {
        for (int i = 0; i < g.gl_pathc; i++)
        {
            cbox_menu_add_item(menu, cbox_menu_item_new_command(g_strdup_printf("Load: %s", g.gl_pathv[i]), cmd_stream_load, g_strdup(g.gl_pathv[i])));
        }
    }
    
    globfree(&g);
        
    cbox_menu_add_item(menu, cbox_menu_item_new_menu("OK", NULL, NULL));    
    
    return menu;
}

///////////////////////////////////////////////////////////////////////////////

static void cbox_rt_set_pattern_and_destroy(struct cbox_rt *rt, struct cbox_midi_pattern *pattern)
{
    struct cbox_midi_pattern *old = cbox_rt_set_pattern(rt, pattern, 0);
    if (old)
        cbox_midi_pattern_destroy(old);
}

int cmd_pattern_none(struct cbox_menu_item_command *item, void *context)
{
    cbox_rt_set_pattern_and_destroy(app.rt, NULL);
    return 0;
}

int cmd_pattern_simple(struct cbox_menu_item_command *item, void *context)
{
    cbox_rt_set_pattern_and_destroy(app.rt, cbox_midi_pattern_new_metronome(1));
    return 0;
}

int cmd_pattern_normal(struct cbox_menu_item_command *item, void *context)
{
    cbox_rt_set_pattern_and_destroy(app.rt, cbox_midi_pattern_new_metronome(app.rt->master->timesig_nom));
    return 0;
}

int cmd_load_drumpattern(struct cbox_menu_item_command *item, void *context)
{
    cbox_rt_set_pattern_and_destroy(app.rt, cbox_midi_pattern_load_drum(item->item.item_context));
    return 0;
}

int cmd_load_drumtrack(struct cbox_menu_item_command *item, void *context)
{
    cbox_rt_set_pattern_and_destroy(app.rt, cbox_midi_pattern_load_drum_track(item->item.item_context));
    return 0;
}

struct cbox_menu *create_pattern_menu(struct cbox_menu_item_menu *item, void *menu_context)
{
    struct cbox_menu *menu = cbox_menu_new();
    struct cbox_config_section_cb_data cb = { .menu = menu };

    cbox_menu_add_item(menu, cbox_menu_item_new_static("Pattern commands", NULL, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("No pattern", cmd_pattern_none, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("Simple metronome", cmd_pattern_simple, NULL));
    cbox_menu_add_item(menu, cbox_menu_item_new_command("Normal metronome", cmd_pattern_normal, NULL));

    cbox_menu_add_item(menu, cbox_menu_item_new_static("Drum tracks", NULL, NULL));
    cb.prefix = "drumtrack:";
    cb.func = cmd_load_drumtrack;
    cbox_config_foreach_section(config_key_process, &cb);
    cbox_menu_add_item(menu, cbox_menu_item_new_static("Drum patterns", NULL, NULL));
    cb.prefix = "drumpattern:";
    cb.func = cmd_load_drumpattern;
    cbox_config_foreach_section(config_key_process, &cb);

    cbox_menu_add_item(menu, cbox_menu_item_new_menu("OK", NULL, NULL));    
    
    return menu;
}

struct cbox_menu *create_main_menu()
{
    struct cbox_menu *main_menu = cbox_menu_new();
    cbox_menu_add_item(main_menu, cbox_menu_item_new_static("Current scene:", scene_format_value, NULL));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_dynamic_menu("Set scene", create_scene_menu, NULL));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_dynamic_menu("Module control", create_stream_menu, NULL));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_dynamic_menu("Pattern control", create_pattern_menu, NULL));
    
    cbox_menu_add_item(main_menu, cbox_menu_item_new_static("Variables", NULL, NULL));
    // cbox_menu_add_item(main_menu, cbox_menu_item_new_int("foo:", &var1, 0, 127, NULL));
    // cbox_menu_add_item(main_menu, "bar:", menu_item_value_double, &mx_double_var2, &var2);
    cbox_menu_add_item(main_menu, cbox_menu_item_new_static("pos:", transport_format_value, "pos"));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_static("bbt:", transport_format_value, "bbt"));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_static("Commands", NULL, NULL));
    cbox_menu_add_item(main_menu, cbox_menu_item_new_command("Quit", cmd_quit, NULL));
    return main_menu;
}

static void app_process_cmd(struct cbox_command_target *ct, struct cbox_osc_command *cmd)
{
    if (!cmd->command)
    {
        g_error("NULL command");
        return;
    }
    if (cmd->command[0] != '/')
    {
        g_error("Invalid global command path %s", cmd->command);
        return;
    }
    const char *obj = &cmd->command[1];
    const char *pos = strchr(obj, '/');
    if (pos)
    {
        int len = pos - obj;
        if (!strncmp(obj, "instr", len))
        {
            obj = &pos[1];
            pos = strchr(obj, '/');
            if (!pos)
            {
                g_error("Invalid instrument path %s", cmd->command);
                return;
            }
            len = pos - obj;
            
            gchar *name = g_strndup(obj, len);
            struct cbox_instrument *instr = cbox_instruments_get_by_name(name);
            if (instr)
            {
                if (!instr->module->cmd_target.process_cmd)
                {
                    g_error("The engine %s has no command target defined\n", instr->engine_name);
                    return;
                }
                struct cbox_osc_command subcmd;
                subcmd.command = pos;
                subcmd.arg_types = cmd->arg_types;
                subcmd.arg_values = cmd->arg_values;
                instr->module->cmd_target.process_cmd(&instr->module->cmd_target, &subcmd);
            }
            else
            {
                g_error("Invalid instrument path %s", cmd->command);
            }
            g_free(name);
            return;
        }
    }
    else
    if (!strcmp(obj, "print_s") && !strcmp(cmd->arg_types, "s"))
    {
        g_message("Print: %s", (const char *)cmd->arg_values[0]);
    }
    else
    if (!strcmp(obj, "print_i") && !strcmp(cmd->arg_types, "i"))
    {
        g_message("Print: %d", *(const int *)cmd->arg_values[0]);
    }
    else
    if (!strcmp(obj, "print_f") && !strcmp(cmd->arg_types, "f"))
    {
        g_message("Print: %f", *(const double *)cmd->arg_values[0]);
    }
    else
    {
        g_error("Unknown combination of target path and argument: %s, %s", cmd->command, cmd->arg_types);
        return;
    }
}

struct cbox_app app =
{
    .rt = NULL,
    .current_scene_name = NULL,
    .cmd_target =
    {
        .process_cmd = app_process_cmd,
        .user_data = &app
    }
};

