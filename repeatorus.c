/*
 * Repeatorus LV2 Plugin - Stutter effect with MIDI capabilities
 * Copyright (C) 2026 Simon Delaruotte
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 */

#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/atom/forge.h>
#include <lv2/log/log.h>
#include <lv2/urid/urid.h>
#include <lv2/midi/midi.h>
#include <lv2/units/units.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define REPEATORUS_URI "urn:simdott:repeatorus"
#define MAX_STAGES 5
#define BPM_TO_MS(bpm) (60000.0f / (bpm))

typedef enum {
    REPEATORUS_INPUT_L = 0,
    REPEATORUS_INPUT_R = 1,
    REPEATORUS_OUTPUT_L = 2,
    REPEATORUS_OUTPUT_R = 3,
    MIDI_THRESHOLD_SENS = 4,      
    REPEATORUS_TEMPO_MASTER = 5,   
    REPEATORUS_BYPASS_1 = 6,
    REPEATORUS_BYPASS_2 = 7,
    REPEATORUS_BYPASS_3 = 8,
    REPEATORUS_BYPASS_4 = 9,
    REPEATORUS_BYPASS_5 = 10,
    MIDI_INPUT = 11,
    MIDI_SWITCH = 12,
    MIDI_CHANNEL = 13,
    MIDI_NOTE_1 = 14,
    MIDI_NOTE_2 = 15,
    MIDI_NOTE_3 = 16,
    MIDI_NOTE_4 = 17,
    MIDI_NOTE_5 = 18
} PortIndex;

typedef enum {
    LOOP_STATE_IDLE = 0,
    LOOP_STATE_RECORDING = 1,
    LOOP_STATE_PLAYING = 2
} LoopState;

typedef struct {
    float* buffer_l;
    float* buffer_r;
    uint32_t buffer_size;
    uint32_t write_pos;
    uint32_t read_pos;
    LoopState state;
    uint32_t loop_length;
    uint32_t samples_recorded;
    
    float last_sample_l;
    float last_sample_r;
    int needs_reset;
    float current_time_ms;
    float target_time_ms;
    float time_smooth_factor;
    float pre_roll_buffer_l[1024];
    float pre_roll_buffer_r[1024];
    uint32_t pre_roll_pos;
    int waiting_for_note;
    float last_env;
} LooperStage;

typedef struct {
    const float* input_l;
    const float* input_r;
    float* output_l;
    float* output_r;
    
    const float* bypass_1;
    const float* tempo_master;
    const float* bypass_2;
    const float* bypass_3;
    const float* bypass_4;
    const float* bypass_5;
    
    const LV2_Atom_Sequence* midi_in;
    const float* midi_switch;
    const float* midi_channel;
    const float* midi_note_1;
    const float* midi_note_2;
    const float* midi_note_3;
    const float* midi_note_4;
    const float* midi_note_5;
    const float* threshold_sens;

    LooperStage stages[MAX_STAGES];

    uint32_t sample_rate;
    
    float current_volume;
    float target_volume;
    float volume_smooth_factor;

    uint32_t latency_samples;
    uint32_t latency_counter;

    int enable_stack[MAX_STAGES];
    int stack_size;
    int previous_bypass[MAX_STAGES];
    
    int midi_bypass[MAX_STAGES];
    int control_bypass[MAX_STAGES];
    int current_bypass[MAX_STAGES];
    
    float stage_times_ms[MAX_STAGES];  // Stores milliseconds
    
    LV2_URID midi_MidiEvent;

} MIDI_Repeatorus;

