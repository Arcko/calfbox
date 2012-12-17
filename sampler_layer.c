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

#include "config-api.h"
#include "dspmath.h"
#include "errors.h"
#include "midi.h"
#include "module.h"
#include "rt.h"
#include "sampler.h"
#include "sfzloader.h"
#include <assert.h>
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <memory.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>

void sampler_layer_set_modulation1(struct sampler_layer *l, enum sampler_modsrc src, enum sampler_moddest dest, float amount, int flags)
{
    sampler_layer_set_modulation(l, src, smsrc_none, dest, amount, flags);
}

void sampler_layer_set_modulation(struct sampler_layer *l, enum sampler_modsrc src, enum sampler_modsrc src2, enum sampler_moddest dest, float amount, int flags)
{
    GSList *p = l->modulations;
    while(p)
    {
        struct sampler_modulation *sm = p->data;
        if (sm->src == src && sm->src2 == src2 && sm->dest == dest)
        {
            sm->amount = amount;
            sm->flags = flags;
            return;
        }
        p = g_slist_next(p);
    }
    struct sampler_modulation *sm = g_malloc0(sizeof(struct sampler_modulation));
    sm->src = src;
    sm->src2 = src2;
    sm->dest = dest;
    sm->amount = amount;
    sm->flags = flags;
    l->modulations = g_slist_prepend(l->modulations, sm);
}

void sampler_layer_add_nif(struct sampler_layer *l, SamplerNoteInitFunc notefunc, int variant, float param)
{
    struct sampler_noteinitfunc *nif = malloc(sizeof(struct sampler_noteinitfunc));
    nif->notefunc = notefunc;
    nif->variant = variant;
    nif->param = param;
    l->nifs = g_slist_prepend(l->nifs, nif);
}

static void lfo_params_clear(struct sampler_lfo_params *lfop)
{
    lfop->freq = 0.f;
    lfop->delay = 0.f;
    lfop->fade = 0.f;
}

#define PROC_FIELDS_INITIALISER(type, name, def_value) \
    l->name = def_value; \
    l->has_##name = 0;
#define PROC_FIELDS_INITIALISER_dBamp(type, name, def_value) \
    l->name = def_value; \
    l->name##_linearized = -1; \
    l->has_##name = 0;
#define PROC_FIELDS_INITIALISER_dahdsr(name, parname) \
    cbox_dahdsr_init(&l->name);
