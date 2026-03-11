#pragma once
// audio_output_i2s.h — Bytebeat Machine
// Backend PCM5102A via Philips I2S + PIO
//
// PINES (fijos, no cambiar sin actualizar el .pio):
//   GP10 = BCLK  (BCK del módulo)
//   GP11 = LRCK  (LCK del módulo) — debe ser BCLK+1
//   GP12 = DIN   (DIN del módulo)
//
// MÓDULO GY-PCM5102 — jumpers requeridos:
//   FMT  = sin soldar (I2S estándar, FMT=0 via pull-down interno)
//   SCK  = sin soldar (SCK interno del módulo)
//   DEMP = sin soldar
//   FLT  = sin soldar
//   XSMT = SOLDAR a 3V3 (si no, el chip queda en mute)
//
// PIO: hardcodeado en PIO0/SM0.
// WS2812 debe usar PIO1 (ver led_controller.h).

#include "audio_output.h"
#include "hardware/pio.h"

class AudioOutputI2S : public AudioOutput {
public:
    static constexpr uint8_t  PIN_BCLK    = 10;
    static constexpr uint8_t  PIN_LRCK    = 11;   // debe ser PIN_BCLK + 1
    static constexpr uint8_t  PIN_DIN     = 12;
    static constexpr uint32_t SAMPLE_RATE = 44100;

    void init()  override;
    void write(int16_t left, int16_t right) override;
    void start() override {}
    void stop()  override {}

private:
    // Hardcodeado PIO0/SM0 — WS2812 usa PIO1
    PIO  pio_         = pio0;
    uint sm_          = 0;
    uint offset_      = 0;
    bool initialized_ = false;
};