static LV2_Handle instantiate(const LV2_Descriptor* descriptor, double rate, const char* path,
                              const LV2_Feature* const* features) {
    MIDI_Repeatorus* repeatorus = (MIDI_Repeatorus*)calloc(1, sizeof(MIDI_Repeatorus));
    if (!repeatorus) return NULL;
    
    repeatorus->sample_rate = (uint32_t)rate;
    uint32_t buffer_size = (uint32_t)rate;

    for (int i = 0; i < MAX_STAGES; i++) {
        repeatorus->stages[i].buffer_size = buffer_size;
        repeatorus->stages[i].buffer_l = (float*)calloc(buffer_size, sizeof(float));
        repeatorus->stages[i].buffer_r = (float*)calloc(buffer_size, sizeof(float));
        
        if (!repeatorus->stages[i].buffer_l || !repeatorus->stages[i].buffer_r) {
            for (int j = 0; j <= i; j++) {
                free(repeatorus->stages[j].buffer_l);
                free(repeatorus->stages[j].buffer_r);
            }
            free(repeatorus);
            return NULL;
        }
        
        repeatorus->stages[i].write_pos = 0;
        repeatorus->stages[i].read_pos = 0;
        repeatorus->stages[i].state = LOOP_STATE_IDLE;
        repeatorus->stages[i].samples_recorded = 0;
        repeatorus->stages[i].time_smooth_factor = 0.5f;
        repeatorus->stages[i].waiting_for_note = 0;
        repeatorus->stages[i].pre_roll_pos = 0;
        memset(repeatorus->stages[i].pre_roll_buffer_l, 0, 1024 * sizeof(float));
        memset(repeatorus->stages[i].pre_roll_buffer_r, 0, 1024 * sizeof(float));
        repeatorus->stages[i].last_sample_l = 0.0f;
        repeatorus->stages[i].last_sample_r = 0.0f;
        repeatorus->stages[i].needs_reset = 0;
        repeatorus->stages[i].last_env = 0.0f;
        repeatorus->previous_bypass[i] = 0;
        repeatorus->midi_bypass[i] = 0;
        repeatorus->control_bypass[i] = 0;
        repeatorus->current_bypass[i] = 0;
        repeatorus->stage_times_ms[i] = 250.0f;  // Default 120 BPM → 250ms
    }

    repeatorus->latency_samples = (uint32_t)(rate * 0.001f);
    repeatorus->latency_counter = 0;
    repeatorus->stack_size = 0;
    
    const LV2_URID_Map* map = NULL;
    for (int i = 0; features[i]; ++i) {
        if (!strcmp(features[i]->URI, LV2_URID__map)) {
            map = (const LV2_URID_Map*)features[i]->data;
            break;
        }
    }
    
    if (!map) {
    fprintf(stderr, "MIDI Repeatorus: ERROR - No LV2_URID__map feature\n");
    for (int i = 0; i < MAX_STAGES; i++) {
        free(repeatorus->stages[i].buffer_l);
        free(repeatorus->stages[i].buffer_r);
    }
    free(repeatorus);
    return NULL;
}
    
    repeatorus->midi_MidiEvent = map->map(map->handle, LV2_MIDI__MidiEvent);

    return (LV2_Handle)repeatorus;
}

static void connect_port(LV2_Handle instance, uint32_t port, void* data) {
    MIDI_Repeatorus* repeatorus = (MIDI_Repeatorus*)instance;
    if (!repeatorus) return;
    
    switch ((PortIndex)port) {
        case REPEATORUS_INPUT_L:
            repeatorus->input_l = (const float*)data;
            break;
        case REPEATORUS_INPUT_R:
            repeatorus->input_r = (const float*)data;
            break;
        case REPEATORUS_OUTPUT_L:
            repeatorus->output_l = (float*)data;
            break;
        case REPEATORUS_OUTPUT_R:
            repeatorus->output_r = (float*)data;
            break;
        case MIDI_THRESHOLD_SENS:
            repeatorus->threshold_sens = (const float*)data;
            break;
        case REPEATORUS_TEMPO_MASTER:  // Renamed
            repeatorus->tempo_master = (const float*)data;
            break;
        case REPEATORUS_BYPASS_1:
            repeatorus->bypass_1 = (const float*)data;
            break;
        case REPEATORUS_BYPASS_2:
            repeatorus->bypass_2 = (const float*)data;
            break;
        case REPEATORUS_BYPASS_3:
            repeatorus->bypass_3 = (const float*)data;
            break;
        case REPEATORUS_BYPASS_4:
            repeatorus->bypass_4 = (const float*)data;
            break;
        case REPEATORUS_BYPASS_5:
            repeatorus->bypass_5 = (const float*)data;
            break;
        case MIDI_INPUT:
            repeatorus->midi_in = (const LV2_Atom_Sequence*)data;
            break;
        case MIDI_SWITCH:
            repeatorus->midi_switch = (const float*)data;
            break;
        case MIDI_CHANNEL:
            repeatorus->midi_channel = (const float*)data;
            break;
        case MIDI_NOTE_1:
            repeatorus->midi_note_1 = (const float*)data;
            break;
        case MIDI_NOTE_2:
            repeatorus->midi_note_2 = (const float*)data;
            break;
        case MIDI_NOTE_3:
            repeatorus->midi_note_3 = (const float*)data;
            break;
        case MIDI_NOTE_4:
            repeatorus->midi_note_4 = (const float*)data;
            break;
        case MIDI_NOTE_5:
            repeatorus->midi_note_5 = (const float*)data;
            break;
        default:
            break;
    }
}

