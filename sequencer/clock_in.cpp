// clock_in.cpp — V1.4.4
#include "clock_in.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"
#include "pico/stdlib.h"
#include "../utils/debug_log.h"

static ClockIn* g_clock_in = nullptr;

void ClockIn::init() {
    g_clock_in = this;
    gpio_init(PIN);
    gpio_set_dir(PIN, GPIO_IN);

    // El circuito hardware tiene un optoacoplador cuyo colector va a 3V3
    // a través de 470Ω. Pin idle = HIGH. Pulso = flanco de subida (LOW→HIGH).
    // pull_down sería incorrecto: mantendría el pin en LOW cuando no hay señal
    // compitiendo contra la resistencia de pull-up de hardware. Sin pull interno.
    gpio_disable_pulls(PIN);

    // gpio_set_irq_enabled_with_callback registra un único handler global
    // para todos los GPIOs del RP2040. Si hay otro periférico con IRQ GPIO
    // (no hay en este diseño, pero por seguridad), el callback ya instalado
    // por el SDK encadena correctamente. Usamos esta función correctamente.
    gpio_set_irq_enabled_with_callback(
        PIN, GPIO_IRQ_EDGE_RISE, true, &ClockIn::gpio_irq_cb);
}

void ClockIn::gpio_irq_cb(uint gpio, uint32_t events) {
    // El callback recibe TODOS los eventos IRQ GPIO del chip.
    // Filtrar estrictamente por PIN y por tipo de evento.
    if (gpio == PIN && (events & GPIO_IRQ_EDGE_RISE) && g_clock_in)
        g_clock_in->on_pulse();
}

// ── IRQ: mínima — sin float ──────────────────────────────────
void ClockIn::on_pulse() {
    uint64_t now      = time_us_64();
    uint64_t interval = now - last_pulse_us_;

    if (last_pulse_us_ != 0 && interval < GLITCH_US)  return;

    if (last_pulse_us_ != 0 && interval < TIMEOUT_US) {
        last_interval_ = interval;
        new_interval_  = true;
        if (ticks_pending_ < MAX_PENDING) ticks_pending_++;
    }

    last_pulse_us_ = now;
}

// ── Loop: EMA + BPM + timeout ────────────────────────────────
void ClockIn::update() {
    uint64_t interval = 0;
    {
        uint32_t s = save_and_disable_interrupts();
        if (new_interval_) {
            new_interval_ = false;
            interval      = last_interval_;
        }
        restore_interrupts(s);
    }

    if (interval > 0) {
        if (period_smooth_us_ == 0)
            period_smooth_us_ = interval;
        else
            period_smooth_us_ = (uint64_t)(
                period_smooth_us_ +
                (int64_t)(EMA_ALPHA *
                          ((float)interval - (float)period_smooth_us_)));

        bpm_ = 60000000.0f / ((float)period_smooth_us_ * EXT_PPQN);

        if (!ext_sync_) {
            ext_sync_ = true;
            LOG_CLOCK("CLOCK: EXT | BPM=%.1f", bpm_);
        }
    }

    // Timeout
    if (ext_sync_ && (time_us_64() - last_pulse_us_ > TIMEOUT_US)) {
        ext_sync_         = false;
        period_smooth_us_ = 0;
        uint32_t s        = save_and_disable_interrupts();
        ticks_pending_    = 0;
        new_interval_     = false;
        restore_interrupts(s);
        LOG_CLOCK("CLOCK: timeout → INT | BPM=%.1f", bpm_);
    }
}

bool ClockIn::consume_tick() {
    uint32_t s = save_and_disable_interrupts();
    bool ok    = ticks_pending_ > 0;
    if (ok) ticks_pending_--;
    restore_interrupts(s);
    return ok;
}
