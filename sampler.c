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
#include "midi.h"
#include "module.h"
#include "sampler.h"
#include "sfzloader.h"
#include <errno.h>
#include <glib.h>
#include <math.h>
#include <memory.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>

GQuark cbox_sampler_error_quark()
{
    return g_quark_from_string("cbox-sampler-error-quark");
}

static void sampler_process_block(struct cbox_module *module, cbox_sample_t **inputs, cbox_sample_t **outputs);
static void sampler_process_event(struct cbox_module *module, const uint8_t *data, uint32_t len);
static void sampler_destroy(struct cbox_module *module);

static void process_voice_mono(struct sampler_voice *v, float **channels)
{
    float lgain = v->last_lgain;
    float rgain = v->last_rgain;
    float lgain_delta = (v->lgain - v->last_lgain) / CBOX_BLOCK_SIZE;
    float rgain_delta = (v->rgain - v->last_rgain) / CBOX_BLOCK_SIZE;
    
    float temp[2][CBOX_BLOCK_SIZE];
    for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        if (v->pos >= v->loop_end)
        {
            if (v->loop_start == (uint32_t)-1)
            {
                v->mode = spt_inactive;
                for (; i < CBOX_BLOCK_SIZE; i++)
                    temp[0][i] = temp[1][i] = 0.f;
                break;
            }
            v->pos = v->pos - v->loop_end + v->loop_start;
        }
        
        float fr = (v->frac_pos >> 8) * (1.0 / (256.0 * 65536.0));
        uint32_t nextsample = v->pos + 1;
        if (nextsample >= v->loop_end && v->loop_start != (uint32_t)-1)
            nextsample -= v->loop_start;
        
        float sample = fr * v->sample_data[nextsample] + (1 - fr) * v->sample_data[v->pos];
        
        if (v->frac_pos > ~v->frac_delta)
            v->pos++;
        v->frac_pos += v->frac_delta;
        v->pos += v->delta;
        
        temp[0][i] = sample * lgain;
        temp[1][i] = sample * rgain;
        lgain += lgain_delta;
        rgain += rgain_delta;
    }
    cbox_biquadf_process_adding(&v->filter_left, &v->filter_coeffs, temp[0], channels[0]);
    cbox_biquadf_process_adding(&v->filter_right, &v->filter_coeffs, temp[1], channels[1]);
}

static void process_voice_stereo(struct sampler_voice *v, float **channels)
{
    float lgain = v->last_lgain;
    float rgain = v->last_rgain;
    float lgain_delta = (v->lgain - v->last_lgain) / CBOX_BLOCK_SIZE;
    float rgain_delta = (v->rgain - v->last_rgain) / CBOX_BLOCK_SIZE;

    float temp[2][CBOX_BLOCK_SIZE];
    for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        if (v->pos >= v->loop_end)
        {
            if (v->loop_start == (uint32_t)-1)
            {
                v->mode = spt_inactive;
                for (; i < CBOX_BLOCK_SIZE; i++)
                    temp[0][i] = temp[1][i] = 0.f;
                break;
            }
            v->pos = v->pos - v->loop_end + v->loop_start;
        }
        
        float fr = (v->frac_pos >> 8) * (1.0 / (256.0 * 65536.0));
        uint32_t nextsample = v->pos + 1;
        if (nextsample >= v->loop_end && v->loop_start != (uint32_t)-1)
            nextsample -= v->loop_start;
        
        float lsample = fr * v->sample_data[nextsample << 1] + (1 - fr) * v->sample_data[v->pos << 1];
        float rsample = fr * v->sample_data[1 + (nextsample << 1)] + (1 - fr) * v->sample_data[1 + (v->pos << 1)];
        
        if (v->frac_pos > ~v->frac_delta)
            v->pos++;
        v->frac_pos += v->frac_delta;
        v->pos += v->delta;
        
        temp[0][i] = lsample * lgain;
        temp[1][i] = rsample * rgain;
        lgain += lgain_delta;
        rgain += rgain_delta;
    }
    cbox_biquadf_process_adding(&v->filter_left, &v->filter_coeffs, temp[0], channels[0]);
    cbox_biquadf_process_adding(&v->filter_right, &v->filter_coeffs, temp[1], channels[1]);
}