static void activate(LV2_Handle instance) {
    MIDI_Repeatorus* repeatorus = (MIDI_Repeatorus*)instance;
    
    for (int i = 0; i < MAX_STAGES; i++) {
        repeatorus->stages[i].write_pos = 0;
        repeatorus->stages[i].read_pos = 0;
        repeatorus->stages[i].state = LOOP_STATE_IDLE;
        repeatorus->stages[i].samples_recorded = 0;
        repeatorus->stages[i].waiting_for_note = 0;
        repeatorus->stages[i].pre_roll_pos = 0;
        memset(repeatorus->stages[i].pre_roll_buffer_l, 0, 1024 * sizeof(float));
        memset(repeatorus->stages[i].pre_roll_buffer_r, 0, 1024 * sizeof(float));
        repeatorus->stages[i].last_sample_l = 0.0f;
        repeatorus->stages[i].last_sample_r = 0.0f;
        repeatorus->stages[i].needs_reset = 0;
        repeatorus->stages[i].last_env = 0.0f;
        repeatorus->previous_bypass[i] = 0;
        repeatorus->midi_bypass[i] = 0;
        repeatorus->control_bypass[i] = 0;
        repeatorus->current_bypass[i] = 0;
    }
    repeatorus->stack_size = 0;
}

static void process_midi_events(MIDI_Repeatorus* repeatorus, const LV2_Atom_Sequence* midi_in) {
    int channel = 2;
    int notes[MAX_STAGES] = {60, 62, 64, 65, 67};
    
    if (repeatorus->midi_channel) {
        channel = (int)(*(repeatorus->midi_channel)) - 1;
        if (channel < 0) channel = 0;
        if (channel > 15) channel = 15;
    }
    
    const float* midi_notes[] = {
        repeatorus->midi_note_1, repeatorus->midi_note_2, repeatorus->midi_note_3, 
        repeatorus->midi_note_4, repeatorus->midi_note_5
    };
    
    for (int i = 0; i < MAX_STAGES; i++) {
        if (midi_notes[i]) {
            notes[i] = (int)(*(midi_notes[i]));
            if (notes[i] < 0) notes[i] = 0;
            if (notes[i] > 127) notes[i] = 127;
        }
    }
    
    LV2_ATOM_SEQUENCE_FOREACH(midi_in, ev) {
        if (ev->body.type == repeatorus->midi_MidiEvent) {
            const uint8_t* const msg = (const uint8_t*)(ev + 1);
            const uint8_t status = msg[0] & 0xF0;
            const uint8_t msg_channel = msg[0] & 0x0F;
            
            if (msg_channel == channel) {
                const uint8_t note = msg[1];
                const uint8_t velocity = msg[2];
                
                for (int i = 0; i < MAX_STAGES; i++) {
                    if (note == notes[i]) {
                        if (status == LV2_MIDI_MSG_NOTE_ON && velocity > 0) {
                            repeatorus->midi_bypass[i] = !repeatorus->midi_bypass[i];
                        } else if (status == LV2_MIDI_MSG_NOTE_OFF || 
                                  (status == LV2_MIDI_MSG_NOTE_ON && velocity == 0)) {
                            repeatorus->midi_bypass[i] = 0;
                        }
                    }
                }
            }
        }
    }
}

