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

struct sfz_load_state
{
    struct sampler_module *m;
    const char *filename;
    const char *sample_path;
    int in_group;
    struct sampler_layer group;
    struct sampler_layer *region;
    GList *layers;
    GError **error;
};

static void load_sfz_end_region(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    // printf("-- copy current region to the list of layers\n");
    struct sampler_layer *l = ls->region;
    sampler_layer_finalize(l, ls->m);
    ls->layers = g_list_append(ls->layers, ls->region);
    ls->region = NULL;
}

static void load_sfz_group(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    if (ls->region)
        load_sfz_end_region(client);
    // printf("-- start group\n");
    sampler_layer_init(&ls->group);
    ls->in_group = 1;
}

static void load_sfz_region(struct sfz_parser_client *client)
{
    struct sfz_load_state *ls = client->user_data;
    
    if (ls->region)
    {
        load_sfz_end_region(client);
    }
    ls->region = malloc(sizeof(struct sampler_layer));
    if (ls->in_group)
    {
        memcpy(ls->region, &ls->group, sizeof(struct sampler_layer));
    }
    else
        sampler_layer_init(ls->region);
    // g_warning("-- start region");
}

static gboolean parse_envelope_param(struct cbox_dahdsr *env, const char *key, const char *value)
{
    float fvalue = atof(value);
    if (!strcmp(key, "start"))
        env->start = fvalue;
    else if (!strcmp(key, "delay"))
        env->delay = fvalue;
    else if (!strcmp(key, "attack"))
        env->attack = fvalue;
    else if (!strcmp(key, "hold"))
        env->hold = fvalue;
    else if (!strcmp(key, "decay"))
        env->decay = fvalue;
    else if (!strcmp(key, "sustain"))
        env->sustain = fvalue / 100.0;
    else if (!strcmp(key, "release"))
        env->release = fvalue;
    else
        return FALSE;
    return TRUE;
}