int skip_inactive_layers(struct sampler_program *prg, int first, int note, int vel)
{
    while(first < prg->layer_count)
    {
        struct sampler_layer *l = prg->layers[first];
        if (note >= l->min_note && note <= l->max_note && vel >= l->min_vel && vel <= l->max_vel)
        {
            return first;
        }
        first++;
    }
    return -1;
}

void sampler_start_note(struct sampler_module *m, struct sampler_channel *c, int note, int vel)
{
    struct sampler_program *prg = c->program;
    if (!prg)
        return;
    struct sampler_layer **pl = prg->layers;
    int lidx = skip_inactive_layers(prg, 0, note, vel);
    if (lidx < 0)
        return;
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        if (m->voices[i].mode == spt_inactive)
        {
            struct sampler_voice *v = &m->voices[i];
            struct sampler_layer *l = pl[lidx];
            
            double freq = l->freq;
            
            if (l->note_scaling != 0 || (l->tune + 100 * l->transpose) != 0)
                freq *= pow(2.0, ((note - l->root_note) * l->note_scaling + l->tune + l->transpose * 100) / 1200.0);
            
            v->sample_data = l->sample_data;
            v->pos = l->sample_offset;
            v->frac_pos = 0;
            v->loop_start = l->loop_start;
            v->loop_end = l->loop_end;
            v->sample_end = l->sample_end;
            v->gain = l->gain * vel / 127.0;
            v->pan = l->pan;
            v->note = note;
            v->vel = vel;
            v->mode = l->mode;
            v->freq = freq;
            v->released = 0;
            v->released_with_sustain = 0;
            v->released_with_sostenuto = 0;
            v->captured_sostenuto = 0;
            v->channel = c;
            v->amp_env.shape = &l->amp_env_shape;
            v->filter_env.shape = &l->filter_env_shape;
            v->last_lgain = 0;
            v->last_rgain = 0;
            v->cutoff = l->cutoff;
            v->resonance = l->resonance;
            v->env_mod = l->env_mod;
            v->loop_mode = l->loop_mode;
            cbox_biquadf_reset(&v->filter_left);
            cbox_biquadf_reset(&v->filter_right);
            cbox_envelope_reset(&v->amp_env);
            cbox_envelope_reset(&v->filter_env);
            lidx = skip_inactive_layers(prg, lidx + 1, note, vel);
            if (lidx < 0)
                break;
        }
    }
}

void sampler_voice_release(struct sampler_voice *v)
{
    if (v->loop_mode != slm_one_shot)
        v->released = 1;
    else
        v->loop_start = -1; // should be guaranteed by layer settings anyway
    
    if (v->loop_mode == slm_loop_sustain)
    {
        v->loop_end = v->sample_end;
        v->loop_start = -1;
    }
}

void sampler_stop_note(struct sampler_module *m, struct sampler_channel *c, int note, int vel)
{
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && v->note == note)
        {
            if (v->captured_sostenuto)
                v->released_with_sostenuto = 1;
            else if (c->sustain)
                v->released_with_sustain = 1;
            else
                sampler_voice_release(v);
        }
    }
}

void sampler_stop_sustained(struct sampler_module *m, struct sampler_channel *c)
{
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && v->released_with_sustain)
        {
            sampler_voice_release(v);
            v->released_with_sustain = 0;
        }
    }
}

void sampler_stop_sostenuto(struct sampler_module *m, struct sampler_channel *c)
{
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && v->released_with_sostenuto)
        {
            sampler_voice_release(v);
            v->released_with_sostenuto = 0;
            // XXXKF unsure what to do with sustain
        }
    }
}

void sampler_capture_sostenuto(struct sampler_module *m, struct sampler_channel *c)
{
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && !v->released && v->loop_mode != slm_one_shot)
        {
            // XXXKF unsure what to do with sustain
            v->captured_sostenuto = 1;
        }
    }
}

void sampler_stop_all(struct sampler_module *m, struct sampler_channel *c)
{
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c)
        {
            sampler_voice_release(v);
            v->released_with_sustain = 0;
            v->released_with_sostenuto = 0;
            v->captured_sostenuto = 0;
        }
    }
}

