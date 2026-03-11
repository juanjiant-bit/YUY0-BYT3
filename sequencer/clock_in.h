#pragma once
// clock_in.h — V1.4.4
// IRQ mínima: timestamp + intervalo + tick counter
// EMA y BPM en update() (loop Core1, fuera de IRQ)
// Todos los timestamps en uint64_t
#include <cstdint>
#include "hardware/sync.h"

class ClockIn {
public:
    static constexpr uint8_t  PIN          = 16;
    static constexpr uint64_t GLITCH_US    = 1000;
    static constexpr uint64_t TIMEOUT_US   = 500000;
    static constexpr uint8_t  EXT_PPQN    = 24;
    static constexpr float    EMA_ALPHA   = 0.15f;
    static constexpr uint16_t MAX_PENDING = 96;

    void init();
    void update();         // Core1 loop: EMA + BPM + timeout

    bool     is_ext_sync()   const { return ext_sync_; }
    float    get_bpm()       const { return bpm_; }
    uint64_t get_period_us() const { return period_smooth_us_; }
    bool     consume_tick();   // IRQ-safe

private:
    static void gpio_irq_cb(uint gpio, uint32_t events);
    void        on_pulse();

    // Escritas por IRQ
    volatile uint64_t last_pulse_us_ = 0;
    volatile uint64_t last_interval_ = 0;
    volatile uint16_t ticks_pending_ = 0;
    volatile bool     new_interval_  = false;

    // Solo Core1 loop
    uint64_t period_smooth_us_ = 0;
    float    bpm_              = 120.0f;
    bool     ext_sync_         = false;
};
