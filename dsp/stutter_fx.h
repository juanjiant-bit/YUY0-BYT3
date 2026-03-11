#pragma once
// stutter_fx.h — Bytebeat Machine V1.14
// Versión optimizada: rate/interpolación/crossfade en fixed-point para
// bajar floats en el hot path del DSP.
// Buffer circular + loop con rate variable y crossfade.
//
// ARQUITECTURA:
//   Dos estados: CAPTURING y PLAYING.
//   CAPTURING: graba las ultimas BUF_SAMPLES muestras en ring buffer.
//   PLAYING:   reproduce ese buffer en loop, mezclado con dry por depth.
//
// PARAMETROS (actualizables en tiempo real):
//   depth 0.0-1.0: crossfade dry/wet
//   rate  0.25-4.0: velocidad de reproduccion del loop
//     rate=1.0 -> loop exacto
//     rate=0.5 -> "slomo", loop suena mas largo/grave
//     rate=2.0 -> loop rapido/glitch
//
// AFTERTOUCH (opcion A, PAD_STUTTER):
//   pressure 0.0-0.4 -> depth 0.0-1.0  (activa el stutter)
//   pressure 0.4-1.0 -> rate  1.0-3.0  (acelera el loop al apretar)
//   Al soltar: depth decae suave a 0 (no click)
//
// NOTA: estéreo — procesa L y R con el mismo read_pos (mono loop).
//
#include <cstdint>

class StutterFx {
public:
    // Buffer de 882 samples = 20ms @ 44100Hz
    // Suficiente para capturar 1/8 a 120bpm (62ms) si se extiende,
    // pero 20ms ya da el efecto glitch clasico.
    static constexpr uint32_t BUF_SAMPLES = 882;

    void init();

    // Llamar cada sample desde AudioEngine::process_one_sample()
    // in_l/in_r: señal entrante (modifica in-place si activo)
    void process(int16_t& in_l, int16_t& in_r);

    // Control de gate (desde EVT_FX_ON / EVT_FX_OFF)
    void gate_on();
    void gate_off();
    void gate_repeat(uint16_t divisor);

    // Aftertouch: pressure 0.0-1.0
    // Mapeo: 0.0-0.4 -> depth, 0.4-1.0 -> rate
    void set_pressure(float pressure);

    // Control directo (para otros modos de aftertouch)
    void set_depth(float d);   // 0.0=dry 1.0=wet
    void set_rate (float r);   // 0.25-4.0

    bool is_active() const { return gate_on_; }
    float get_depth() const { return (float)depth_target_q15_ / 32767.0f; }
    float get_rate()  const { return (float)rate_q16_ / 65536.0f; }

private:
    // Buffer circular (mono, se usa para L y R)
    int16_t  buf_l_[BUF_SAMPLES] = {};
    int16_t  buf_r_[BUF_SAMPLES] = {};

    // Estado
    bool     gate_on_     = false;
    uint32_t write_pos_    = 0;              // posicion de escritura en buffer
    uint32_t read_pos_q16_ = 0;              // posicion de lectura en Q16
    uint32_t loop_start_   = 0;              // inicio del loop congelado
    uint32_t loop_len_     = BUF_SAMPLES;    // longitud del loop capturado

    // Parametros en fixed-point
    int32_t  depth_target_q15_ = 0;          // objetivo del crossfade
    int32_t  depth_smooth_q15_ = 0;          // valor suavizado (evita clicks)
    uint32_t rate_q16_         = (1u << 16); // velocidad de lectura
    uint32_t rate_smooth_q16_  = (1u << 16);

    static constexpr int32_t DEPTH_ALPHA_Q15 = 1638; // ~0.05 en Q15
    static constexpr int32_t RATE_ALPHA_Q15  = 4915; // ~0.15 en Q15
};
