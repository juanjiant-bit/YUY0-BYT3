#pragma once
// audio_engine.h — Bytebeat Machine V1.14
// Engine de audio con separación control-rate/audio-rate, swing real y
// spread estéreo optimizado por segmentos.
#include "audio_output.h"
#include "drums/drum_engine.h"
#include "../synth/lead_osc.h"
#include "../synth/quantizer.h"
#include "../synth/note_mode.h"
#include "../dsp/dsp_chain.h"
#include "dsp/ar_envelope.h"
#include "../utils/ring_buffer.h"
#include "../sequencer/event_types.h"
#include "../io/input_router.h"   // para PAD_MUTE, DrumParam
#include <cstdint>

class StateManager;

class AudioEngine {
public:
    static constexpr uint32_t SAMPLE_RATE = 44100;
    static constexpr uint16_t BLOCK_SIZE  = 32;

    void init(AudioOutput* output, StateManager* state);
    void run();
    void set_event_queue(RingBuffer<SequencerEvent, 128>* q);

private:
    static bool timer_callback(repeating_timer_t* rt);
    void        generate_samples();
    void        process_one_sample();
    void        drain_events();
    void        refresh_control_block();
    void        begin_spread_segment(const EvalContext& ctx, uint32_t base_t, float pitch_ratio);
    void        update_macro_motion(int32_t bb_i, float macro);
    static inline float smoothstep(float edge0, float edge1, float x) {
        x = clamp01((x - edge0) / (edge1 - edge0));
        return x * x * (3.0f - 2.0f * x);
    }

    static inline int16_t lerp_i16(int16_t a, int16_t b, uint8_t frac_q8) {
        int32_t ai = a;
        int32_t bi = b;
        return (int16_t)(ai + (((bi - ai) * frac_q8) >> 8));
    }

    static inline float clamp01(float x) {
        return (x < 0.0f) ? 0.0f : ((x > 1.0f) ? 1.0f : x);
    }

    AudioOutput*                    output_      = nullptr;
    StateManager*                   state_mgr_   = nullptr;
    RingBuffer<SequencerEvent, 128>* event_queue_ = nullptr;
    DspChain                        dsp_;
    DrumEngine                      drums_;
    LeadOsc                         lead_osc_;
    uint8_t                         last_lead_note_  = 69;
    uint32_t                        note_update_ctr_ = 0;
    repeating_timer_t               timer_;

    struct MacroMotionState {
        float    rhythm_raw  = 0.0f;
        float    chaos_raw   = 0.0f;
        float    density_raw = 0.0f;
        float    rhythm_s    = 0.0f;
        float    chaos_s     = 0.0f;
        float    density_s   = 0.0f;
        float    pan_s       = 0.0f;
        float    macro_s     = 0.0f;
        int32_t  prev_bb     = 0;
        // LFSR Galois 16-bit para chaos real (independiente del contenido del bytebeat)
        // Polinomio x^16+x^15+x^13+x^4+1 — período 65535
        uint16_t lfsr_state  = 0xACE1u;
    };

    struct MacroMotionOutputs {
        float pan = 0.0f;
        float drive_mod = 0.0f;
        float grain_mod = 0.0f;
        float chorus_mod = 0.0f;
        float reverb_mod = 0.0f;
    };

    MacroMotionState macro_motion_ = {};
    MacroMotionOutputs macro_out_ = {};

    float    current_bpm_      = 120.0f;
    float    stutter_depth_    = 0.0f;
    float    note_pitch_ratio_ = 1.0f;
    EvalContext cached_ctx_    = {};
    uint32_t    vibrato_phase_q24_ = 0;
    static constexpr uint32_t VIBRATO_RATE_Q24 = 2000; // ~5.3 Hz @ 44.1kHz

    // Spread runtime: evalúa el canal R a menor tasa y lo interpola por tramos
    // para evitar una segunda evaluación completa del graph en cada sample.
    uint8_t  spread_stride_      = 4;
    uint8_t  spread_phase_       = 0;
    uint8_t  spread_phase_inc_q8_= 64;
    uint16_t spread_mix_q15_     = 0;
    uint32_t spread_t_offset_    = 1;
    float    spread_macro_delta_ = 0.0f;
    int16_t  spread_seg_start_   = 0;
    int16_t  spread_seg_end_     = 0;

    bool     fx_freeze_on_   = false;
    bool     fx_oct_down_on_ = false;
    bool     fx_oct_up_on_   = false;
    bool     fx_vibrato_on_  = false;
    bool     fx_repeat4_on_  = false;
    bool     fx_repeat8_on_  = false;
    bool     fx_repeat16_on_ = false;
    float    fx_pitch_mul_current_ = 1.0f;
    float    vibrato_depth_current_ = 0.0f;

    // V1.10 — Envelope AR
    ArEnvelope  envelope_;
    bool        env_gate_     = false;  // gate actual (pad presionado o seq activa)
    static constexpr uint32_t ENV_GATE_HOLD = 88; // ~2ms @ 44100Hz (V1.11)
    uint32_t    env_gate_hold_ctr_ = 0;

    uint32_t accumulator_  = 0;
    static constexpr uint32_t ACCUM_ADD = 441;
    static constexpr uint32_t ACCUM_TOP = 10000;


    // Cache de control-rate para evitar recalcular setters del DSP sin cambios
    float base_drive_         = 0.0f;
    float base_reverb_room_   = 0.0f;
    float base_reverb_wet_    = 0.0f;
    float base_chorus_        = 0.0f;
    float base_grain_         = 0.0f;
    float cached_drive_       = -1.0f;
    float cached_reverb_room_ = -1.0f;
    float cached_reverb_wet_  = -1.0f;
    float cached_chorus_      = -1.0f;
    float cached_hp_          = -1.0f;
    float cached_grain_       = -1.0f;
    float cached_snap_        = -1.0f;
    float cached_stutter_     = -1.0f;
    float cached_attack_      = -1.0f;
    float cached_release_     = -1.0f;
    float cached_env_loop_t_  = -1.0f;
    float cached_drum_color_  = -1.0f;
    float cached_drum_decay_  = -1.0f;
    float cached_duck_        = -1.0f;

    // Aftertouch: modulaciones momentáneas superpuestas al valor base.
    // Se suman al base_ correspondiente en refresh_control_block().
    // Se resetean a 0.0 cuando la presión baja del DEADZONE del input_router.
    float at_reverb_wet_      = 0.0f;  // SNAP pads → reverb wet adicional
    float at_grain_wet_       = 0.0f;  // MUTE pad  → grain freeze wet adicional
    float cached_bpm_for_snap_= -1.0f;

    int32_t quant_mix_q15_    = 0;

    volatile uint32_t sample_tick_ = 0;
};
