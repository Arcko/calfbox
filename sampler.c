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
#include "module.h"
#include <glib.h>
#include <math.h>
#include <memory.h>
#include <sndfile.h>
#include <stdio.h>
#include <stdlib.h>

static void sampler_process_block(struct cbox_module *module, cbox_sample_t **inputs, cbox_sample_t **outputs);
static void sampler_process_event(struct cbox_module *module, const uint8_t *data, uint32_t len);
static void sampler_destroy(struct cbox_module *module);

#define MAX_SAMPLER_VOICES 128

enum sample_player_type
{
    spt_inactive,
    spt_mono16,
    spt_stereo16
};

struct sampler_layer
{
    enum sample_player_type mode;
    int16_t *sample_data;
    uint32_t sample_offset;
    uint32_t loop_start;
    uint32_t loop_end;
    float gain;
    float pan;
    float freq;
    int min_note, max_note;
    int min_vel, max_vel;
};

struct sampler_program
{
    int prog_no;
    struct sampler_layer **layers;
    int layer_count;
};

struct sampler_channel
{
    float pitchbend;
    float pbrange;
    int sustain, sostenuto;
    int volume, pan, expression, modulation;
    struct sampler_program *program;
};

struct sampler_voice
{
    enum sample_player_type mode;
    int16_t *sample_data;
    uint32_t pos, delta, loop_start, loop_end;
    uint32_t frac_pos, frac_delta;
    int note;
    int vel;
    int released, released_with_sustain, released_with_sostenuto, captured_sostenuto;
    float freq;
    float gain;
    float pan;
    float lgain, rgain;
    struct sampler_channel *channel;
};

struct sampler_module
{
    struct cbox_module module;

    int srate;
    struct sampler_voice voices[MAX_SAMPLER_VOICES];
    struct sampler_channel channels[1];
    struct sampler_layer *layers;
    int layer_count;
    struct sampler_program *programs;
    int program_count;
};

static void process_voice_mono(struct sampler_voice *v, float **channels)
{
    if (v->released)
    {
        v->mode = spt_inactive;
        return;
    }
    float lgain = v->lgain;
    float rgain = v->rgain;
    for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        if (v->pos >= v->loop_end)
        {
            if (v->loop_start == (uint32_t)-1)
            {
                v->mode = spt_inactive;
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
        
        channels[0][i] += sample * lgain;
        channels[1][i] += sample * rgain;
    }
}

static void process_voice_stereo(struct sampler_voice *v, float **channels)
{
    if (v->released)
    {
        v->mode = spt_inactive;
        return;
    }
    float lgain = v->lgain;
    float rgain = v->rgain;
    for (int i = 0; i < CBOX_BLOCK_SIZE; i++)
    {
        if (v->pos >= v->loop_end)
        {
            if (v->loop_start == (uint32_t)-1)
            {
                v->mode = spt_inactive;
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
        
        channels[0][i] += lsample * lgain;
        channels[1][i] += rsample * rgain;
    }
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
            
            double freq = l->freq * pow(2.0, (note - 69) / 12.0);
            
            v->sample_data = l->sample_data;
            v->pos = l->sample_offset;
            v->frac_pos = 0;
            v->loop_start = l->loop_start;
            v->loop_end = l->loop_end;
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
            lidx = skip_inactive_layers(prg, lidx + 1, note, vel);
            if (lidx < 0)
                break;
        }
    }
}

void sampler_stop_note(struct sampler_module *m, struct sampler_channel *c, int note, int vel)
{
    struct sampler_layer *l = m->layers;
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
                v->released = 1;
        }
    }
}

void sampler_stop_sustained(struct sampler_module *m, struct sampler_channel *c)
{
    struct sampler_layer *l = m->layers;
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && v->released_with_sustain)
        {
            v->released = 1;
            v->released_with_sustain = 0;
        }
    }
}

void sampler_stop_sostenuto(struct sampler_module *m, struct sampler_channel *c)
{
    struct sampler_layer *l = m->layers;
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && v->released_with_sostenuto)
        {
            v->released = 1;
            v->released_with_sostenuto = 0;
            // XXXKF unsure what to do with sustain
        }
    }
}