static void process_looper_stage(LooperStage* stage, const float* in_l, const float* in_r,
                                 float* out_l, float* out_r, uint32_t n_samples,
                                 int bypass_current, uint32_t sample_rate, 
                                 float target_time_ms, float linear_threshold) {
    
    stage->target_time_ms = target_time_ms;
    
    if (stage->state == LOOP_STATE_IDLE) {
        stage->current_time_ms += stage->time_smooth_factor * (stage->target_time_ms - stage->current_time_ms);
    }
    
    uint32_t new_loop_length = (uint32_t)(stage->current_time_ms * sample_rate / 1000.0f);
    if (new_loop_length > stage->buffer_size) new_loop_length = stage->buffer_size;
    if (new_loop_length < 1) new_loop_length = 1;
    
    if (bypass_current && stage->state == LOOP_STATE_IDLE) {
        stage->waiting_for_note = 1;
        stage->pre_roll_pos = 0;
    }
    else if (!bypass_current) {
        stage->waiting_for_note = 0;
        stage->state = LOOP_STATE_IDLE;
    }
    
    for (uint32_t i = 0; i < n_samples; i++) {
        if (stage->waiting_for_note) {
            stage->pre_roll_buffer_l[stage->pre_roll_pos] = in_l[i];
            stage->pre_roll_buffer_r[stage->pre_roll_pos] = in_r[i];
            stage->pre_roll_pos = (stage->pre_roll_pos + 1) % 1024;
            
            float env = fabsf(in_l[i]) + fabsf(in_r[i]);
            
            if (env > linear_threshold && stage->last_env <= linear_threshold) {
                stage->state = LOOP_STATE_RECORDING;
                stage->waiting_for_note = 0;
                stage->write_pos = 0;
                stage->read_pos = 0;
                stage->samples_recorded = 0;
                stage->loop_length = new_loop_length;
                
                uint32_t samples_to_copy = (stage->loop_length < 1024) ? stage->loop_length : 1024;
                uint32_t copy_start = (stage->pre_roll_pos >= samples_to_copy) ? 
                                     (stage->pre_roll_pos - samples_to_copy) : 
                                     (1024 - (samples_to_copy - stage->pre_roll_pos));
                
                for (uint32_t j = 0; j < samples_to_copy; j++) {
                    uint32_t pre_roll_idx = (copy_start + j) % 1024;
                    stage->buffer_l[j] = stage->pre_roll_buffer_l[pre_roll_idx];
                    stage->buffer_r[j] = stage->pre_roll_buffer_r[pre_roll_idx];
                }
                
                for (uint32_t j = samples_to_copy; j < stage->loop_length; j++) {
                    stage->buffer_l[j] = 0.0f;
                    stage->buffer_r[j] = 0.0f;
                }
            }
            
            stage->last_env = env;
            out_l[i] = in_l[i];
            out_r[i] = in_r[i];
        } 
        else {
            stage->last_env = 0.0f;
            
            switch (stage->state) {
                case LOOP_STATE_IDLE:
                    out_l[i] = in_l[i];
                    out_r[i] = in_r[i];
                    break;
                    
                case LOOP_STATE_RECORDING:
                    stage->buffer_l[stage->write_pos] = in_l[i];
                    stage->buffer_r[stage->write_pos] = in_r[i];
                    out_l[i] = in_l[i];
                    out_r[i] = in_r[i];
                    stage->write_pos++;
                    stage->samples_recorded++;
                    
                    if (stage->write_pos >= stage->loop_length) {
                        stage->state = LOOP_STATE_PLAYING;
                        stage->read_pos = 0;
                        stage->needs_reset = 1;
                    }
                    break;
                    
                case LOOP_STATE_PLAYING:
                    if (stage->read_pos == 0 && stage->needs_reset) {
                        float fade = 0.0f;
                        const int fade_samples = 8;
                        
                        if (i < fade_samples) {
                            fade = (float)i / (float)fade_samples;
                            out_l[i] = stage->last_sample_l * (1.0f - fade) + 
                                       stage->buffer_l[0] * fade;
                            out_r[i] = stage->last_sample_r * (1.0f - fade) + 
                                       stage->buffer_r[0] * fade;
                        } else {
                            out_l[i] = stage->buffer_l[stage->read_pos];
                            out_r[i] = stage->buffer_r[stage->read_pos];
                        }
                    } else {
                        out_l[i] = stage->buffer_l[stage->read_pos];
                        out_r[i] = stage->buffer_r[stage->read_pos];
                    }
                    
                    stage->last_sample_l = out_l[i];
                    stage->last_sample_r = out_r[i];
                    
                    stage->read_pos++;
                    if (stage->read_pos >= stage->loop_length) {
                        stage->read_pos = 0;
                        stage->needs_reset = 1;
                    } else {
                        stage->needs_reset = 0;
                    }
                    break;
            }
        }
    }
    
    if (stage->state == LOOP_STATE_RECORDING && stage->write_pos >= stage->loop_length) {
        stage->write_pos = 0;
    }
}

