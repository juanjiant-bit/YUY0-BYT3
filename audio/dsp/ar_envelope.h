#pragma once
// ar_envelope.h — Bytebeat Machine V1.11
// CAMBIOS vs V1.10:
//   + next_gain(bool gate) → int32_t Q15: avanza estado UNA VEZ, retorna gain.
//     Permite aplicar el mismo gain a L y R sin doble avance.
//   + retrigger(): reinicia ciclo AR desde ATTACK sin reset a 0
//     (permite legato natural en secuencias).
//   + process() reemplazado por next_gain() + apply(). Ver hot path abajo.
//
// USO CORRECTO EN audio_engine.cpp:
//   int32_t gain = envelope_.next_gain(gate);   // una vez por sample
//   l = ArEnvelope::apply(l, gain);
//   r = ArEnvelope::apply(r, gain);
//
// CURVAS:
//   release: 1ms..8s  — v^2.5 sin pow()
//   attack:  1ms..600ms — v^2
//
#include <cstdint>

class ArEnvelope {
public:
    static constexpr uint32_t SAMPLE_RATE   = 44100;
    static constexpr float    ATTACK_MIN_S  = 0.001f;
    static constexpr float    ATTACK_MAX_S  = 0.600f;
    static constexpr float    RELEASE_MIN_S = 0.001f;
    static constexpr float    RELEASE_MAX_S = 8.000f;

    void init() {
        env_q15_          = 0;
        phase_            = Phase::IDLE;
        loop_             = false;
        loop_dir_up_      = true;
        prev_gate_        = false;
        attack_inc_       = time_to_inc(ATTACK_MIN_S);
        release_coef_q15_ = time_to_release_coef(RELEASE_MIN_S);
        loop_attack_inc_  = attack_inc_;
        loop_rel_coef_    = release_coef_q15_;
        release_pot_      = 0.0f;
        attack_pot_       = 0.0f;
        loop_time_scale_  = 1.0f;
    }

    // ── Setters — llamar solo cuando cambia el valor del pot ──
    void set_attack(float v) {
        v = clamp01(v);
        attack_pot_      = v;
        float t          = ATTACK_MIN_S + (ATTACK_MAX_S - ATTACK_MIN_S) * v * v;
        attack_inc_      = time_to_inc(t);
        loop_attack_inc_ = time_to_inc(t * loop_time_scale_);
    }

    void set_release(float v) {
        v = clamp01(v);
        release_pot_      = v;
        float t           = RELEASE_MIN_S + (RELEASE_MAX_S - RELEASE_MIN_S)
                            * v * v * approx_sqrt(v);   // v^2.5
        release_coef_q15_ = time_to_release_coef(t);
        loop_rel_coef_    = time_to_release_coef(t * loop_time_scale_);
    }

    void set_loop(bool on) {
        loop_ = on;
        if (on) { phase_ = Phase::ATTACK; loop_dir_up_ = true; }
        else    { phase_ = Phase::IDLE; }
    }

    void set_loop_time_scale(float v) {
        v = clamp01(v);
        loop_time_scale_ = 0.1f + v * 0.9f;
        float ta = ATTACK_MIN_S + (ATTACK_MAX_S - ATTACK_MIN_S) * attack_pot_ * attack_pot_;
        loop_attack_inc_ = time_to_inc(ta * loop_time_scale_);
        float tr = RELEASE_MIN_S + (RELEASE_MAX_S - RELEASE_MIN_S)
                   * release_pot_ * release_pot_ * approx_sqrt(release_pot_);
        loop_rel_coef_ = time_to_release_coef(tr * loop_time_scale_);
    }

    // Retrigger desde el sequencer (modo normal, sin Note Mode).
    // NO resetea env_q15_ — el attack sube desde donde está el envelope
    // en ese momento (legato natural: no hay click si el release no terminó).
    void retrigger() {
        phase_ = Phase::ATTACK;
        // loop_dir_up_ se resetea también para que el ciclo empiece por attack
        if (loop_) loop_dir_up_ = true;
    }

