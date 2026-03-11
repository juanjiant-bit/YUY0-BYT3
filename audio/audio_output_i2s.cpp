// audio_output_i2s.cpp — Bytebeat Machine
// PCM5102A via Philips I2S + PIO0/SM0
#include "audio_output_i2s.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "pico/stdlib.h"
#include "pcm5102_i2s.pio.h"

void AudioOutputI2S::init() {
    if (initialized_) return;

    // Usar la función de init del c-sdk block del .pio
    // PIO0, SM0 — hardcodeado, no conflicts con WS2812 (que usa PIO1)
    offset_ = pio_add_program(pio_, &pcm5102_i2s_program);
    sm_     = pio_claim_unused_sm(pio_, true);

    pcm5102_i2s_program_init(pio_, sm_, offset_,
                              PIN_DIN, PIN_BCLK, SAMPLE_RATE);

    // Precargar silencio para que el PLL del PCM5102A se enganche
    // antes de que lleguen muestras reales (evita el "pop" de arranque)
    for (int i = 0; i < 16; ++i) {
        pio_sm_put_blocking(pio_, sm_, 0u);
    }

    initialized_ = true;
}

void AudioOutputI2S::write(int16_t left, int16_t right) {
    // Empaquetar en 32 bits: L ocupa los 16 bits altos (se emite primero),
    // R ocupa los 16 bits bajos.
    // El .pio hace out(1) MSB-first desde el bit 31, entonces:
    //   bits 31..16 → canal LRCK=0 (izquierdo)
    //   bits 15..0  → canal LRCK=1 (derecho)
    const uint32_t frame = ((uint32_t)(uint16_t)left  << 16)
                         |  (uint32_t)(uint16_t)right;
    pio_sm_put_blocking(pio_, sm_, frame);
}