static void run(LV2_Handle instance, uint32_t n_samples) {
    MIDI_Repeatorus* repeatorus = (MIDI_Repeatorus*)instance;

    if (!repeatorus->tempo_master || !repeatorus->input_l || !repeatorus->input_r || 
        !repeatorus->output_l || !repeatorus->output_r) {
        return;
    }

    // Convert master tempo from BPM to milliseconds (stage 1 = whole note)
    float master_bpm = *(repeatorus->tempo_master);
    
    float master_time_ms = BPM_TO_MS(master_bpm);
    repeatorus->stage_times_ms[0] = master_time_ms;
    
    // Stage 2 = half note (T/2), Stage 3 = quarter note (T/4), etc.
    for (int i = 1; i < MAX_STAGES; i++) {
        float new_time_ms = master_time_ms / (float)(1 << i);  // Divide by powers of 2
        if (fabsf(new_time_ms - repeatorus->stage_times_ms[i]) > 1.0f) {
            repeatorus->stage_times_ms[i] = new_time_ms;
        }
    }

    const float* bypass_ports[] = {
        repeatorus->bypass_1, repeatorus->bypass_2, repeatorus->bypass_3, 
        repeatorus->bypass_4, repeatorus->bypass_5
    };
    
    for (int i = 0; i < MAX_STAGES; i++) {
        if (bypass_ports[i]) {
            repeatorus->control_bypass[i] = (*bypass_ports[i] >= 0.5f) ? 1 : 0;
        }
    }
    
    if (repeatorus->midi_in) {
        process_midi_events(repeatorus, repeatorus->midi_in);
    }
    
    float use_midi = 0.0f;
    if (repeatorus->midi_switch) {
        use_midi = *(repeatorus->midi_switch);
    }
    
    for (int i = 0; i < MAX_STAGES; i++) {
        repeatorus->current_bypass[i] = (use_midi > 0.5f) ? repeatorus->midi_bypass[i] : repeatorus->control_bypass[i];
    }

    for (int i = 0; i < MAX_STAGES; i++) {
        if (repeatorus->current_bypass[i] && !repeatorus->previous_bypass[i]) {
            for (int j = 0; j < repeatorus->stack_size; j++) {
                if (repeatorus->enable_stack[j] == i) {
                    for (int k = j; k < repeatorus->stack_size - 1; k++) {
                        repeatorus->enable_stack[k] = repeatorus->enable_stack[k + 1];
                    }
                    repeatorus->stack_size--;
                    break;
                }
            }
            repeatorus->enable_stack[repeatorus->stack_size] = i;
            repeatorus->stack_size++;
        }
        else if (!repeatorus->current_bypass[i] && repeatorus->previous_bypass[i]) {
            for (int j = 0; j < repeatorus->stack_size; j++) {
                if (repeatorus->enable_stack[j] == i) {
                    for (int k = j; k < repeatorus->stack_size - 1; k++) {
                        repeatorus->enable_stack[k] = repeatorus->enable_stack[k + 1];
                    }
                    repeatorus->stack_size--;
                    break;
                }
            }
        }
        
        repeatorus->previous_bypass[i] = repeatorus->current_bypass[i];
    }

    int active_stage = -1;
    if (repeatorus->stack_size > 0) {
        active_stage = repeatorus->enable_stack[repeatorus->stack_size - 1];
    }

    int processing_bypass[MAX_STAGES];
    for (int i = 0; i < MAX_STAGES; i++) {
        processing_bypass[i] = (i == active_stage) ? 1 : 0;
    }

    float* temp_l[MAX_STAGES + 1];
    float* temp_r[MAX_STAGES + 1];
    
    for (int i = 0; i < MAX_STAGES + 1; i++) {
        temp_l[i] = (float*)malloc(n_samples * sizeof(float));
        temp_r[i] = (float*)malloc(n_samples * sizeof(float));
        if (!temp_l[i] || !temp_r[i]) {
            for (int j = 0; j <= i; j++) {
                free(temp_l[j]);
                free(temp_r[j]);
            }
            return;
        }
    }

    if (repeatorus->latency_counter < repeatorus->latency_samples) {
        for (uint32_t i = 0; i < n_samples; i++) {
            temp_l[0][i] = repeatorus->input_l[i];
            temp_r[0][i] = repeatorus->input_r[i];
        }
        repeatorus->latency_counter++;
    } else {
        memcpy(temp_l[0], repeatorus->input_l, n_samples * sizeof(float));
        memcpy(temp_r[0], repeatorus->input_r, n_samples * sizeof(float));
    }

    for (int stage_idx = 0; stage_idx < MAX_STAGES; stage_idx++) {
    float linear_threshold = 0.001f;  // -60 dB default

if (repeatorus->threshold_sens) {
    float sens = *(repeatorus->threshold_sens);
    if (sens < 0.0f) sens = 0.0f;
    if (sens > 100.0f) sens = 100.0f;

    // Map 0-100 to -1 dB to -60 dB (reverse mapping)
    float db_val = -1.0f - (sens * 0.59f);  // 0 → -1dB, 100 → -60dB
    linear_threshold = powf(10.0f, db_val / 20.0f);  
    }

    process_looper_stage(&repeatorus->stages[stage_idx],
                        temp_l[stage_idx], temp_r[stage_idx],
                        temp_l[stage_idx + 1], temp_r[stage_idx + 1],
                        n_samples, processing_bypass[stage_idx], 
                        repeatorus->sample_rate, repeatorus->stage_times_ms[stage_idx],
                        linear_threshold);
}

    for (uint32_t i = 0; i < n_samples; i++) {
        repeatorus->output_l[i] = temp_l[MAX_STAGES][i];
        repeatorus->output_r[i] = temp_r[MAX_STAGES][i];
    }

    for (int i = 0; i < MAX_STAGES + 1; i++) {
        free(temp_l[i]);
        free(temp_r[i]);
    }
}

static void deactivate(LV2_Handle instance) {
    activate(instance);
}

static void cleanup(LV2_Handle instance) {
    MIDI_Repeatorus* repeatorus = (MIDI_Repeatorus*)instance;
    
    for (int i = 0; i < MAX_STAGES; i++) {
        free(repeatorus->stages[i].buffer_l);
        free(repeatorus->stages[i].buffer_r);
    }
    free(repeatorus);
}

static const LV2_Descriptor descriptor = {
    REPEATORUS_URI,
    instantiate,
    connect_port,
    activate,
    run,
    deactivate,
    cleanup,
    NULL
};

LV2_SYMBOL_EXPORT const LV2_Descriptor* lv2_descriptor(uint32_t index) {
    return index == 0 ? &descriptor : NULL;
}
