#pragma once
// drum_engine.h — Bytebeat Machine V1.14
// Drum engine con "barandas": cada voz mantiene su identidad
// sin importar el seed/zone/macro. Variación controlada por
// drum_color (family blend) y drum_decay (decay scale).
//
// FAMILIES:
//   Kick:  0=analog(suave), 1=digital thump(punch), 2=industrial(distorsionado)
//   Snare: 0=noise-heavy,   1=tone-heavy,            2=ringy
//   Hat:   0=closed tight,  1=open,                  2=metallic
//
// ROLL: sincronizado a BPM — divisiones fijas 1/8, 1/16, 1/32
// Sidechain: duck_depth controlable en vivo (0.2-0.8 interno)
//
#include <cstdint>
#include "../../sequencer/event_types.h"

// ── Tabla seno Q15 256 puntos (shared con LeadOsc) ───────────
// Definición en drum_engine.cpp — extern para lead_osc.h
extern const int16_t SINE_TABLE_256[256];

inline int16_t sine_q15(uint32_t phase_q24) {
    return SINE_TABLE_256[(uint8_t)(phase_q24 >> 16)];
}

// ── LFSR 16-bit Galois (noise) ────────────────────────────────
inline uint16_t lfsr_next(uint16_t& s) {
    uint16_t bit = s & 1; s >>= 1;
    if (bit) s ^= 0xB400u;
    return s;
}

// ── Envelopes ─────────────────────────────────────────────────
// Exponencial aproximado por 3 tramos lineales (sin exp())
static inline int32_t exp_env(uint32_t pos, uint32_t len) {
    if (pos >= len) return 0;
    uint32_t t = len / 3;
    if (pos < t)        return 65535 - (32768 * pos) / t;
    if (pos < 2 * t)    return 32767 - (24575 * (pos - t)) / t;
    return 8192 - (8192 * (pos - 2*t)) / t;
}

static inline int32_t lin_env(uint32_t pos, uint32_t len) {
    if (pos >= len) return 0;
    return (int32_t)(65535u - (65535u * pos) / len);
}

// HPF 1er orden simple (para hat) — sin float
struct MiniHPF {
    int32_t x_prev = 0, y_acc = 0;
    int16_t process(int16_t x) {
        // R=0.9 → fc ≈ 700Hz @ 44100 para hat
        // Aumentamos corte: R=0.7 → fc ≈ 3kHz (aprox)
        y_acc = (int32_t)x - x_prev + (y_acc * 7000) / 10000;
        x_prev = x;
        if (y_acc >  32767) return  32767;
        if (y_acc < -32768) return -32768;
        return (int16_t)y_acc;
    }
    void reset() { x_prev = 0; y_acc = 0; }
};

// ─────────────────────────────────────────────────────────────
// KICK VOICE
// ─────────────────────────────────────────────────────────────
struct KickVoice {
    // Family 0: analog — pitch suave 180→45Hz, decay 220ms
    // Family 1: digital thump — pitch agresivo 200→60Hz, decay 160ms (punch)
    // Family 2: industrial — mismo pitch, decay 280ms + soft-clip fuerte
    static constexpr uint32_t DECAY_SAMPLES[3]  = { 9702, 7056, 12348 };
    static constexpr uint32_t FREQ_START_Q8[3]  = { 180*256, 200*256, 160*256 };
    static constexpr uint32_t FREQ_END_Q8[3]    = {  45*256,  60*256,  35*256 };
    static constexpr uint32_t PITCH_ENV_SAMP    = 2205;  // 50ms siempre

    void trigger(uint8_t family, float decay_scale) {
        family_   = family > 2 ? 2 : family;
        decay_len_= (uint32_t)(DECAY_SAMPLES[family_] * (0.5f + decay_scale));
        position_ = 0;
        phase_q24_= 0;
        active    = true;
    }

