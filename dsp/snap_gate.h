#pragma once
// snap_gate.h — Bytebeat Machine V1.14
// Snap Gate: VCA periódico que trunca los envelopes de todo el audio.
// A amount=0 → bypass. A amount=1 → todo el audio se convierte en "ticks".
//
// Mecanismo:
//   Un retriggering periódico genera una ventana de ganancia muy corta.
//   La ventana tiene attack instantáneo y decay exponencial muy rápido.
//   El período se sincroniza a BPM (1/16 de beat por defecto).
//   
//   amount 0.0    → bypass (ganancia 1.0 siempre)
//   amount 0.0–0.5 → decay de la ventana baja de "normal" a muy corto
//   amount 0.5–1.0 → period también se acorta (densidad de ticks aumenta)
//
// Período base: 1/16 de beat a 120 BPM = 22050/16 ≈ 1378 samples
// Decay base (amount=0.5): 50ms = 2205 samples
// Decay mínimo (amount=1.0): 2ms = 88 samples → tick puro
//
// El período puede actualizarse desde AudioEngine via set_bpm().
//
#include <cstdint>

struct SnapGate {
    // Constantes (todas en samples @ 44100Hz)
    static constexpr uint32_t PERIOD_BASE  = 1378;   // 1/16 beat @ 120BPM
    static constexpr uint32_t DECAY_LONG   = 2205;   // 50ms (amount=0.0)
    static constexpr uint32_t DECAY_SHORT  = 88;     // 2ms (amount=1.0)
    // Release Q15: coeficiente de exponencial
    // Para decay D ms: coeff = 1 - 1/(D*44.1) en Q15 aprox
    // decay=50ms → coeff ≈ 32750, decay=2ms → coeff ≈ 31810
    static constexpr int32_t  GAIN_FULL_Q15= 32767;

    void init() {
        gain_q15_   = GAIN_FULL_Q15;
        phase_      = 0;
        period_     = PERIOD_BASE;
        amount_     = 0.0f;
        decay_coeff_= GAIN_FULL_Q15;
    }

    // amount [0.0, 1.0] — llamar desde safe point
    void set_amount(float a) {
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        amount_ = a;

        if (a < 0.01f) {
            gain_q15_    = GAIN_FULL_Q15;
            decay_coeff_ = GAIN_FULL_Q15;
            return;
        }

        // decay_samples: interpola entre DECAY_LONG y DECAY_SHORT
        float decay_s = DECAY_LONG * (1.0f - a) + DECAY_SHORT * a;
        // coeff = exp(-1/decay_s) ≈ 1 - 1/decay_s para decay largo
        // En Q15: coeff_q15 = 32767 - 32767/decay_s
        float coeff_f = 1.0f - 1.0f / decay_s;
        if (coeff_f < 0.0f) coeff_f = 0.0f;
        decay_coeff_ = (int32_t)(coeff_f * 32767.0f);

        // period: a 0.5 = PERIOD_BASE, a 1.0 = PERIOD_BASE/4 (más denso)
        if (a > 0.5f) {
            float density = 1.0f + (a - 0.5f) * 2.0f * 3.0f;  // 1× → 4×
            period_ = (uint32_t)(base_period_ / density);
            if (period_ < 44) period_ = 44;  // mínimo 1ms entre ticks
        } else {
            period_ = base_period_;
        }
    }

    // Sincronizar período a BPM externo (llamar cuando BPM cambia)
    void set_bpm(float bpm) {
        if (bpm < 20.0f) bpm = 20.0f;
        if (bpm > 300.0f) bpm = 300.0f;
        uint32_t beat_samp = (uint32_t)(44100.0f * 60.0f / bpm);
        base_period_ = beat_samp / 16;  // 1/16 de beat
        if (base_period_ < 44) base_period_ = 44;
        // Reescalar el period actual si amount ya estaba seteado
        if (amount_ <= 0.5f) {
            period_ = base_period_;
        } else {
            float density = 1.0f + (amount_ - 0.5f) * 2.0f * 3.0f;
            period_ = (uint32_t)(base_period_ / density);
            if (period_ < 44) period_ = 44;
        }
    }

    bool is_active() const { return amount_ > 0.01f; }

    // Procesar stereo in-place
    void process(int16_t& l, int16_t& r) {
        if (amount_ < 0.01f) return;

        // Retrigger cuando el phase alcanza el period
        if (phase_ >= period_) {
            phase_    = 0;
            gain_q15_ = GAIN_FULL_Q15;  // attack instantáneo
        }
        phase_++;

        // Decay exponencial
        gain_q15_ = (gain_q15_ * decay_coeff_) >> 15;

        // Aplicar ganancia
        int32_t out_l = ((int32_t)l * gain_q15_) >> 15;
        int32_t out_r = ((int32_t)r * gain_q15_) >> 15;

        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        l = (int16_t)out_l;
        r = (int16_t)out_r;
    }

private:
    int32_t  gain_q15_    = GAIN_FULL_Q15;
    uint32_t phase_       = 0;
    uint32_t period_      = PERIOD_BASE;
    uint32_t base_period_ = PERIOD_BASE;
    int32_t  decay_coeff_ = GAIN_FULL_Q15;
    float    amount_      = 0.0f;
};