void sampler_process_block(struct cbox_module *module, cbox_sample_t **inputs, cbox_sample_t **outputs)
{
    struct sampler_module *m = (struct sampler_module *)module;
    
    //float channels[2][CBOX_BLOCK_SIZE];
    
    for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
        outputs[0][i] = outputs[1][i] = 0.f;
    
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        
        if (v->mode != spt_inactive)
        {
            struct sampler_channel *c = v->channel;
            
            float amp_env = cbox_envelope_get_next(&v->amp_env, v->released);
            float filter_env = cbox_envelope_get_next(&v->filter_env, v->released);
            if (v->amp_env.cur_stage < 0)
            {
                v->mode = spt_inactive;
                continue;
            }            
            
            double maxv = 127 << 7;
            double freq = v->freq * c->pitchbend;
            uint64_t freq64 = freq * 65536.0 * 65536.0 / m->srate;
            v->delta = freq64 >> 32;
            v->frac_delta = freq64 & 0xFFFFFFFF;
            float gain = amp_env * v->gain * c->volume * c->expression  / (maxv * maxv);
            float pan = v->pan + (c->pan * 1.0 / maxv - 0.5) * 2;
            if (pan < -1)
                pan = -1;
            if (pan > 1)
                pan = 1;
            v->lgain = gain * (1 - pan)  / 32768.0;
            v->rgain = gain * pan / 32768.0;
            float cutoff = v->cutoff*pow(2.0,filter_env*v->env_mod/1200);
            if (cutoff < 20)
                cutoff = 20;
            if (cutoff > m->srate * 0.45)
                cutoff = m->srate * 0.45;
            cbox_biquadf_set_lp_rbj(&v->filter_coeffs, cutoff, v->resonance, m->srate);
            
            if (v->mode == spt_stereo16)
                process_voice_stereo(v, outputs);
            else
                process_voice_mono(v, outputs);
            
            v->last_lgain = v->lgain;
            v->last_rgain = v->rgain;
        }
    }    
}

void sampler_process_cc(struct sampler_module *m, struct sampler_channel *c, int cc, int val)
{
    int enabled = val;
    switch(cc)
    {
        case 1:
            c->modulation = val << 7;
            break;
        case 7:
            c->volume = val << 7;
            break;
        case 10:
            c->pan = val << 7;
            break;
        case 11:
            c->expression = val << 7;
            break;
        case 64:
            if (c->sustain && !enabled)
            {
                sampler_stop_sustained(m, c);
            }
            c->sustain = enabled;
            break;
        case 66:
            if (c->sostenuto && !enabled)
                sampler_stop_sostenuto(m, c);
            if (!c->sostenuto && enabled)
                sampler_capture_sostenuto(m, c);
            c->sostenuto = enabled;
            break;
        
        case 120:
        case 123:
            sampler_stop_all(m, c);
            break;
        case 121:
            sampler_process_cc(m, c, 64, 0);
            sampler_process_cc(m, c, 66, 0);
            c->volume = 100 << 7;
            c->pan = 64 << 7;
            c->expression = 127 << 7;
            c->modulation = 0;
            break;
    }
}

void sampler_program_change(struct sampler_module *m, struct sampler_channel *c, int program)
{
    // XXXKF replace with something more efficient
    for (int i = 0; i < m->program_count; i++)
    {
        // XXXKF support banks
        if (m->programs[i].prog_no == program)
        {
            c->program = &m->programs[i];
            return;
        }
    }
    g_warning("Unknown program %d", program);
    c->program = &m->programs[0];
}

void sampler_process_event(struct cbox_module *module, const uint8_t *data, uint32_t len)
{
    struct sampler_module *m = (struct sampler_module *)module;
    if (len > 0)
    {
        int cmd = data[0] >> 4;
        int chn = data[0] & 15;
        struct sampler_channel *c = &m->channels[chn];
        switch(cmd)
        {
            case 8:
                sampler_stop_note(m, c, data[1], data[2]);
                break;

            case 9:
                if (data[2] > 0)
                    sampler_start_note(m, c, data[1], data[2]);
                else
                    sampler_stop_note(m, c, data[1], data[2]);
                break;
            
            case 10:
                // polyphonic pressure not handled
                break;
            
            case 11:
                sampler_process_cc(m, c, data[1], data[2]);
                break;

            case 12:
                sampler_program_change(m, c, data[1]);
                break;

            case 13:
                // ca
                break;

            case 14:
                c->pitchbend = pow(2.0, (data[1] + 128 * data[2] - 8192) * c->pbrange / (1200.0 * 8192.0));
                break;

            }
    }
}

