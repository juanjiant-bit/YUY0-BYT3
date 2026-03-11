#pragma once
// chorus.h — Bytebeat Machine V1.14
// Chorus BBD (Bucket Brigade Delay) simplificado para RP2040.
// Sin malloc, sin float pesado en hot path.
//
// Algoritmo:
//   L: delay modulado por LFO senoidal  + (delay_center + depth * sin(lfo))
//   R: LFO con fase 180° (antifase) → efecto stereo sin duplicar buffer
//   wet mix con la señal original
//
// Parámetros:
//   amount  0.0–1.0:
//     0.0       → bypass total
//     0.0–0.5   → wet mix sube de 0 a 0.5, delay center fijo en 7ms
//     0.5–1.0   → wet 0.5, depth del LFO sube (más modulación)
//
// RAM: DELAY_MAX_SAMP × 2 bytes = 1764 × 2 = ~3.5 KB
//
#include <cstdint>
#include <cstring>
#include "../synth/bytebeat_node.h"   // para EvalContext (no usado aquí, solo compatibilidad)

struct Chorus {
    // Delay máximo: 40ms @ 44100 = 1764 samples
    // Center: 7ms = 309 samples
    // Depth máx: 3ms = 132 samples (LFO swing ±3ms)
    static constexpr uint16_t DELAY_MAX_SAMP  = 1764;   // 40ms
    static constexpr uint16_t DELAY_CENTER    = 309;    // 7ms
    static constexpr uint16_t DEPTH_MAX_SAMP  = 132;    // 3ms
    // LFO rate: ~0.5Hz en Q24 phase increment
    // phase_inc = rate_hz / sample_rate * 2^24
    // 0.5Hz → 0.5/44100 * 16777216 ≈ 190
    static constexpr uint32_t LFO_RATE_MIN_Q24 = 150;  // ~0.39Hz
    static constexpr uint32_t LFO_RATE_MAX_Q24 = 560;  // ~1.45Hz

    void init() {
        memset(buf_l_, 0, sizeof(buf_l_));
        memset(buf_r_, 0, sizeof(buf_r_));
        write_pos_ = 0;
        lfo_phase_ = 0;
        amount_    = 0.0f;
        wet_q15_   = 0;
        dry_q15_   = 32767;
        feedback_q15_ = 0;
        lfo_rate_q24_ = LFO_RATE_MIN_Q24;
        delay_center_samp_ = DELAY_CENTER;
        depth_samp_ = DEPTH_MAX_SAMP / 4;
    }

    // amount [0.0, 1.0] — llamar desde AudioEngine safe point
    void set_amount(float a) {
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        amount_ = a;

        // Curva musical: amount bajo = ancho y suave,
        // amount alto = más ensemble / seasick.
        const float wet = 0.55f * a * a;
        const float depth_norm = 0.12f + 0.88f * (a * a);
        const float center_norm = 1.0f + 0.22f * a;
        const float feedback = (a > 0.55f) ? ((a - 0.55f) / 0.45f) * 0.18f : 0.0f;

        wet_q15_ = (int32_t)(wet * 32767.0f);
        if (wet_q15_ < 0) wet_q15_ = 0;
        if (wet_q15_ > 32767) wet_q15_ = 32767;
        dry_q15_ = 32767 - wet_q15_;
        feedback_q15_ = (int32_t)(feedback * 32767.0f);
        lfo_rate_q24_ = (uint32_t)(LFO_RATE_MIN_Q24 + (LFO_RATE_MAX_Q24 - LFO_RATE_MIN_Q24) * (0.2f + 0.8f * a));
        delay_center_samp_ = (uint16_t)(DELAY_CENTER * center_norm);
        if (delay_center_samp_ >= DELAY_MAX_SAMP) delay_center_samp_ = DELAY_MAX_SAMP - 1;
        depth_samp_ = (int32_t)(depth_norm * DEPTH_MAX_SAMP);
        if (depth_samp_ < 1) depth_samp_ = 1;
        if (depth_samp_ > DEPTH_MAX_SAMP) depth_samp_ = DEPTH_MAX_SAMP;
    }

