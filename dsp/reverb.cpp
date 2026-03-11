// reverb.cpp — Bytebeat Machine V1.14
// Reverb Schroeder/Freeverb simplificado.
#include "reverb.h"
#include <cstring>

void Reverb::init() {
    reset();
    // Defaults: sala mediana, damping moderado, reverb sutil
    set_room_size(0.84f);
    set_damping(0.5f);
    set_wet(0.25f);
    set_width(0.8f);
}

void Reverb::reset() {
    comb_l0_.reset(); comb_l1_.reset(); comb_l2_.reset(); comb_l3_.reset();
    comb_r0_.reset(); comb_r1_.reset(); comb_r2_.reset(); comb_r3_.reset();
    ap0_l_.reset(); ap0_r_.reset();
    ap1_l_.reset(); ap1_r_.reset();
}

void Reverb::set_room_size(float r) {
    if (r < 0.0f) r = 0.0f;
    if (r > 1.0f) r = 1.0f;
    // Mapeo: room 0.0-1.0 -> feedback 0.70-0.92
    // Valores de Freeverb: roomscale=0.28, roomoffset=0.7
    float fb = 0.7f + r * 0.22f;
    feedback_q15_ = (int32_t)(fb * 32767.0f);
    // Sala grande = cola más oscura; sala chica = más brillo.
    damp_q15_ = (int32_t)((0.22f + r * 0.58f) * 0.4f * 32767.0f);
}

void Reverb::set_damping(float d) {
    if (d < 0.0f) d = 0.0f;
    if (d > 1.0f) d = 1.0f;
    // Freeverb: damp_scale=0.4, damp_offset=0.0
    // Mas damping -> mas absorcion de altas -> sala con moqueta
    damp_q15_ = (int32_t)(d * 0.4f * 32767.0f);
}

void Reverb::set_wet(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    wet_ = w;
    const float wet_curve = w * w;
    wet_q15_ = (int32_t)(wet_curve * 32767.0f);
    if (wet_q15_ < 0) wet_q15_ = 0;
    if (wet_q15_ > 32767) wet_q15_ = 32767;
    dry_q15_ = 32767;
}

void Reverb::set_width(float w) {
    if (w < 0.0f) w = 0.0f;
    if (w > 1.0f) w = 1.0f;
    width_ = w;
    const float w1 = width_ * 0.5f + 0.5f;
    const float w2 = 0.5f - width_ * 0.5f;
    width_l_q15_ = (int32_t)(w1 * 32767.0f);
    width_x_q15_ = (int32_t)(w2 * 32767.0f);
}

void Reverb::process(int16_t input_mono, int16_t& out_l, int16_t& out_r) {
    if (wet_q15_ < 32) return;  // bypass si wet≈0

    // Input pre-scale: dividir por 4 para compensar suma de 4 combs
    int16_t in_scaled = input_mono >> 2;

    // ── 4 comb L en paralelo ─────────────────────────────────
    int32_t sum_l = 0;
    sum_l += comb_l0_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_l += comb_l1_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_l += comb_l2_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_l += comb_l3_.process(in_scaled, feedback_q15_, damp_q15_);

    // ── 4 comb R en paralelo ─────────────────────────────────
    int32_t sum_r = 0;
    sum_r += comb_r0_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_r += comb_r1_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_r += comb_r2_.process(in_scaled, feedback_q15_, damp_q15_);
    sum_r += comb_r3_.process(in_scaled, feedback_q15_, damp_q15_);

    // Clamp tras suma
    if (sum_l >  32767) sum_l =  32767; if (sum_l < -32768) sum_l = -32768;
    if (sum_r >  32767) sum_r =  32767; if (sum_r < -32768) sum_r = -32768;

    // ── 2 allpass en serie (difusion) ────────────────────────
    int16_t diff_l = ap1_l_.process(ap0_l_.process((int16_t)sum_l));
    int16_t diff_r = ap1_r_.process(ap0_r_.process((int16_t)sum_r));

    // ── Width: mezcla stereo en Q15 ──────────────────────────
    int32_t rev_l = ((int32_t)diff_l * width_l_q15_ + (int32_t)diff_r * width_x_q15_) >> 15;
    int32_t rev_r = ((int32_t)diff_r * width_l_q15_ + (int32_t)diff_l * width_x_q15_) >> 15;

    // ── Mezcla dry/wet ───────────────────────────────────────
    int32_t mix_l = (((int32_t)out_l * dry_q15_) >> 15) + ((rev_l * wet_q15_) >> 15);
    int32_t mix_r = (((int32_t)out_r * dry_q15_) >> 15) + ((rev_r * wet_q15_) >> 15);

    if (mix_l >  32767) mix_l =  32767; if (mix_l < -32768) mix_l = -32768;
    if (mix_r >  32767) mix_r =  32767; if (mix_r < -32768) mix_r = -32768;

    out_l = (int16_t)mix_l;
    out_r = (int16_t)mix_r;
}
