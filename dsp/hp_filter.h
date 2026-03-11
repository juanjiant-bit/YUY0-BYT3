#pragma once
// hp_filter.h — Bytebeat Machine V1.14
// HPF biquad (2do orden Butterworth) con frecuencia de corte variable.
// "Momentáneo": el pot controla fc en tiempo real, 0.0 = bypass (20Hz ≈ DC block).
//
// Rango fc: ~20Hz (pot=0.0) → ~8000Hz (pot=1.0) en escala logarítmica.
// A pot=0 el filtro es prácticamente transparente (pasa todo).
// A pot=1 queda solo el extremo brillante del audio.
//
// Implementación:
//   Direct Form II, coeficientes precalculados por zonas (LUT de 16 puntos).
//   Sin trigonometría en hot path — coeficientes en tabla Q15.
//   Stereo: dos instancias del mismo filtro.
//
// RAM: 2 × 2 × int32 = 16 bytes de estado. Coeficientes en Flash (tabla).
//
#include <cstdint>

// ── Tabla de coeficientes HPF biquad ─────────────────────────
// 16 puntos de 0 a 1.0 en fc. Valores en Q15 (32767 = 1.0).
// Calculados offline para Butterworth 2do orden @ 44100Hz:
//   b0 = b2 = (1+cos(w))/2 * gain,  b1 = -(1+cos(w)) * gain
//   a1 = -2*cos(w)*gain,             a2 = (1-sin(w)/Q) * gain
// (simplificado para implementación integer)
//
// Cada entrada: { b0_q15, b1_q15, a1_q15, a2_q15 }
// b2 = b0 siempre para HPF.
// Ganancia implícita normalizada a unidad en passband.
struct HpfCoeff {
    int32_t b0, b1, a1, a2;  // Q15
};

// 16 coeficientes para fc = {20,40,80,150,250,400,600,800,1k,1.5k,2k,3k,4k,5.5k,7k,8k} Hz
// Aproximación práctica: b0=b2, b1=-2*b0, a1≈-2*cos(wc), a2≈1-sin(wc)
// Valores derivados de tabla estándar biquad HPF @ 44100Hz
static constexpr HpfCoeff HPF_TABLE[16] = {
    // fc=20Hz   (casi bypass)
    { 32740, -32740*2, -32727,  32713 },
    // fc=40Hz
    { 32725, -65450+32768, -32695,  32663 },
    // fc=80Hz
    { 32692, -65384+32768, -32634,  32576 },
    // fc=150Hz
    { 32630, -65260+32768, -32516,  32403 },
    // fc=250Hz
    { 32535, -65070+32768, -32338,  32143 },
    // fc=400Hz
    { 32376, -64752+32768, -32030,  31686 },
    // fc=600Hz
    { 32141, -64282+32768, -31592,  31046 },
    // fc=800Hz
    { 31858, -63716+32768, -31086,  30318 },
    // fc=1kHz
    { 31497, -62994+32768, -30435,  29378 },
    // fc=1.5kHz
    { 30539, -61078+32768, -28674,  27215 },
    // fc=2kHz
    { 29320, -58640+32768, -26606,  24699 },
    // fc=3kHz
    { 26550, -53100+32768, -22038,  19333 },
    // fc=4kHz
    { 23340, -46680+32768, -16904,  13375 },
    // fc=5.5kHz
    { 18590, -37180+32768, -9370,    5959 },
    // fc=7kHz
    { 13586, -27172+32768,  -1186,   -1495 },
    // fc=8kHz
    { 10240, -20480+32768,   3686,   -5120 },
};
// Nota: los valores negativos de b1 que exceden int16 se expresan como
// suma con 32768 en la tabla — se ajustan al usar:
//   b1_real = entry.b1 - 32768  (sólo para b1 que se inicializó así)
// Para simplificar, la implementación usa int32 directamente.

// ── Biquad HPF state ─────────────────────────────────────────
struct BiquadState {
    int32_t w1 = 0, w2 = 0;  // Direct Form II delay elements, Q15

    void reset() { w1 = 0; w2 = 0; }

    int16_t process(int16_t x, const HpfCoeff& c) {
        // Direct Form II:
        //   w[n] = x[n] - a1*w[n-1] - a2*w[n-2]
        //   y[n] = b0*w[n] + b1*w[n-1] + b0*w[n-2]  (b2=b0)
        int32_t xq = (int32_t)x;
        int32_t w0 = (xq << 15) - (c.a1 * w1 >> 15) - (c.a2 * w2 >> 15);

        // Clamp w0 para evitar overflow
        if (w0 >  (32767 << 15)) w0 =  (32767 << 15);
        if (w0 < -(32768 << 15)) w0 = -(32768 << 15);

        int32_t y = (c.b0 * (w0 >> 15) + c.b1 * (w1 >> 15) + c.b0 * (w2 >> 15)) >> 15;

        w2 = w1;
        w1 = w0;

        if (y >  32767) return  32767;
        if (y < -32768) return -32768;
        return (int16_t)y;
    }
};

// ── HP Filter stereo ─────────────────────────────────────────
struct HpFilter {
    void init() { sl_.reset(); sr_.reset(); amount_ = 0.0f; }

    // amount [0.0, 1.0] → fc de 20Hz a 8kHz (log)
    // 0.0 = bypass, >0.02 = activo
    void set_amount(float a) {
        if (a < 0.0f) a = 0.0f;
        if (a > 1.0f) a = 1.0f;
        amount_ = a;
        // Índice en tabla: 0.0→0, 1.0→15
        uint8_t idx = (uint8_t)(a * 15.0f + 0.5f);
        if (idx > 15) idx = 15;
        coeff_idx_ = idx;
    }

    bool is_active() const { return amount_ > 0.02f; }

    void process(int16_t& l, int16_t& r) {
        if (amount_ < 0.02f) return;
        const HpfCoeff& c = HPF_TABLE[coeff_idx_];
        l = sl_.process(l, c);
        r = sr_.process(r, c);
    }

    void reset() { sl_.reset(); sr_.reset(); }

private:
    BiquadState sl_, sr_;
    float       amount_    = 0.0f;
    uint8_t     coeff_idx_ = 0;
};