    // duck_depth_q15: 0..32767 (profundidad interna ya escalada)
    // sidechain_gain_q15_: 0=ducking total, 32767=sin ducking
    int16_t process(int32_t duck_depth_q15) {
        if (!active) { sidechain_gain_q15_ = 32767; return 0; }

        uint32_t freq_q8;
        if (position_ < PITCH_ENV_SAMP) {
            uint32_t t = (position_ << 8) / PITCH_ENV_SAMP;
            freq_q8 = FREQ_START_Q8[family_] -
                      ((FREQ_START_Q8[family_] - FREQ_END_Q8[family_]) * t >> 8);
        } else {
            freq_q8 = FREQ_END_Q8[family_];
        }

        phase_q24_ += (freq_q8 * 381u) >> 8;
        if (phase_q24_ >= 0x1000000u) phase_q24_ -= 0x1000000u;

        int32_t s   = sine_q15(phase_q24_);
        int32_t env = exp_env(position_, decay_len_);
        int32_t out = (s * env) >> 16;

        // Family 2: soft clip para sonido industrial
        if (family_ == 2) {
            out = out * 3 / 2;
            if (out >  32767) out =  32767;
            if (out < -32768) out = -32768;
        }

        // Sidechain Q15: gain = 1 - env_norm * duck_depth
        int32_t env_q15 = env >> 1;  // 0..32767 aprox
        sidechain_gain_q15_ = 32767 - ((env_q15 * duck_depth_q15) >> 15);
        if (sidechain_gain_q15_ < 0) sidechain_gain_q15_ = 0;

        if (++position_ >= decay_len_) {
            active = false; sidechain_gain_q15_ = 32767;
        }
        return (int16_t)out;
    }

    int32_t  sidechain_gain_q15_ = 32767;
    bool     active         = false;

private:
    uint8_t  family_   = 0;
    uint32_t decay_len_= 9702;
    uint32_t phase_q24_= 0;
    uint32_t position_ = 0;
};

// ─────────────────────────────────────────────────────────────
// SNARE VOICE
// ─────────────────────────────────────────────────────────────
struct SnareVoice {
    // Garantías de identidad:
    // Noise ≥ 40% siempre, Tone ≥ 20% siempre
    // Family 0: noise 70% + tone 30%
    // Family 1: noise 40% + tone 60%
    // Family 2: noise 50% + tone 50% + ring decay largo
    static constexpr uint32_t DECAY_SAMPLES[3]  = { 6615, 5953, 11025 };  // 150/135/250ms
    static constexpr uint32_t RING_FREQ_Q8[3]   = { 0, 0, 200*256 };      // solo family 2

    // Noise / tone mix garantizado
    static constexpr int32_t NOISE_FRAC[3] = { 7, 4, 5 };  // /10
    static constexpr int32_t TONE_FRAC[3]  = { 3, 6, 5 };  // /10

    void trigger(uint8_t family, float decay_scale) {
        family_   = family > 2 ? 2 : family;
        decay_len_= (uint32_t)(DECAY_SAMPLES[family_] * (0.5f + decay_scale));
        position_ = 0;
        phase_q24_= 0;
        if (!noise_init_) { lfsr_ = 0xACE1u; noise_init_ = true; }
        active    = true;
    }

    int16_t process() {
        if (!active) return 0;

        int32_t env  = exp_env(position_, decay_len_);

        // Noise component
        lfsr_next(lfsr_);
        int32_t noise = ((int16_t)lfsr_) * env >> 16;

        // Tone component (200Hz para snare body)
        phase_q24_ += (200u * 256u * 381u) >> 8;
        if (phase_q24_ >= 0x1000000u) phase_q24_ -= 0x1000000u;
        int32_t tone = (sine_q15(phase_q24_) * env) >> 16;

        // Family 2: ring adicional (larga resonancia)
        int32_t ring = 0;
        if (family_ == 2) {
            ring_ph_   += (RING_FREQ_Q8[2] * 381u) >> 8;
            if (ring_ph_ >= 0x1000000u) ring_ph_ -= 0x1000000u;
            int32_t ring_env = lin_env(position_, decay_len_);
            ring = (sine_q15(ring_ph_) * ring_env) >> 17;  // -6dB
        }

        int32_t out = (noise * NOISE_FRAC[family_] + tone * TONE_FRAC[family_]) / 10 + ring;
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;

        if (++position_ >= decay_len_) active = false;
        return (int16_t)out;
    }

