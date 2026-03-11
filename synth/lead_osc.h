#pragma once
// lead_osc.h — Bytebeat Machine V1.16
// Oscilador senoidal con modulación FM/AM por el bytebeat activo.
//
// MODULACIÓN (Note Mode):
//   FM: el output del bytebeat del frame anterior modula el delta de fase.
//       fm_depth=0.0 → seno puro, fm_depth=1.0 → timbre bytebeat impreso.
//   AM: el bytebeat modula la amplitud del seno (complemento sutil del FM).
//   set_mod_depth(macro): macro 0.0–1.0 controla ambas profundidades.
//     macro=0 → seno puro afinado
//     macro=1 → FM máximo, el bytebeat tiñe la afinación completamente
//
// PORTAMENTO: glide entre frecuencias por slew rate en set_freq_slew().
//
#include <cstdint>
#include "bytebeat_node.h"  // SINE_TABLE está en drum_engine.h pero
                            // para no duplicar usamos la del drum

// Tabla seno Q15 256 puntos (definida en drum_engine.h como extern o inline)
// Para evitar duplicados, declaramos extern y la definimos en un .cpp
extern const int16_t SINE_TABLE_256[256];

class LeadOsc {
public:
    void init() {
        phase_q24_   = 0;
        freq_hz_     = 220.0f;
        delta_       = freq_to_delta(220.0f);
        fm_depth_    = 0.0f;
        am_depth_    = 0.0f;
        bb_norm_     = 0.0f;
    }

    // Llamar cuando cambia la frecuencia (no necesariamente cada sample)
    void set_freq(float hz) {
        if (hz < 20.0f)   hz = 20.0f;
        if (hz > 8000.0f) hz = 8000.0f;
        freq_hz_ = hz;
        delta_   = freq_to_delta(hz);
    }

    // Portamento suave: slew hacia target_freq en ~20ms
    void set_freq_slew(float hz) {
        target_freq_ = hz;
        slew_active_ = true;
    }

    // Alimentar el output bytebeat normalizado del frame anterior (-1.0..+1.0).
    // Llamar una vez por sample ANTES de process().
    // Si mod_depth=0 esta llamada no tiene efecto en el audio.
    void feed_bytebeat(int16_t bb_sample) {
        // Normalizar a -1.0..+1.0. Suavizar con un slew para evitar
        // zipper noise cuando el bytebeat cambia bruscamente de valor.
        const float target = (float)bb_sample * (1.0f / 32767.0f);
        bb_norm_ += 0.08f * (target - bb_norm_);  // tau ~12 samples
    }

    // Profundidad de modulación FM+AM desde el parámetro Macro (0.0–1.0).
    // FM depth: 0.0–0.40 (saturado en 0.4 para evitar aliasing excesivo).
    // AM depth: 0.0–0.15 (complemento sutil, 37.5% del FM).
    // Llamar cuando cambia el macro (control-rate, no cada sample).
    void set_mod_depth(float macro) {
        const float m = macro < 0.0f ? 0.0f : (macro > 1.0f ? 1.0f : macro);
        fm_depth_ = m * 0.40f;
        am_depth_ = m * 0.15f;
    }

    // process(): llamar cada sample. Retorna int16_t.
    // Aplica FM/AM si fm_depth_ > 0.
    int16_t process() {
        // Portamento
        if (slew_active_) {
            float diff = target_freq_ - freq_hz_;
            if (diff > 0.5f || diff < -0.5f) {
                freq_hz_ += diff * SLEW_ALPHA;
                delta_    = freq_to_delta(freq_hz_);
            } else {
                freq_hz_  = target_freq_;
                slew_active_ = false;
                delta_    = freq_to_delta(freq_hz_);
            }
        }

        // ── FM: modular el delta de fase con el bytebeat ─────────
        // delta_fm = delta_base * (1 + fm_depth * bb_norm)
        // Con fm_depth=0 → delta_base sin cambios (seno puro).
        // Con fm_depth=0.4 y bb_norm=+1 → delta*1.4 (fase más rápida ~+5st).
        // Con fm_depth=0.4 y bb_norm=-1 → delta*0.6 (fase más lenta ~-7st).
        // El promedio del bytebeat tiende a 0 así que la afinación
        // percibida permanece centrada en la nota objetivo.
        uint32_t delta_mod = delta_;
        if (fm_depth_ > 0.001f) {
            const float mod   = 1.0f + fm_depth_ * bb_norm_;
            // Clamp: no invertir la fase (mod >= 0.1 para mantener afinación)
            const float mod_c = mod < 0.1f ? 0.1f : (mod > 3.0f ? 3.0f : mod);
            delta_mod = (uint32_t)(delta_ * mod_c);
        }

        // Avance de fase Q24
        phase_q24_ += delta_mod;
        if (phase_q24_ >= 0x1000000u) phase_q24_ -= 0x1000000u;

        // Lookup seno Q15
        const uint8_t idx = (uint8_t)(phase_q24_ >> 16);
        int16_t s = SINE_TABLE_256[idx];

        // ── AM: modular amplitud con el bytebeat ─────────────────
        // am_depth=0 → sin cambio, am_depth=0.15 → tremolo sutil ±15%
        if (am_depth_ > 0.001f) {
            const float am = 1.0f + am_depth_ * bb_norm_;
            const float am_c = am < 0.0f ? 0.0f : (am > 2.0f ? 2.0f : am);
            s = (int16_t)((int32_t)s * (int32_t)(am_c * 16384.0f) >> 14);
        }

        return s;
    }

    float get_freq() const { return freq_hz_; }

private:
    static uint32_t freq_to_delta(float hz) {
        return (uint32_t)(hz * 380.633f);
    }

    static constexpr float SLEW_ALPHA = 0.002f;

    uint32_t phase_q24_   = 0;
    uint32_t delta_       = 0;
    float    freq_hz_     = 220.0f;
    float    target_freq_ = 220.0f;
    bool     slew_active_ = false;

    // Modulación FM/AM
    float    fm_depth_    = 0.0f;   // 0.0–0.40
    float    am_depth_    = 0.0f;   // 0.0–0.15
    float    bb_norm_     = 0.0f;   // bytebeat suavizado -1..+1
};