static void init_channel(struct sampler_module *m, struct sampler_channel *c)
{
    c->pitchbend = 1;
    c->pbrange = 200; // cents
    c->sustain = 0;
    c->sostenuto = 0;
    c->volume = 100 << 7;
    c->pan = 64 << 7;
    c->expression = 127 << 7;
    c->modulation = 0;
    c->program = m->program_count ? &m->programs[0] : NULL;
}

struct sampler_waveform *sampler_waveform_new_from_file(const char *context_name, const char *filename, GError **error)
{
    int i;
    int nshorts;
    
    if (!filename)
    {
        g_set_error(error, CBOX_SAMPLER_ERROR, CBOX_SAMPLER_ERROR_INVALID_LAYER, "%s: no filename specified", context_name);
        return NULL;
    }
    struct sampler_waveform *waveform = malloc(sizeof(struct sampler_waveform));
    SNDFILE *sndfile = sf_open(filename, SFM_READ, &waveform->info);
    if (!sndfile)
    {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno (errno), "%s: cannot open '%s'", context_name, filename);
        return NULL;
    }
    waveform->data = malloc(waveform->info.channels * 2 * (waveform->info.frames + 1));
    if (waveform->info.channels != 1 && waveform->info.channels != 2)
    {
        g_set_error(error, CBOX_SAMPLER_ERROR, CBOX_SAMPLER_ERROR_INVALID_WAVEFORM, 
            "%s: cannot open file '%s': unsupported channel count %d", context_name, filename, (int)waveform->info.channels);
        return NULL;
    }
    nshorts = waveform->info.channels * (waveform->info.frames + 1);
    for (i = 0; i < nshorts; i++)
        waveform->data[i] = 0;
    sf_readf_short(sndfile, waveform->data, waveform->info.frames);
    sf_close(sndfile);
    
    return waveform;
}

static void cbox_config_get_dahdsr(const char *cfg_section, const char *prefix, struct cbox_dahdsr *env)
{
    gchar *v;
    
    v = g_strdup_printf("%s_delay", prefix);
    env->delay = cbox_config_get_float(cfg_section, v, env->delay);
    g_free(v);
    
    v = g_strdup_printf("%s_attack", prefix);
    env->attack = cbox_config_get_float(cfg_section, v, env->attack);
    g_free(v);
    
    v = g_strdup_printf("%s_attack", prefix);
    env->hold = cbox_config_get_float(cfg_section, v, env->hold);
    g_free(v);
    
    v = g_strdup_printf("%s_decay", prefix);
    env->decay = cbox_config_get_float(cfg_section, v, env->decay);
    g_free(v);
    
    v = g_strdup_printf("%s_sustain", prefix);
    env->sustain = cbox_config_get_float(cfg_section, v, env->sustain);
    g_free(v);
    
    v = g_strdup_printf("%s_release", prefix);
    env->release = cbox_config_get_float(cfg_section, v, env->release);
    g_free(v);
}

void sampler_layer_init(struct sampler_layer *l)
{
    l->sample_data = NULL;
    l->sample_offset = 0;
    l->sample_end = 0;
    l->freq = 44100;
    l->loop_start = -1;
    l->loop_end = 0;
    l->gain = 1.0;
    l->pan = 0.5;
    l->mode = spt_mono16;
    l->root_note = 69;
    l->note_scaling = 100.0;
    l->min_note = 0;
    l->max_note = 127;
    l->min_vel = 0;
    l->max_vel = 127;
    l->cutoff = 21000;
    l->resonance = 0.707;
    l->env_mod = 0;
    l->amp_env.delay = 0;
    l->amp_env.attack = 0;
    l->amp_env.hold = 0;
    l->amp_env.decay = 0;
    l->amp_env.sustain = 1;
    l->amp_env.release = 0.05;
    l->filter_env.delay = 0;
    l->filter_env.attack = 0;
    l->filter_env.hold = 0;
    l->filter_env.decay = 0;
    l->filter_env.sustain = 1;
    l->filter_env.release = 0.05;
    l->tune = 0;
    l->transpose = 0;
    l->loop_mode = slm_unknown;
}

