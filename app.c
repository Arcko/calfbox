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

#include "app.h"
#include "blob.h"
#include "config-api.h"
#include "instr.h"
#include "io.h"
#include "layer.h"
#include "menu.h"
#include "menuitem.h"
#include "meter.h"
#include "midi.h"
#include "module.h"
#include "scene.h"
#include "seq.h"
#include "song.h"
#include "track.h"
#include "ui.h"
#include "wavebank.h"

#include <assert.h>
#include <glib.h>
#include <glob.h>
#include <getopt.h>
#include <math.h>
#include <ncurses.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

static gboolean app_process_cmd(struct cbox_command_target *ct, struct cbox_command_target *fb, struct cbox_osc_command *cmd, GError **error)
{
    if (!cmd->command)
    {
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "NULL command");
        return FALSE;
    }
    if (cmd->command[0] != '/')
    {
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Invalid global command path '%s'", cmd->command);
        return FALSE;
    }
    const char *obj = &cmd->command[1];
    const char *pos = strchr(obj, '/');
    if (pos)
    {
        if (!strncmp(obj, "master/", 7))
            return cbox_execute_sub(&app.rt->master->cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "config/", 7))
            return cbox_execute_sub(&app.config_cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "scene/", 6))
            return cbox_execute_sub(&app.rt->scene->cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "rt/", 3))
            return cbox_execute_sub(&app.rt->cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "song/", 5))
            return cbox_execute_sub(&app.rt->master->song->cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "waves/", 6))
            return cbox_execute_sub(&cbox_waves_cmd_target, fb, cmd, pos, error);
        else
        if (!strncmp(obj, "doc/", 4))
            return cbox_execute_sub(cbox_document_get_cmd_target(app.document), fb, cmd, pos, error);
        else
        {
            g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Unknown combination of target path and argument: '%s', '%s'", cmd->command, cmd->arg_types);
            return FALSE;
        }
    }
    else
    if (!strcmp(obj, "on_idle") && !strcmp(cmd->arg_types, ""))
    {
        cbox_app_on_idle();
        return TRUE;
    }
    else
    if (!strcmp(obj, "send_event") && (!strcmp(cmd->arg_types, "iii") || !strcmp(cmd->arg_types, "ii") || !strcmp(cmd->arg_types, "i")))
    {
        int mcmd = CBOX_ARG_I(cmd, 0);
        int arg1 = 0, arg2 = 0;
        if (cmd->arg_types[1] == 'i')
        {
            arg1 = CBOX_ARG_I(cmd, 1);
            if (cmd->arg_types[2] == 'i')
                arg2 = CBOX_ARG_I(cmd, 2);
        }
        struct cbox_midi_buffer buf;
        cbox_midi_buffer_init(&buf);
        cbox_midi_buffer_write_inline(&buf, 0, mcmd, arg1, arg2);
        cbox_rt_send_events(app.rt, &buf);
        return TRUE;
    }
    else
    if (!strcmp(obj, "play_note") && !strcmp(cmd->arg_types, "iii"))
    {
        int channel = CBOX_ARG_I(cmd, 0);
        int note = CBOX_ARG_I(cmd, 1);
        int velocity = CBOX_ARG_I(cmd, 2);
        struct cbox_midi_buffer buf;
        cbox_midi_buffer_init(&buf);
        cbox_midi_buffer_write_inline(&buf, 0, 0x90 + ((channel - 1) & 15), note & 127, velocity & 127);
        cbox_midi_buffer_write_inline(&buf, 1, 0x80 + ((channel - 1) & 15), note & 127, velocity & 127);
        cbox_rt_send_events(app.rt, &buf);
        return TRUE;
    }
    else
    if (!strcmp(obj, "update_playback") && !strcmp(cmd->arg_types, ""))
    {
        cbox_rt_update_song_playback(app.rt);
        return TRUE;
    }
    else
    if (!strcmp(obj, "get_pattern") && !strcmp(cmd->arg_types, ""))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;
        
        if (app.rt->master->song && app.rt->master->song->tracks)
        {
            struct cbox_track *track = app.rt->master->song->tracks->data;
            if (track)
            {
                struct cbox_track_item *item = track->items->data;
                struct cbox_midi_pattern *pattern = item->pattern;
                int length = 0;
                struct cbox_blob *blob = cbox_midi_pattern_to_blob(pattern, &length);
                gboolean res = cbox_execute_on(fb, NULL, "/pattern", "bi", error, blob, length);
                cbox_blob_destroy(blob);
                if (!res)
                    return FALSE;
            }
        }
        return TRUE;
    }
    else
    if (!strcmp(obj, "new_meter") && !strcmp(cmd->arg_types, ""))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;

        struct cbox_meter *meter = cbox_meter_new(app.document, cbox_rt_get_sample_rate(app.rt));

        return cbox_execute_on(fb, NULL, "/uuid", "o", error, meter);
    }
    else
    if (!strcmp(obj, "new_recorder") && !strcmp(cmd->arg_types, "s"))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;

        struct cbox_recorder *rec = cbox_recorder_new_stream(app.rt, CBOX_ARG_S(cmd, 0));

        return cbox_execute_on(fb, NULL, "/uuid", "o", error, rec);
    }
    else
    if (!strcmp(obj, "new_scene") && !strcmp(cmd->arg_types, "ii"))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;

        struct cbox_rt *rt = cbox_rt_new(app.document);
        cbox_rt_set_offline(rt, CBOX_ARG_I(cmd, 0), CBOX_ARG_I(cmd, 1));
        struct cbox_scene *rec = cbox_scene_new(app.document, rt, TRUE);

        return cbox_execute_on(fb, NULL, "/uuid", "o", error, rec);
    }
    else
    if (!strcmp(obj, "print_s") && !strcmp(cmd->arg_types, "s"))
    {
        g_message("Print: %s", CBOX_ARG_S(cmd, 0));
        return TRUE;
    }
    else
    if (!strcmp(obj, "print_i") && !strcmp(cmd->arg_types, "i"))
    {
        g_message("Print: %d", CBOX_ARG_I(cmd, 0));
        return TRUE;
    }
    else
    if (!strcmp(obj, "print_f") && !strcmp(cmd->arg_types, "f"))
    {
        g_message("Print: %f", CBOX_ARG_F(cmd, 0));
        return TRUE;
    }
    else
    {
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Unknown combination of target path and argument: '%s', '%s'", cmd->command, cmd->arg_types);
        return FALSE;
    }
}