    bool is_active() const { return amount_ > 0.005f; }

    // Procesar stereo in-place
    void process(int16_t& l, int16_t& r) {
        if (amount_ < 0.005f) return;

        // LFO senoidal Q15 usando tabla de seno compartida
        extern const int16_t SINE_TABLE_256[256];
        int16_t lfo_l = SINE_TABLE_256[(uint8_t)(lfo_phase_ >> 16)];
        // R: fase + 128 = 180° de diferencia → antifase
        int16_t lfo_r = SINE_TABLE_256[(uint8_t)((lfo_phase_ >> 16) + 128)];

        // Parámetros precomputados en set_amount() para evitar float en hot path.
        int32_t off_l = (int32_t)delay_center_samp_ + ((lfo_l * depth_samp_) >> 15);
        int32_t off_r = (int32_t)delay_center_samp_ + ((lfo_r * depth_samp_) >> 15);

        // Clamp offset
        if (off_l < 1) off_l = 1;
        if (off_l >= DELAY_MAX_SAMP) off_l = DELAY_MAX_SAMP - 1;
        if (off_r < 1) off_r = 1;
        if (off_r >= DELAY_MAX_SAMP) off_r = DELAY_MAX_SAMP - 1;

        // Leer desde posición retrasada (interpolación lineal entre 2 muestras)
        uint16_t rd_l0 = (write_pos_ + DELAY_MAX_SAMP - (uint16_t)off_l) % DELAY_MAX_SAMP;
        uint16_t rd_l1 = (rd_l0 + 1) % DELAY_MAX_SAMP;
        uint16_t rd_r0 = (write_pos_ + DELAY_MAX_SAMP - (uint16_t)off_r) % DELAY_MAX_SAMP;
        uint16_t rd_r1 = (rd_r0 + 1) % DELAY_MAX_SAMP;

        int32_t del_l  = buf_l_[rd_l0];
        int32_t del_r  = buf_r_[rd_r0];
        (void)rd_l1; (void)rd_r1;  // delay entero — delay simple

        // Escribir en buffer circular con feedback muy leve para más cuerpo.
        int32_t wr_l = (int32_t)l + ((del_l * feedback_q15_) >> 15);
        int32_t wr_r = (int32_t)r + ((del_r * feedback_q15_) >> 15);
        if (wr_l > 32767) wr_l = 32767; if (wr_l < -32768) wr_l = -32768;
        if (wr_r > 32767) wr_r = 32767; if (wr_r < -32768) wr_r = -32768;
        buf_l_[write_pos_] = (int16_t)wr_l;
        buf_r_[write_pos_] = (int16_t)wr_r;

        int32_t out_l = ((int32_t)l * dry_q15_ + del_l * wet_q15_) >> 15;
        int32_t out_r = ((int32_t)r * dry_q15_ + del_r * wet_q15_) >> 15;

        if (out_l >  32767) out_l =  32767;
        if (out_l < -32768) out_l = -32768;
        if (out_r >  32767) out_r =  32767;
        if (out_r < -32768) out_r = -32768;

        l = (int16_t)out_l;
        r = (int16_t)out_r;

        // Avanzar posición y LFO
        write_pos_ = (write_pos_ + 1) % DELAY_MAX_SAMP;
        lfo_phase_ += lfo_rate_q24_;
        // wrap a 2π (una vuelta completa = 256 steps de la tabla × 65536)
        // lfo_phase_ es uint32 → wrap automático al overflowear
    }

private:
    int16_t  buf_l_[DELAY_MAX_SAMP] = {};
    int16_t  buf_r_[DELAY_MAX_SAMP] = {};
    uint16_t write_pos_ = 0;
    uint32_t lfo_phase_ = 0;
    float    amount_    = 0.0f;
    int32_t  wet_q15_   = 0;
    int32_t  dry_q15_   = 32767;
    int32_t  feedback_q15_ = 0;
    uint32_t lfo_rate_q24_ = LFO_RATE_MIN_Q24;
    uint16_t delay_center_samp_ = DELAY_CENTER;
    int32_t  depth_samp_ = DEPTH_MAX_SAMP / 4;
};