void sampler_layer_set_waveform(struct sampler_layer *l, struct sampler_waveform *waveform)
{
    l->sample_data = waveform ? waveform->data : NULL;
    l->freq = (waveform && waveform->info.samplerate) ? waveform->info.samplerate : 44100;
    l->loop_end = waveform ? waveform->info.frames : 0;
    l->sample_end = waveform ? waveform->info.frames : 0;
    l->mode = waveform && waveform->info.channels == 2 ? spt_stereo16 : spt_mono16;
}

void sampler_layer_finalize(struct sampler_layer *l, struct sampler_module *m)
{
    cbox_envelope_init_dahdsr(&l->amp_env_shape, &l->amp_env, m->srate / CBOX_BLOCK_SIZE);
    cbox_envelope_init_dahdsr(&l->filter_env_shape, &l->filter_env,  m->srate / CBOX_BLOCK_SIZE);

    if (l->loop_mode == slm_unknown)
        l->loop_mode = l->loop_start == -1 ? slm_no_loop : slm_loop_continuous;
    
    if (l->loop_mode == slm_one_shot || l->loop_mode == slm_no_loop)
        l->loop_start = -1;

    if ((l->loop_mode == slm_loop_continuous || l->loop_mode == slm_loop_sustain) && l->loop_start == -1)
    {
        l->loop_start = 0;
    }
}

void sampler_load_layer_overrides(struct sampler_layer *l, struct sampler_module *m, const char *cfg_section)
{
    char *imp = cbox_config_get_string(cfg_section, "import");
    if (imp)
        sampler_load_layer_overrides(l, m, imp);
    l->sample_offset = cbox_config_get_int(cfg_section, "offset", l->sample_offset);
    l->loop_start = cbox_config_get_int(cfg_section, "loop_start", l->loop_start);
    l->loop_end = cbox_config_get_int(cfg_section, "loop_end", l->loop_end);
    l->gain = cbox_config_get_gain_db(cfg_section, "gain", 0.0);
    l->pan = cbox_config_get_float(cfg_section, "pan", l->pan);
    l->note_scaling = cbox_config_get_float(cfg_section, "note_scaling", l->note_scaling);
    l->root_note = cbox_config_get_int(cfg_section, "root_note", l->root_note);
    l->min_note = cbox_config_get_note(cfg_section, "low_note", l->min_note);
    l->max_note = cbox_config_get_note(cfg_section, "high_note", l->max_note);
    l->min_vel = cbox_config_get_int(cfg_section, "low_vel", l->min_vel);
    l->max_vel = cbox_config_get_int(cfg_section, "high_vel", l->max_vel);
    l->transpose = cbox_config_get_int(cfg_section, "transpose", l->transpose);
    l->tune = cbox_config_get_float(cfg_section, "tune", l->tune);
    cbox_config_get_dahdsr(cfg_section, "amp", &l->amp_env);
    cbox_config_get_dahdsr(cfg_section, "filter", &l->filter_env);
    l->cutoff = cbox_config_get_float(cfg_section, "cutoff", l->cutoff);
    l->resonance = cbox_config_get_float(cfg_section, "resonance", l->resonance);
    l->env_mod = cbox_config_get_float(cfg_section, "env_mod", l->env_mod);
    if (cbox_config_get_int(cfg_section, "one_shot", 0))
        l->loop_mode = slm_one_shot;
    if (cbox_config_get_int(cfg_section, "loop_sustain", 0))
        l->loop_mode = slm_loop_sustain;
}

void sampler_load_layer(struct sampler_module *m, struct sampler_layer *l, const char *cfg_section, struct sampler_waveform *waveform)
{
    sampler_layer_init(l);
    sampler_layer_set_waveform(l, waveform);
    sampler_load_layer_overrides(l, m, cfg_section);
    sampler_layer_finalize(l, m);
}