struct config_foreach_data
{
    const char *prefix;
    const char *command;
    struct cbox_command_target *fb;
    GError **error;
    gboolean success;
};

void api_config_cb(void *user_data, const char *key)
{
    struct config_foreach_data *cfd = user_data;
    if (!cfd->success)
        return;
    if (cfd->prefix && strncmp(cfd->prefix, key, strlen(cfd->prefix)))
        return;
    
    if (!cbox_execute_on(cfd->fb, NULL, cfd->command, "s", cfd->error, key))
    {
        cfd->success = FALSE;
        return;
    }
}

static gboolean config_process_cmd(struct cbox_command_target *ct, struct cbox_command_target *fb, struct cbox_osc_command *cmd, GError **error)
{
    if (!strcmp(cmd->command, "/sections") && (!strcmp(cmd->arg_types, "") || !strcmp(cmd->arg_types, "s")))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;
        
        struct config_foreach_data cfd = {cmd->arg_types[0] == 's' ? CBOX_ARG_S(cmd, 0) : NULL, "/section", fb, error, TRUE};
        cbox_config_foreach_section(api_config_cb, &cfd);
        return cfd.success;
    }
    else if (!strcmp(cmd->command, "/keys") && (!strcmp(cmd->arg_types, "s") || !strcmp(cmd->arg_types, "ss")))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;
        
        struct config_foreach_data cfd = {cmd->arg_types[1] == 's' ? CBOX_ARG_S(cmd, 1) : NULL, "/key", fb, error, TRUE};
        cbox_config_foreach_key(api_config_cb, CBOX_ARG_S(cmd, 0), &cfd);
        return cfd.success;
    }
    else if (!strcmp(cmd->command, "/get") && !strcmp(cmd->arg_types, "ss"))
    {
        if (!cbox_check_fb_channel(fb, cmd->command, error))
            return FALSE;
        
        const char *value = cbox_config_get_string(CBOX_ARG_S(cmd, 0), CBOX_ARG_S(cmd, 1));
        if (!value)
            return TRUE;
        return cbox_execute_on(fb, NULL, "/value", "s", error, value);
    }
    else if (!strcmp(cmd->command, "/set") && !strcmp(cmd->arg_types, "sss"))
    {
        cbox_config_set_string(CBOX_ARG_S(cmd, 0), CBOX_ARG_S(cmd, 1), CBOX_ARG_S(cmd, 2));
        return TRUE;
    }
    else if (!strcmp(cmd->command, "/delete") && !strcmp(cmd->arg_types, "ss"))
    {
        cbox_config_remove_key(CBOX_ARG_S(cmd, 0), CBOX_ARG_S(cmd, 1));
        return TRUE;
    }
    else if (!strcmp(cmd->command, "/delete_section") && !strcmp(cmd->arg_types, "s"))
    {
        cbox_config_remove_section(CBOX_ARG_S(cmd, 0));
        return TRUE;
    }
    else if (!strcmp(cmd->command, "/save") && !strcmp(cmd->arg_types, ""))
    {
        return cbox_config_save(NULL, error);
    }
    else if (!strcmp(cmd->command, "/save") && !strcmp(cmd->arg_types, "s"))
    {
        return cbox_config_save(CBOX_ARG_S(cmd, 0), error);
    }
    else
    {
        g_set_error(error, CBOX_MODULE_ERROR, CBOX_MODULE_ERROR_FAILED, "Unknown combination of target path and argument: '%s', '%s'", cmd->command, cmd->arg_types);
        return FALSE;
    }
}

//////////////////////////////////////////////////////////////////////////////////////////////////////////////////////////

void cbox_app_on_idle()
{
    if (app.rt->io)
    {
        GError *error = NULL;
        if (cbox_io_get_disconnect_status(&app.io, &error))
            cbox_io_poll_ports(&app.io);
        else
        {
            if (error)
                g_error_free(error);
            int auto_reconnect = cbox_config_get_int("io", "auto_reconnect", 0);
            if (auto_reconnect > 0)
            {
                sleep(auto_reconnect);
                GError *error = NULL;
                if (!cbox_io_cycle(&app.io, &error))
                    g_warning("Cannot cycle the I/O: %s", (error && error->message) ? error->message : "Unknown error");
            }
        }
    }
    if (app.rt)
        cbox_rt_handle_cmd_queue(app.rt);    
}

struct cbox_app app =
{
    .rt = NULL,
    .current_scene_name = NULL,
    .cmd_target =
    {
        .process_cmd = app_process_cmd,
        .user_data = &app
    },
    .config_cmd_target =
    {
        .process_cmd = config_process_cmd,
        .user_data = &app
    },
};