void sampler_capture_sostenuto(struct sampler_module *m, struct sampler_channel *c)
{
    struct sampler_layer *l = m->layers;
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c && !v->released)
        {
            // XXXKF unsure what to do with sustain
            v->captured_sostenuto = 1;
        }
    }
}

void sampler_stop_all(struct sampler_module *m, struct sampler_channel *c)
{
    struct sampler_layer *l = m->layers;
    for (int i = 0; i < MAX_SAMPLER_VOICES; i++)
    {
        struct sampler_voice *v = &m->voices[i];
        if (v->mode != spt_inactive && v->channel == c)
        {
            v->released = 0;
            v->released_with_sustain = 0;
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
            double maxv = 127 << 7;
            double freq = v->freq * c->pitchbend;
            uint64_t freq64 = freq * 65536.0 * 65536.0 / m->srate;
            v->delta = freq64 >> 32;
            v->frac_delta = freq64 & 0xFFFFFFFF;
            float gain = v->gain * c->volume * c->expression  / (maxv * maxv);
            float pan = v->pan + (c->pan * 1.0 / maxv - 0.5) * 2;
            if (pan < -1)
                pan = -1;
            if (pan > 1)
                pan = 1;
            v->lgain = gain * (1 - pan)  / 32768.0;
            v->rgain = gain * pan / 32768.0;
            
            if (v->mode == spt_stereo16)
                process_voice_stereo(v, outputs);
            else
                process_voice_mono(v, outputs);
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
        // XXXKF only channel 1 supported for now
        if (cmd < 15 && chn)
            return;
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
    c->program = &m->programs[0];
}

struct sampler_waveform
{
    int16_t *data;
    SF_INFO info;
};

static struct sampler_waveform *load_waveform(const char *context_name, const char *filename)
{
    int i;
    int nshorts;
    
    struct sampler_waveform *waveform = malloc(sizeof(struct sampler_waveform));
    SNDFILE *sndfile = sf_open(filename, SFM_READ, &waveform->info);
    if (!sndfile)
    {
        g_error("%s: cannot open file '%s': %s", context_name, filename, sf_strerror(NULL));
        return NULL;
    }
    waveform->data = malloc(waveform->info.channels * 2 * (waveform->info.frames + 1));
    if (waveform->info.channels != 1 && waveform->info.channels != 2)
    {
        g_error("%s: cannot open file '%s': unsupported channel count %d", context_name, filename, (int)waveform->info.channels);
        return NULL;
    }
    nshorts = waveform->info.channels * (waveform->info.frames + 1);
    for (i = 0; i < nshorts; i++)
        waveform->data[i] = 0;
    sf_readf_short(sndfile, waveform->data, waveform->info.frames);
    sf_close(sndfile);
    
    return waveform;
}

void sampler_load_layer(struct sampler_layer *l, struct sampler_waveform *waveform)
{
    l->sample_data = waveform->data;
    l->sample_offset = 0;
    l->freq = waveform->info.samplerate ? waveform->info.samplerate : 44100;
    l->loop_start = -1;
    l->loop_end = waveform->info.frames;
    l->gain = 1;
    l->pan = 0.5;
    l->mode = waveform->info.channels == 2 ? spt_stereo16 : spt_mono16;
    l->min_note = 0;
    l->max_note = 127;
    l->min_vel = 0;
    l->max_vel = 127;
}

struct cbox_module *sampler_create(void *user_data, const char *cfg_section, int srate)
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
        
    char *filename = cbox_config_get_string(cfg_section, "file");
    
    m->layer_count = 1;
    m->layers = malloc(sizeof(struct sampler_layer) * m->layer_count);
    struct sampler_waveform *waveform = load_waveform(cfg_section, filename);
    if (!waveform)
        return NULL;
    sampler_load_layer(&m->layers[0], waveform);
    
    m->program_count = 1;
    m->programs = malloc(sizeof(struct sampler_program) * m->program_count);
    m->programs[0].prog_no = 0;
    m->programs[0].layer_count = 1;
    m->programs[0].layers = malloc(sizeof(struct sampler_layer *) * m->programs[0].layer_count);
    m->programs[0].layers[0] = &m->layers[0];
    
    for (i = 0; i < MAX_SAMPLER_VOICES; i++)
        m->voices[i].mode = spt_inactive;
    
    init_channel(m, &m->channels[0]);

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

