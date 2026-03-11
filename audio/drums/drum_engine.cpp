// drum_engine.cpp — Bytebeat Machine V1.14
// Implementación DrumEngine con families y roll sincronizado a BPM.
#include "drum_engine.h"
#include "../../utils/debug_log.h"

// ── Tabla seno Q15 256 puntos (shared con LeadOsc) ───────────
// extern declarado en drum_engine.h y lead_osc.h
const int16_t SINE_TABLE_256[256] = {
       0,  804, 1608, 2410, 3212, 4011, 4808, 5602,
    6393, 7179, 7962, 8739, 9512,10279,11039,11793,
   12539,13279,14010,14732,15446,16151,16846,17530,
   18204,18868,19519,20159,20787,21403,22005,22594,
   23170,23731,24279,24811,25329,25832,26319,26790,
   27245,27683,28105,28510,28898,29268,29621,29956,
   30273,30571,30852,31113,31356,31580,31785,31971,
   32137,32285,32412,32521,32609,32678,32728,32757,
   32767,32757,32728,32678,32609,32521,32412,32285,
   32137,31971,31785,31580,31356,31113,30852,30571,
   30273,29956,29621,29268,28898,28510,28105,27683,
   27245,26790,26319,25832,25329,24811,24279,23731,
   23170,22594,22005,21403,20787,20159,19519,18868,
   18204,17530,16846,16151,15446,14732,14010,13279,
   12539,11793,11039,10279, 9512, 8739, 7962, 7179,
    6393, 5602, 4808, 4011, 3212, 2410, 1608,  804,
       0, -804,-1608,-2410,-3212,-4011,-4808,-5602,
   -6393,-7179,-7962,-8739,-9512,-10279,-11039,-11793,
  -12539,-13279,-14010,-14732,-15446,-16151,-16846,-17530,
  -18204,-18868,-19519,-20159,-20787,-21403,-22005,-22594,
  -23170,-23731,-24279,-24811,-25329,-25832,-26319,-26790,
  -27245,-27683,-28105,-28510,-28898,-29268,-29621,-29956,
  -30273,-30571,-30852,-31113,-31356,-31580,-31785,-31971,
  -32137,-32285,-32412,-32521,-32609,-32678,-32728,-32757,
  -32767,-32757,-32728,-32678,-32609,-32521,-32412,-32285,
  -32137,-31971,-31785,-31580,-31356,-31113,-30852,-30571,
  -30273,-29956,-29621,-29268,-28898,-28510,-28105,-27683,
  -27245,-26790,-26319,-25832,-25329,-24811,-24279,-23731,
  -23170,-22594,-22005,-21403,-20787,-20159,-19519,-18868,
  -18204,-17530,-16846,-16151,-15446,-14732,-14010,-13279,
  -12539,-11793,-11039,-10279,-9512,-8739,-7962,-7179,
   -6393,-5602,-4808,-4011,-3212,-2410,-1608, -804
};

void DrumEngine::init() {
    drum_color_ = 0.0f;
    drum_decay_ = 0.5f;
    duck_depth_q15_ = 16384;
    for (int i = 0; i < 3; i++) {
        roll_active_[i]  = false;
        roll_counter_[i] = 0;
        roll_period_[i]  = 11025;  // default 1/16 a 120BPM
    }
    LOG("DrumEngine V1.14 init OK");
}

void DrumEngine::trigger(DrumId id, float vel) {
    uint8_t fam = color_to_family(drum_color_);
    (void)vel;  // reserved para V1.7 velocity-sensitive
    switch (id) {
    case DRUM_KICK:  kick_.trigger(fam, drum_decay_);  break;
    case DRUM_SNARE: snare_.trigger(fam, drum_decay_); break;
    case DRUM_HAT:   hat_.trigger(fam, drum_decay_);   break;
    }
}

void DrumEngine::roll_on(DrumId id) {
    if ((uint8_t)id >= 3) return;
    roll_active_[(uint8_t)id]  = true;
    roll_counter_[(uint8_t)id] = 0;
    // Trigger inmediato al inicio del roll
    trigger(id);
}

void DrumEngine::roll_off(DrumId id) {
    if ((uint8_t)id >= 3) return;
    roll_active_[(uint8_t)id] = false;
}

void DrumEngine::set_params(float color, float decay, float duck) {
    // V1.7: valor negativo = no cambiar ese parámetro
    // (permite EVT_DRUM_PARAM selectivo para color y decay)
    if (color >= 0.0f) drum_color_ = color;
    if (decay >= 0.0f) drum_decay_ = decay;
    if (duck  >= 0.0f) {
        if (duck > 1.0f) duck = 1.0f;
        const float depth = 0.2f + duck * 0.6f;
        duck_depth_q15_ = (int32_t)(depth * 32767.0f);
    }
}

void DrumEngine::tick_bpm(float bpm) {
    // Calcular periodo de roll (1/16 a BPM actual)
    // periodo_1/16 = (60 / bpm / 4) * 44100 samples
    // = 60 * 44100 / (bpm * 4) = 661500 / bpm
    if (bpm < 20.0f) bpm = 20.0f;
    uint32_t period = (uint32_t)(661500.0f / bpm);
    if (period < 441) period = 441;     // mínimo 10ms (~270BPM)
    for (int i = 0; i < 3; i++) roll_period_[i] = period;
}

void DrumEngine::process(int16_t& out_l, int16_t& out_r, int32_t& sidechain_out_q15) {
    // ── Rolls sincronizados ───────────────────────────────────
    for (uint8_t i = 0; i < 3; i++) {
        if (!roll_active_[i]) continue;
        if (++roll_counter_[i] >= roll_period_[i]) {
            roll_counter_[i] = 0;
            trigger((DrumId)i);
        }
    }

    // ── Procesar voces ────────────────────────────────────────
    int32_t kick_s  = kick_.process(duck_depth_q15_);
    int32_t snare_s = snare_.process();
    int32_t hat_s   = hat_.process();

    // Mezcla: kick a -3dB, snare -6dB, hat -9dB (respecto al kick)
    // En Q15: kick×1.0, snare×0.707, hat×0.5
    int32_t mix = kick_s
                + ((snare_s * 23170) >> 15)   // ×0.707 Q15
                + ((hat_s   * 16384) >> 15);   // ×0.5   Q15

    // Safety clamp
    if (mix >  32767) mix =  32767;
    if (mix < -32768) mix = -32768;

    // Mono (stereo se puede agregar con delay mínimo en V1.7)
    out_l = (int16_t)mix;
    out_r = (int16_t)mix;

    // Sidechain del kick
    sidechain_out_q15 = kick_.sidechain_gain_q15_;
}