#define PROC_FIELDS_INITIALISER_lfo(name, parname) \
    lfo_params_clear(&l->name##_params);

void sampler_layer_init(struct sampler_layer *l)
{
    l->waveform = NULL;
    l->sample_data = NULL;
    
    SAMPLER_FIXED_FIELDS(PROC_FIELDS_INITIALISER)
    
    l->sample_end = 0;
    l->freq = 44100;
    l->mode = spt_mono16;
    l->filter = sft_lp12;
    l->use_keyswitch = 0;
    l->last_key = 0;
    l->loop_mode = slm_unknown;
    l->velcurve[0] = 0;
    l->velcurve[127] = 1;
    for (int i = 1; i < 127; i++)
        l->velcurve[i] = -1;
    l->modulations = NULL;
    l->nifs = NULL;
    sampler_layer_set_modulation(l, 74, smsrc_none, smdest_cutoff, 9600, 2);
    sampler_layer_set_modulation(l, 71, smsrc_none, smdest_resonance, 12, 2);
    sampler_layer_set_modulation(l, 1, smsrc_pitchlfo, smdest_pitch, 100, 0);
}

#define PROC_RESET_HASFIELDS(type, name, def_value) \
    dst->has_##name = 0;
#define PROC_RESET_HASFIELDS_dBamp(type, name, def_value) \
    dst->has_##name = 0;
// XXXKF: not implemented yet - still thinking about best way to store it
#define PROC_RESET_HASFIELDS_dahdsr(name, parname) 
#define PROC_RESET_HASFIELDS_lfo(name, parname)

void sampler_layer_clone(struct sampler_layer *dst, const struct sampler_layer *src, int reset_hasfields)
{
    memcpy(dst, src, sizeof(struct sampler_layer));
    if (dst->waveform)
        cbox_waveform_ref(dst->waveform);
    dst->modulations = g_slist_copy(dst->modulations);
    for(GSList *mod = dst->modulations; mod; mod = mod->next)
    {
        gpointer dst = g_malloc(sizeof(struct sampler_modulation));
        memcpy(dst, mod->data, sizeof(struct sampler_modulation));
        mod->data = dst;
    }
    dst->nifs = g_slist_copy(dst->nifs);
    for(GSList *nif = dst->nifs; nif; nif = nif->next)
    {
        gpointer dst = g_malloc(sizeof(struct sampler_noteinitfunc));
        memcpy(dst, nif->data, sizeof(struct sampler_noteinitfunc));
        nif->data = dst;
    }
    if (reset_hasfields)
    {
        SAMPLER_FIXED_FIELDS(PROC_RESET_HASFIELDS)
    }
}

void sampler_layer_set_waveform(struct sampler_layer *l, struct cbox_waveform *waveform)
{
    l->waveform = waveform;
    l->sample_data = waveform ? waveform->data : NULL;
    l->freq = (waveform && waveform->info.samplerate) ? waveform->info.samplerate : 44100;
    l->loop_end = waveform ? waveform->info.frames : 0;
    l->sample_end = waveform ? waveform->info.frames : 0;
    l->mode = waveform && waveform->info.channels == 2 ? spt_stereo16 : spt_mono16;
}

#define PROC_FIELDS_FINALISER(type, name, def_value) 
#define PROC_FIELDS_FINALISER_dBamp(type, name, def_value) \
    l->name##_linearized = dB2gain(l->name);
#define PROC_FIELDS_FINALISER_dahdsr(name, parname) \
    cbox_envelope_init_dahdsr(&l->name##_shape, &l->name, m->module.srate / CBOX_BLOCK_SIZE);
#define PROC_FIELDS_FINALISER_lfo(name, parname) /* no finaliser required */

void sampler_layer_finalize(struct sampler_layer *l, struct sampler_module *m)
{
    SAMPLER_FIXED_FIELDS(PROC_FIELDS_FINALISER)

    if (l->loop_mode == slm_unknown)
        l->loop_mode = l->loop_start == -1 ? slm_no_loop : slm_loop_continuous;
    
    if (l->loop_mode == slm_one_shot || l->loop_mode == slm_no_loop)
        l->loop_start = -1;

    if ((l->loop_mode == slm_loop_continuous || l->loop_mode == slm_loop_sustain) && l->loop_start == -1)
    {
        l->loop_start = 0;
    }

    // if no amp_velcurve_nnn setting, default to quadratic
    if (l->velcurve_quadratic == -1)
        l->velcurve_quadratic = 1;
    
    if (l->key >= 0 && l->key <= 127)
        l->lokey = l->hikey = l->pitch_keycenter = l->key;

    // interpolate missing points in velcurve
    int start = 0;
    for (int i = 1; i < 128; i++)
    {
        if (l->velcurve[i] == -1)
            continue;
        float sv = l->velcurve[start];
        float ev = l->velcurve[i];
        if (l->velcurve_quadratic)
        {
            for (int j = start + 1; j < i; j++)
                l->velcurve[j] = sv + (ev - sv) * (j - start) * (j - start) / ((i - start) * (i - start));
        }
        else
        {
            for (int j = start + 1; j < i; j++)
                l->velcurve[j] = sv + (ev - sv) * (j - start) / (i - start);
        }
        start = i;
    }
    l->use_keyswitch = ((l->sw_down != -1) || (l->sw_up != -1) || (l->sw_last != -1) || (l->sw_previous != -1));
    l->last_key = l->sw_lokey;
}

struct layer_foreach_struct
{
    struct sampler_layer *layer;
    const char *cfg_section;
};

static void layer_foreach_func(void *user_data, const char *key)
{
    // file loading is handled in load_program
    if (!strcmp(key, "file"))
        return;
    // import is handled in sampler_load_layer_overrides
    if (!strcmp(key, "import"))
        return;
    struct layer_foreach_struct *lfs = user_data;
    const char *value = cbox_config_get_string(lfs->cfg_section, key);
    if (!sampler_layer_apply_param(lfs->layer, key, value))
        g_warning("Unknown sample layer parameter: %s in section %s", key, lfs->cfg_section);
}

void sampler_layer_load_overrides(struct sampler_layer *l, const char *cfg_section)
{
    char *imp = cbox_config_get_string(cfg_section, "import");
    if (imp)
        sampler_layer_load_overrides(l, imp);
    
    struct layer_foreach_struct lfs = {
        .layer = l,
        .cfg_section = cfg_section
    };
    cbox_config_foreach_key(layer_foreach_func, cfg_section, &lfs);
}

void sampler_layer_load(struct sampler_layer *l, struct sampler_module *m, const char *cfg_section, struct cbox_waveform *waveform)
{
    sampler_layer_init(l);
    sampler_layer_set_waveform(l, waveform);
    sampler_layer_load_overrides(l, cfg_section);
    sampler_layer_finalize(l, m);
}

static int sfz_note_from_string(const char *note)
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
    
    for (pos = 1; tolower(note[pos]) == 'b' || note[pos] == '#'; pos++)
        nv += (note[pos] != '#') ? -1 : +1;
    
    if ((note[pos] == '-' && note[pos + 1] == '1' && note[pos + 2] == '\0') || (note[pos] >= '0' && note[pos] <= '9' && note[pos + 1] == '\0'))
    {
        return nv + 12 * (1 + atoi(note + pos));
    }
    
    return -1;
}

static gboolean parse_envelope_param(struct sampler_layer *layer, int env_type, const char *key, const char *value)
{
    struct cbox_dahdsr *env = NULL;
    enum sampler_modsrc src;
    enum sampler_moddest dest;
    switch(env_type)
    {
        case 0:
            env = &layer->amp_env;
            src = smsrc_ampenv;
            dest = smdest_gain;
            break;
        case 1:
            env = &layer->filter_env;
            src = smsrc_filenv;
            dest = smdest_cutoff;
            break;
        case 2:
            env = &layer->pitch_env;
            src = smsrc_pitchenv;
            dest = smdest_pitch;
            break;
    }
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
    else if (!strcmp(key, "depth"))
        sampler_layer_set_modulation1(layer, src, dest, atof(value), 0);
    else if (!strcmp(key, "vel2delay"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 0, fvalue);
    else if (!strcmp(key, "vel2attack"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 1, fvalue);
    else if (!strcmp(key, "vel2hold"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 2, fvalue);
    else if (!strcmp(key, "vel2decay"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 3, fvalue);
    else if (!strcmp(key, "vel2sustain"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 4, fvalue);
    else if (!strcmp(key, "vel2release"))
        sampler_layer_add_nif(layer, sampler_nif_vel2env, (env_type << 4) + 5, fvalue);
    else if (!strcmp(key, "vel2depth"))
        sampler_layer_set_modulation(layer, smsrc_vel, src, dest, atof(value), 0);
    else
        return FALSE;
    return TRUE;
}

static gboolean parse_lfo_param(struct sampler_layer *layer, int lfo_type, const char *key, const char *value)
{
    struct sampler_lfo_params *params = NULL;
    enum sampler_modsrc src;
    enum sampler_moddest dest;
    switch(lfo_type)
    {
        case 0:
            params = &layer->amp_lfo_params;
            src = smsrc_amplfo;
            dest = smdest_gain;
            break;
        case 1:
            params = &layer->filter_lfo_params;
            src = smsrc_fillfo;
            dest = smdest_cutoff;
            break;
        case 2:
            params = &layer->pitch_lfo_params;
            src = smsrc_pitchlfo;
            dest = smdest_pitch;
            break;
    }
    float fvalue = atof(value);
    if (!strcmp(key, "depth"))
        sampler_layer_set_modulation1(layer, src, dest, fvalue, 0);
    else if (!strcmp(key, "depthchanaft"))
        sampler_layer_set_modulation(layer, src, smsrc_chanaft, dest, fvalue, 0);
    else if (!strcmp(key, "depthpolyaft"))
        sampler_layer_set_modulation(layer, src, smsrc_polyaft, dest, fvalue, 0);
    else if (!strcmp(key, "freq"))
        params->freq = atof(value);
    else if (!strcmp(key, "delay"))
        params->delay = atof(value);
    else if (!strcmp(key, "fade"))
        params->fade = atof(value);
    else if (!strncmp(key, "depthcc", 7))
    {
        int cc = atoi(key + 7);
        if (cc > 0 && cc < 120)
            sampler_layer_set_modulation(layer, src, cc, dest, fvalue, 0);
        else
            return FALSE;
    }
    else 
        return FALSE;
    return TRUE;
}

#define PARSE_PARAM_midi_note_t(field, strname, valuestr) \
    return ((l->field = sfz_note_from_string(value)), (l->has_##field = 1));
#define PARSE_PARAM_int(field, strname, valuestr) \
    return ((l->field = atoi(value)), (l->has_##field = 1));
#define PARSE_PARAM_uint32_t(field, strname, valuestr) \
    return ((l->field = (uint32_t)strtoul(value, NULL, 10)), (l->has_##field = 1));
#define PARSE_PARAM_float(field, strname, valuestr) \
    return ((l->field = atof(value)), (l->has_##field = 1));

#define PROC_APPLY_PARAM(type, name, def_value) \
    if (!strcmp(key, #name)) { \
        PARSE_PARAM_##type(name, #name, value) \
    }
#define PROC_APPLY_PARAM_dBamp(type, name, def_value) \
    if (!strcmp(key, #name)) { \
        return ((l->name = atof(value)), (l->has_##name = 1)); \
    }
// LFO and envelope need special handling now
// XXXKF: Neither support the 'has' bit flags - mostly because the format
// is not finalised yet.
#define PROC_APPLY_PARAM_dahdsr(name, parname) 
#define PROC_APPLY_PARAM_lfo(name, parname) 

gboolean sampler_layer_apply_param(struct sampler_layer *l, const char *key, const char *value)
{
    // XXXKF: this is seriously stupid code, this should use a hash table for
    // fixed strings, or something else that doesn't explode O(N**2) with
    // number of attributes. But premature optimization is a root of all evil.

    SAMPLER_FIXED_FIELDS(PROC_APPLY_PARAM)

    // XXXKF: to make things worse, some attributes have names different from
    // C field names, or have multiple names, or don't map 1:1 to internal model
    
    if (!strcmp(key, "lolev"))
        l->lovel = atoi(value), l->has_lovel = 1;
    else if (!strcmp(key, "hilev"))
        l->hivel = atoi(value), l->has_hivel = 1;
    else if (!strcmp(key, "offset"))
        l->sample_offset = atoi(value), l->has_sample_offset = 1;
    else if (!strcmp(key, "offset_random"))
        l->sample_offset_random = atoi(value), l->has_sample_offset_random = 1;
    else if (!strcmp(key, "loopstart"))
        l->loop_start = atoi(value), l->has_loop_start = 1;
    else if (!strcmp(key, "loopend"))
        l->loop_end = atoi(value), l->has_loop_end = 1;
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
    else if (!strcmp(key, "cutoff_chanaft"))
        sampler_layer_set_modulation1(l, smsrc_chanaft, smdest_cutoff, atof(value), 0);
    else if (!strcmp(key, "amp_random"))
        sampler_layer_add_nif(l, sampler_nif_addrandom, 0, atof(value));
    else if (!strcmp(key, "fil_random"))
        sampler_layer_add_nif(l, sampler_nif_addrandom, 1, atof(value));
    else if (!strcmp(key, "pitch_random"))
        sampler_layer_add_nif(l, sampler_nif_addrandom, 2, atof(value));
    else if (!strcmp(key, "pitch_veltrack"))
        sampler_layer_add_nif(l, sampler_nif_vel2pitch, 0, atof(value));
    else if (!strcmp(key, "effect1"))
        return (l->send1gain = atof(value) / 100.0, l->has_send1gain = 1);
    else if (!strcmp(key, "effect2"))
        return (l->send2gain = atof(value) / 100.0, l->has_send2gain = 1);
    else if (!strcmp(key, "effect1bus"))
        return (l->send1bus = atoi(value), l->has_send1bus = 1);
    else if (!strcmp(key, "effect2bus"))
        return (l->send2bus = atoi(value), l->has_send2bus = 1);
    else if (!strncmp(key, "amplfo_", 7))
        return parse_lfo_param(l, 0, key + 7, value);
    else if (!strncmp(key, "fillfo_", 7))
        return parse_lfo_param(l, 1, key + 7, value);
    else if (!strncmp(key, "pitchlfo_", 9))
        return parse_lfo_param(l, 2, key + 9, value);
    else if (!strcmp(key, "fil_type"))
    {
        enum sampler_filter_type ft = sampler_filter_type_from_string(value);
        if (ft == sft_unknown)
            g_warning("Unhandled filter type: %s", value);
        else
            l->filter = ft;
    }
    else if (!strncmp(key, "ampeg_", 6))
        return parse_envelope_param(l, 0, key + 6, value);
    else if (!strncmp(key, "fileg_", 6))
        return parse_envelope_param(l, 1, key + 6, value);
    else if (!strncmp(key, "pitcheg_", 8))
        return parse_envelope_param(l, 2, key + 8, value);
    else if (!strncmp(key, "delay_cc", 8))
    {
        int ccno = atoi(key + 8);
        if (ccno > 0 && ccno < 120)
            sampler_layer_add_nif(l, sampler_nif_cc2delay, ccno, atof(value));
        else
            return FALSE;
    }
    else if (!strncmp(key, "cutoff_cc", 9))
    {
        int ccno = atoi(key + 9);
        if (ccno > 0 && ccno < 120)
            sampler_layer_set_modulation1(l, ccno, smdest_cutoff, atof(value), 0);
        else
            return FALSE;
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
            return FALSE;
    }
    else
        return FALSE;
    
    return TRUE;
}

#define TYPE_PRINTF_uint32_t(name, def_value) \
    if (l->has_##name) \
        fprintf(f, " %s=%u", #name, (unsigned)(l->name));
#define TYPE_PRINTF_int(name, def_value) \
    if (l->has_##name) \
        fprintf(f, " %s=%d", #name, (int)(l->name));
#define TYPE_PRINTF_midi_note_t(name, def_value) \
    if (l->has_##name) { \
        int val = l->name; \
        if (val == -1) \
            fprintf(f, " %s=-1", #name); \
        else \
            fprintf(f, " %s=%c%s%d", #name, "ccddeffggaab"[val%12], "\000#\000#\000\000#\000#\000#\000#\000"+(val%12), (val/12-1)); \
    } else {}
#define TYPE_PRINTF_float(name, def_value) \
    if (l->has_##name) \
        fprintf(f, " %s=%g", #name, (float)(l->name));

#define PROC_FIELDS_TO_FILEPTR(type, name, def_value) \
    TYPE_PRINTF_##type(name, def_value)
#define PROC_FIELDS_TO_FILEPTR_dBamp(type, name, def_value) \
    if (l->has_##name) \
        fprintf(f, " %s=%g", #name, (float)(l->name));

#define ENV_PARAM_OUTPUT(env, envfield, envname, param, def_value) \
    if (l->has_##envfield##_##param) \
        fprintf(f, " " #envname "_" #param "=%g", env.param);
#define PROC_FIELDS_TO_FILEPTR_dahdsr(name, parname) \
    ENV_PARAM_OUTPUT(l->name, name, parname, start, 0) \
    ENV_PARAM_OUTPUT(l->name, name, parname, delay, 0) \
    ENV_PARAM_OUTPUT(l->name, name, parname, hold, 0) \
    ENV_PARAM_OUTPUT(l->name, name, parname, decay, 0) \
    ENV_PARAM_OUTPUT(l->name, name, parname, sustain, 1) \
    ENV_PARAM_OUTPUT(l->name, name, parname, release, 0.05) \

#define PROC_FIELDS_TO_FILEPTR_lfo(name, parname) \
    ENV_PARAM_OUTPUT(l->name##_params, name, parname, freq, 0) \
    ENV_PARAM_OUTPUT(l->name##_params, name, parname, delay, 0) \
    ENV_PARAM_OUTPUT(l->name##_params, name, parname, fade, 0) \

void sampler_layer_dump(struct sampler_layer *l, FILE *f)
{
    if (l->waveform && l->waveform->display_name)
        fprintf(f, " sample=%s", l->waveform->display_name);
    SAMPLER_FIXED_FIELDS(PROC_FIELDS_TO_FILEPTR)
    fprintf(f, "\n");
}
