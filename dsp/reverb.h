#pragma once
// reverb.h — Bytebeat Machine V1.14
// Reverb Schroeder/Freeverb simplificado para RP2040.
// 4 comb filters + 2 allpass, stereo out.
//
// RAM: ~40KB en SRAM. Asegurar que el linker lo ponga en RAM, no Flash.
// Tiempo de reverb: room_size=0.84 -> cola de ~1.5s @ 44100Hz.
//
// PARAMETROS:
//   room_size 0.0-1.0: tamanio de sala (feedback comb, 0.7-0.9)
//   damping   0.0-1.0: absorcion de altas frecuencias
//   wet       0.0-1.0: mezcla
//   width     0.0-1.0: separacion stereo (0=mono, 1=ancho maximo)
//
#include <cstdint>
#include <cstring>

// ── Comb filter con damping LPF integrado ─────────────────────
// y[n] = x[n] + feedback * lpf(y[n-L])
// lpf: y_lpf = y_lpf*(1-damp) + y_delayed*damp  (1er orden)
template<uint16_t SIZE>
struct CombFilter {
    int16_t buf[SIZE] = {};
    uint16_t pos      = 0;
    int32_t  lpf_acc  = 0;  // acumulador LPF en Q15

    void reset() { memset(buf, 0, sizeof(buf)); pos = 0; lpf_acc = 0; }

    int16_t process(int16_t input, int32_t feedback_q15, int32_t damp_q15) {
        int16_t delayed = buf[pos];

        // LPF: lpf_acc = delayed*(1-damp) + lpf_acc*damp
        int32_t one_minus_damp = 32767 - damp_q15;
        lpf_acc = ((int32_t)delayed * one_minus_damp + lpf_acc * damp_q15) >> 15;

        // Clamp LPF
        if (lpf_acc >  32767) lpf_acc =  32767;
        if (lpf_acc < -32768) lpf_acc = -32768;

        // Feedback
        int32_t fb = ((int32_t)(int16_t)lpf_acc * feedback_q15) >> 15;
        int32_t out = (int32_t)input + fb;

        // Clamp y escribir
        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        buf[pos] = (int16_t)out;
        pos = (pos + 1 >= SIZE) ? 0 : pos + 1;

        return (int16_t)out;
    }
};

// ── Allpass filter ─────────────────────────────────────────────
// Difusion: mezcla el pasado con el presente
// y[n] = -x[n] + x[n-L] + g*y[n-L]    (g tipicamente 0.5)
template<uint16_t SIZE>
struct AllpassFilter {
    int16_t buf[SIZE] = {};
    uint16_t pos      = 0;

    void reset() { memset(buf, 0, sizeof(buf)); pos = 0; }

    int16_t process(int16_t input) {
        static constexpr int32_t G_Q15 = 16384;  // 0.5 en Q15

        int16_t delayed = buf[pos];
        int32_t out = -(int32_t)input + delayed
                      + ((int32_t)delayed * G_Q15 >> 15)
                      - ((int32_t)input   * G_Q15 >> 15);

        if (out >  32767) out =  32767;
        if (out < -32768) out = -32768;
        buf[pos] = (int16_t)((int32_t)input + ((int32_t)(int16_t)out * G_Q15 >> 15));
        pos = (pos + 1 >= SIZE) ? 0 : pos + 1;
        return (int16_t)out;
    }
};

// ── Reverb principal ───────────────────────────────────────────
class Reverb {
public:
    // Longitudes de comb (primos entre si, stereo offset de 23 samples)
    // Basado en Freeverb, ajustado para 44100Hz
    // L: 1116 1188 1277 1356  (en samples)
    // R: L+23 cada uno
    static constexpr uint16_t COMB_L0 = 1116, COMB_R0 = 1139;
    static constexpr uint16_t COMB_L1 = 1188, COMB_R1 = 1211;
    static constexpr uint16_t COMB_L2 = 1277, COMB_R2 = 1300;
    static constexpr uint16_t COMB_L3 = 1356, COMB_R3 = 1379;
    static constexpr uint16_t AP_0    = 556,  AP_1 = 441;

    void init();
    void set_room_size(float r);  // 0.0-1.0
    void set_damping  (float d);  // 0.0-1.0
    void set_wet      (float w);  // 0.0-1.0
    void set_width    (float w);  // 0.0-1.0

    // Procesar in-place. input_mono = mono mix pre-reverb.
    // out_l/out_r: suma reverb al audio existente segun wet.
    void process(int16_t input_mono, int16_t& out_l, int16_t& out_r);
    bool is_active() const { return wet_q15_ >= 32; }

    void reset();

private:
    // Comb filters L
    CombFilter<COMB_L0> comb_l0_;
    CombFilter<COMB_L1> comb_l1_;
    CombFilter<COMB_L2> comb_l2_;
    CombFilter<COMB_L3> comb_l3_;
    // Comb filters R
    CombFilter<COMB_R0> comb_r0_;
    CombFilter<COMB_R1> comb_r1_;
    CombFilter<COMB_R2> comb_r2_;
    CombFilter<COMB_R3> comb_r3_;
    // Allpass (compartidos L/R — suficiente para difusion)
    AllpassFilter<AP_0> ap0_l_, ap0_r_;
    AllpassFilter<AP_1> ap1_l_, ap1_r_;

    // Parametros en Q15
    int32_t feedback_q15_ = 27525;  // room_size=0.84 -> 0.84*32767=27525
    int32_t damp_q15_     = 16384;  // damping=0.5
    float   wet_          = 0.25f;  // cache valor usuario
    float   width_        = 0.8f;   // cache valor usuario
    int32_t wet_q15_      = 8192;
    int32_t dry_q15_      = 24575;
    int32_t width_l_q15_  = 29490;
    int32_t width_x_q15_  = 3277;
};