static gboolean load_program(struct sampler_module *m, struct sampler_program *prg, const char *cfg_section, GError **error)
{
    int i;

    g_clear_error(error);
    prg->prog_no = cbox_config_get_int(cfg_section, "program", 0);

    char *spath = cbox_config_get_string(cfg_section, "sample_path");
    char *sfz = cbox_config_get_string(cfg_section, "sfz");
    if (sfz)
    {
        return sampler_module_load_program_sfz(m, prg, sfz, spath, error);
    }
    
    int layer_count = 0;
    for (i = 0; ; i++)
    {
        gchar *s = g_strdup_printf("layer%d", i);
        char *p = cbox_config_get_string(cfg_section, s);
        g_free(s);
        
        if (!p)
        {
            layer_count = i;
            break;
        }
    }

    prg->layer_count = layer_count ? layer_count : 1;
    prg->layers = malloc(sizeof(struct sampler_layer *) * prg->layer_count);
    for (i = 0; i < prg->layer_count; i++)
    {
        prg->layers[i] = malloc(sizeof(struct sampler_layer));
        char *where = NULL;
        if (layer_count)
        {
            gchar *s = g_strdup_printf("layer%d", i);
            where = g_strdup_printf("slayer:%s", cbox_config_get_string(cfg_section, s));
            g_free(s);
        }
        const char *sample_file = cbox_config_get_string(where ? where : cfg_section, "file");
        
        gchar *sample_pathname = g_build_filename(spath ? spath : "", sample_file, NULL);
        struct sampler_waveform *waveform = sampler_waveform_new_from_file(where ? where : cfg_section, sample_pathname, error);
        g_free(sample_pathname);
        
        if (!waveform)
            return FALSE;
        sampler_load_layer(m, prg->layers[i], where, waveform);
        if (where)
            g_free(where);
    }
    return TRUE;
}

struct cbox_module *sampler_create(void *user_data, const char *cfg_section, int srate, GError **error)
{
    int result = 0;
    int i;
    static int inited = 0;
    if (!inited)
    {
        inited = 1;
    }
    
    struct sampler_module *m = malloc(sizeof(struct sampler_module));
    cbox_module_init(&m->module, m);
    m->module.process_event = sampler_process_event;
    m->module.process_block = sampler_process_block;
    m->module.destroy = sampler_destroy;
    m->srate = srate;
    m->programs = NULL;
            
    for (i = 0; ; i++)
    {
        gchar *s = g_strdup_printf("program%d", i);
        char *p = cbox_config_get_string(cfg_section, s);
        g_free(s);
        
        if (!p)
        {
            m->program_count = i;
            break;
        }
    }
    if (!m->program_count)
    {
        g_set_error(error, CBOX_SAMPLER_ERROR, CBOX_SAMPLER_ERROR_NO_PROGRAMS, "%s: no programs defined", cfg_section);
        cbox_module_destroy(&m->module);
        return FALSE;
    }
    m->programs = malloc(sizeof(struct sampler_program) * m->program_count);
    int success = 1;
    for (i = 0; i < m->program_count; i++)
    {
        gchar *s = g_strdup_printf("program%d", i);
        char *p = g_strdup_printf("spgm:%s", cbox_config_get_string(cfg_section, s));
        g_free(s);
        
        if (!load_program(m, &m->programs[i], p, error))
        {
            success = 0;
            break;
        }
    }
    if (!success)
    {
        // XXXKF free programs/layers, first ensuring that they're fully initialised
        free(m);
        return NULL;
    }
    
    for (i = 0; i < MAX_SAMPLER_VOICES; i++)
        m->voices[i].mode = spt_inactive;
    
    for (i = 0; i < 16; i++)
        init_channel(m, &m->channels[i]);

    return &m->module;
}

void sampler_destroy(struct cbox_module *module)
{
    struct sampler_module *m = (struct sampler_module *)module;
    
}

struct cbox_module_livecontroller_metadata sampler_controllers[] = {
};

struct cbox_module_keyrange_metadata sampler_keyranges[] = {
};

DEFINE_MODULE(sampler, 0, 2)