    bool active = false;

private:
    uint8_t  family_     = 0;
    uint32_t decay_len_  = 6615;
    uint32_t position_   = 0;
    uint16_t lfsr_       = 0xACE1u;
    bool     noise_init_ = false;
    uint32_t phase_q24_  = 0;
    uint32_t ring_ph_    = 0;
};

// ─────────────────────────────────────────────────────────────
// HAT VOICE
// ─────────────────────────────────────────────────────────────
struct HatVoice {
    // Family 0: closed tight — decay 40ms
    // Family 1: open — decay 300ms
    // Family 2: metallic — decay 80ms + coloración (lfsr con taps distintos)
    static constexpr uint32_t DECAY_SAMPLES[3] = { 1764, 13230, 3528 };  // 40/300/80ms

    void trigger(uint8_t family, float decay_scale) {
        family_   = family > 2 ? 2 : family;
        decay_len_= (uint32_t)(DECAY_SAMPLES[family_] * (0.5f + decay_scale));
        position_ = 0;
        if (family_ == 2) { lfsr_ = 0xDEADu; }
        else              { lfsr_ = 0xACE1u; }
        hpf_.reset();
        active    = true;
    }

    int16_t process() {
        if (!active) return 0;

        // Noise con HPF para carácter de hat
        uint16_t raw = lfsr_next(lfsr_);
        // Family 2: mezcla dos LFSR para metallic (más rico espectralmente)
        if (family_ == 2) {
            uint16_t raw2 = raw ^ (raw << 3);
            raw = (uint16_t)((raw + raw2) >> 1);
        }

        int32_t env  = (family_ == 1) ?
                       lin_env(position_, decay_len_) :   // open: lineal
                       exp_env(position_, decay_len_);    // closed/metallic: exp

        int32_t noise = ((int16_t)raw * env) >> 16;
        int16_t out   = hpf_.process((int16_t)noise);

        if (++position_ >= decay_len_) active = false;
        return out;
    }

    bool active = false;

private:
    uint8_t  family_    = 0;
    uint32_t decay_len_ = 1764;
    uint32_t position_  = 0;
    uint16_t lfsr_      = 0xACE1u;
    MiniHPF  hpf_;
};

// ─────────────────────────────────────────────────────────────
// DRUM ENGINE — coordinador principal
// ─────────────────────────────────────────────────────────────
class DrumEngine {
public:
    void init();

    // Triggerear hit (desde EVT_DRUM_HIT)
    void trigger(DrumId id, float vel = 1.0f);

    // Roll on/off (desde EVT_ROLL_ON/OFF)
    void roll_on(DrumId id);
    void roll_off(DrumId id);

    // Parámetros de variación (Core0, safe point)
    // drum_color: 0-1 → blend entre families  (-1 = no cambiar)
    // drum_decay: 0-1 → escala de decay        (-1 = no cambiar)
    // duck_depth: 0.2-0.8 → sidechain          (-1 = no cambiar)
    // Llamar con -1.0 en los campos que no se quieren modificar
    // (para EVT_DRUM_PARAM que cambia solo un parámetro a la vez)
    void set_params(float drum_color, float drum_decay, float duck_depth);

    // Tick de BPM (llamar cada sample desde AudioEngine)
    // bpm: BPM actual del sequencer
    void tick_bpm(float bpm);

    // Process: genera sample stereo + escribe sidechain_gain
    void process(int16_t& out_l, int16_t& out_r, int32_t& sidechain_gain_q15);

private:
    uint8_t  color_to_family(float color) const {
        // 0.0-0.33 → family 0, 0.33-0.66 → family 1, 0.66-1.0 → family 2
        if (color < 0.33f) return 0;
        if (color < 0.66f) return 1;
        return 2;
    }

    KickVoice   kick_;
    SnareVoice  snare_;
    HatVoice    hat_;

    float    drum_color_  = 0.0f;
    float    drum_decay_  = 0.5f;
    int32_t  duck_depth_q15_ = 16384;

    // Roll state
    bool     roll_active_[3]      = {};
    uint32_t roll_counter_[3]     = {};
    uint32_t roll_period_[3]      = {};   // samples entre hits (1/16 a BPM)
    static constexpr uint32_t ROLL_DIV = 16;  // 1/16 de beat

    // DC block + safety per voice output
    // (simplificado — el limiter global ya está en DspChain)
};
