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

#include "sampler.h"
#include "sfzparser.h"

#define DUMP_LAYER_ATTRIBS 0

struct sfz_load_state
{
    struct sampler_module *m;
    const char *filename;
    const char *sample_path;
    struct sampler_program *program;
    struct sampler_layer *group;
    struct sampler_layer *region;
    GSList *layers;
    GError **error;
};

static void load_sfz_end_region(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    // printf("-- copy current region to the list of layers\n");
    struct sampler_layer *l = ls->region;
    sampler_layer_finalize(l, ls->m);
    ls->layers = g_slist_prepend(ls->layers, ls->region);

#if DUMP_LAYER_ATTRIBS
    fprintf(stdout, "<region>");
    sampler_layer_dump(ls->region, stdout);
#endif
    ls->region = NULL;
}

static void load_sfz_group(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    if (ls->group && ls->group->waveform)
    {
        cbox_waveform_unref(ls->group->waveform);
        ls->group->waveform = NULL;
    }
    if (ls->region)
        load_sfz_end_region(client);
    // printf("-- start group\n");
    ls->group = sampler_layer_new(NULL);
}

static void load_sfz_region(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    
    if (ls->region)
    {
        load_sfz_end_region(client);
    }
    else
    {
#if DUMP_LAYER_ATTRIBS
        fprintf(stdout, "<group>");
        sampler_layer_dump(&ls->group, stdout);
#endif
    }
    ls->region = sampler_layer_new(ls->group);
    // g_warning("-- start region");
}

static gboolean load_sfz_key_value(struct sfz_parser_client *client, const char *key, const char *value)
{
    struct sfz_load_state *ls = client->user_data;
    struct sampler_layer *l = ls->region ? ls->region : ls->group;
    int unhandled = 0;
    if (!ls->region && !ls->group)
    {
        g_warning("Cannot use parameter '%s' outside of region or group", key);
        return TRUE;
    }
    
    if (!strcmp(key, "sample"))
    {
        if (l->waveform != NULL)
        {
            cbox_waveform_unref(l->waveform);
            l->waveform = NULL;
        }
        gchar *value_copy = g_strdup(value);
        for (int i = 0; value_copy[i]; i++)
        {
            if (value_copy[i] == '\\')
                value_copy[i] = '/';
        }
        gchar *filename = g_build_filename(ls->sample_path ? ls->sample_path : "", value_copy, NULL);
        g_free(value_copy);
        struct cbox_waveform *wf = cbox_wavebank_get_waveform(ls->filename, filename, ls->error);
        g_free(filename);
        if (!wf)
            return FALSE;
        sampler_layer_set_waveform(l, wf);
        if (l->loop_end == 0)
            l->loop_end = l->sample_end;
        return TRUE;
    }
    
    if (!sampler_layer_apply_param(l, key, value))
    {
        g_warning("Unhandled sfz key: %s", key);
        return FALSE;
    }
    return TRUE;
}

gboolean sampler_module_load_program_sfz(struct sampler_module *m, struct sampler_program *prg, const char *sfz, const char *sample_path, int is_from_string, GError **error)
{
    struct sfz_load_state ls = { .group = NULL, .m = m, .filename = sfz, .layers = NULL, .region = NULL, .error = error, .sample_path = sample_path, .program = prg };
    struct sfz_parser_client c = { .user_data = &ls, .region = load_sfz_region, .group = load_sfz_group, .key_value = load_sfz_key_value };
    g_clear_error(error);

    gboolean status = is_from_string ? load_sfz_from_string(sfz, strlen(sfz), &c, error) : load_sfz(sfz, &c, error);
    if (!status)
        return FALSE;

    if (ls.region)
        load_sfz_end_region(&c);
    if (ls.group && ls.group->waveform)
    {
        cbox_waveform_unref(ls.group->waveform);
        ls.group->waveform = NULL;
    }
    
    for(GSList *p = ls.layers; p; p = g_slist_next(p))
    {
        struct sampler_layer *l = p->data;
        l->parent_program = prg;
    }
    prg->layers = g_slist_reverse(ls.layers);
    return TRUE;
}