static gboolean load_sfz_key_value(struct sfz_parser_client *client, const char *key, const char *value)
{
    struct sfz_load_state *ls = client->user_data;
    struct sampler_layer *l = ls->region ? ls->region : &ls->group;
    int unhandled = 0;
    if (!ls->region && !ls->in_group)
    {
        g_warning("Cannot use parameter '%s' outside of region or group", key);
        return TRUE;
    }
    
    if (!strcmp(key, "sample"))
    {
        gchar *filename = g_build_filename(ls->sample_path ? ls->sample_path : "", value, NULL);
        struct sampler_waveform *wf = sampler_waveform_new_from_file(ls->filename, filename, ls->error);
        g_free(filename);
        if (!wf)
            return FALSE;
        sampler_layer_set_waveform(l, wf);
        if (l->loop_end == 0)
            l->loop_end = l->sample_end;
    }
    else if (!strcmp(key, "lokey"))
        l->min_note = note_from_string(value);
    else if (!strcmp(key, "hikey"))
        l->max_note = note_from_string(value);
    else if (!strcmp(key, "pitch_keycenter"))
        l->root_note = note_from_string(value);
    else if (!strcmp(key, "pitch_keytrack"))
        l->note_scaling = atof(value);
    else if (!strcmp(key, "key"))
        l->min_note = l->max_note = l->root_note = note_from_string(value);
    else if (!strcmp(key, "lovel") || !strcmp(key, "lolev"))
        l->min_vel = atoi(value);
    else if (!strcmp(key, "hivel") || !strcmp(key, "hilev"))
        l->max_vel = atoi(value);
    else if (!strcmp(key, "loop_start") || !strcmp(key, "loopstart"))
        l->loop_start = atoi(value);
    else if (!strcmp(key, "loop_end") || !strcmp(key, "loopend"))
        l->loop_end = atoi(value);
    else if (!strcmp(key, "loop_evolve") || !strcmp(key, "loopevolve"))
        l->loop_evolve = atoi(value);
    else if (!strcmp(key, "loop_overlap") || !strcmp(key, "loopoverlap"))
        l->loop_overlap = atoi(value);
    else if (!strcmp(key, "loop_mode") || !strcmp(key, "loopmode"))
    {
        if (!strcmp(value, "one_shot"))
            l->loop_mode = slm_one_shot;
        else if (!strcmp(value, "no_loop"))
            l->loop_mode = slm_no_loop;
        else if (!strcmp(value, "loop_continuous"))
            l->loop_mode = slm_loop_continuous;
        else if (!strcmp(value, "loop_sustain"))
            l->loop_mode = slm_loop_sustain;
        else
        {
            g_warning("Unhandled loop mode: %s", value);
        }
    }
    else if (!strcmp(key, "volume"))
        l->gain = dB2gain(atof(value));
    else if (!strcmp(key, "pan"))
        l->pan = atof(value) / 100.0;
    else if (!strcmp(key, "cutoff"))
        l->cutoff = atof(value);
    else if (!strcmp(key, "resonance"))
        l->resonance = dB2gain(atof(value));
    else if (!strcmp(key, "fileg_depth"))
        l->fileg_depth = atof(value);
    else if (!strcmp(key, "pitcheg_depth"))
        l->pitcheg_depth = atof(value);
    else if (!strcmp(key, "tune"))
        l->tune = atof(value);
    else if (!strcmp(key, "transpose"))
        l->transpose = atoi(value);
    else if (!strcmp(key, "group"))
        l->exclusive_group = atoi(value);
    else if (!strcmp(key, "off_by"))
        l->off_by = atoi(value);
    else if (!strcmp(key, "velcurve_quadratic"))
        l->velcurve_quadratic = atoi(value);
    else if (!strcmp(key, "fil_veltrack"))
        l->fil_veltrack = atof(value);
    else if (!strncmp(key, "ampeg_", 6))
    {
        if (!parse_envelope_param(&l->amp_env, key + 6, value))
            unhandled = 1;
    }
    else if (!strncmp(key, "fileg_", 6))
    {
        if (!parse_envelope_param(&l->filter_env, key + 6, value))
            unhandled = 1;
    }
    else if (!strncmp(key, "pitcheg_", 7))
    {
        if (!parse_envelope_param(&l->pitch_env, key + 8, value))
            unhandled = 1;
    }
    else if (!strncmp(key, "amp_velcurve_", 13))
    {
        // if not known yet, set to 0, it can always be overriden via velcurve_quadratic setting
        if (l->velcurve_quadratic == -1)
            l->velcurve_quadratic = 0;
        int point = atoi(key + 13);
        if (point >= 0 && point <= 127)
        {
            l->velcurve[point] = atof(value);
            if (l->velcurve[point] < 0)
                l->velcurve[point] = 0;
            if (l->velcurve[point] > 1)
                l->velcurve[point] = 1;
        }
        else
            unhandled = 1;
    }
    else
        unhandled = 1;
    
    if (unhandled)
        g_warning("Unhandled sfz key: %s", key);
    return TRUE;
}

gboolean sampler_module_load_program_sfz(struct sampler_module *m, struct sampler_program *prg, const char *sfz, const char *sample_path, GError **error)
{
    struct sfz_load_state ls = { .in_group = 0, .m = m, .filename = sfz, .layers = NULL, .region = NULL, .error = error, .sample_path = sample_path };
    struct sfz_parser_client c = { .user_data = &ls, .region = load_sfz_region, .group = load_sfz_group, .key_value = load_sfz_key_value };
    g_clear_error(error);

    if (!load_sfz(sfz, &c, error))
    {
        return FALSE;
    }
    if (ls.region)
        load_sfz_end_region(&c);
    
    prg->layer_count = g_list_length(ls.layers);
    prg->layers = malloc(prg->layer_count * sizeof(struct sampler_layer *));
    GList *p = ls.layers;
    for(int i = 0; p; i++)
    {
        prg->layers[i] = p->data;
        p = g_list_next(p);
    }
    g_list_free(ls.layers);
    return TRUE;
}