    // ── Hot path (Core0): un solo avance por sample ───────────
    //
    // next_gain(): avanza el estado EXACTAMENTE UNA VEZ y retorna gain Q15.
    // Llamar con el gate actualizado. Usar apply() para L y R.
    //
    inline int32_t next_gain(bool gate) {
        const bool rising = gate && !prev_gate_;
        prev_gate_ = gate;
        if (loop_) {
            if (rising) { phase_ = Phase::ATTACK; loop_dir_up_ = true; }
            step_loop();
        } else {
            if (rising) phase_ = Phase::ATTACK;
            step_ar(gate);
        }
        return env_q15_;
    }

    // apply(): sample × gain_q15 >> 15. Sin float, sin clamp necesario
    // (32767 × 32767 >> 15 = 32766 < 32767).
    static inline int16_t apply(int16_t s, int32_t gain_q15) {
        return (int16_t)(((int32_t)s * gain_q15) >> 15);
    }

    float get_level()  const { return (float)env_q15_ * (1.0f / 32767.0f); }
    bool  is_looping() const { return loop_; }

private:
    enum class Phase : uint8_t { IDLE, ATTACK, SUSTAIN, RELEASE };

    void step_ar(bool gate) {
        switch (phase_) {
        case Phase::IDLE:    env_q15_ = 0; break;
        case Phase::ATTACK:
            env_q15_ += attack_inc_;
            if (env_q15_ >= 32767) {
                env_q15_ = 32767;
                phase_   = gate ? Phase::SUSTAIN : Phase::RELEASE;
            }
            break;
        case Phase::SUSTAIN:
            env_q15_ = 32767;
            if (!gate) phase_ = Phase::RELEASE;
            break;
        case Phase::RELEASE:
            env_q15_ = ((int32_t)env_q15_ * release_coef_q15_) >> 15;
            if (env_q15_ < 8) { env_q15_ = 0; phase_ = Phase::IDLE; }
            break;
        }
    }

    void step_loop() {
        if (loop_dir_up_) {
            env_q15_ += loop_attack_inc_;
            if (env_q15_ >= 32767) { env_q15_ = 32767; loop_dir_up_ = false; }
        } else {
            env_q15_ = ((int32_t)env_q15_ * loop_rel_coef_) >> 15;
            if (env_q15_ < 8) { env_q15_ = 0; loop_dir_up_ = true; }
        }
    }

    static int32_t time_to_inc(float time_s) {
        if (time_s < 0.0001f) time_s = 0.0001f;
        int32_t inc = (int32_t)(32767.0f / (time_s * (float)SAMPLE_RATE));
        return inc < 1 ? 1 : (inc > 32767 ? 32767 : inc);
    }
    static int32_t time_to_release_coef(float time_s) {
        if (time_s < 0.0001f) time_s = 0.0001f;
        int32_t c = (int32_t)(32767.0f * (1.0f - 1.0f / (time_s * (float)SAMPLE_RATE)));
        return c < 1 ? 1 : (c > 32766 ? 32766 : c);
    }
    static float approx_sqrt(float v) {
        if (v <= 0.0f) return 0.0f;
        float x = v * 0.5f + 0.25f;
        x = 0.5f * (x + v / x);
        x = 0.5f * (x + v / x);
        return x;
    }
    static float clamp01(float v) { return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }

    int32_t env_q15_          = 0;
    Phase   phase_            = Phase::IDLE;
    bool    loop_             = false;
    bool    loop_dir_up_      = true;
    bool    prev_gate_        = false;
    int32_t attack_inc_       = 1;
    int32_t release_coef_q15_ = 32760;
    int32_t loop_attack_inc_  = 1;
    int32_t loop_rel_coef_    = 32760;
    float   release_pot_      = 0.0f;
    float   attack_pot_       = 0.0f;
    float   loop_time_scale_  = 1.0f;
};
