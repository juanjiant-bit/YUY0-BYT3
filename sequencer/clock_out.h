#pragma once
// clock_out.h — V1.4.4
// Pulso limpio via alarm. alarm_id como miembro (no global).
#include <cstdint>
#include "pico/time.h"
#include "hardware/gpio.h"

class ClockOut {
public:
    static constexpr uint8_t  PIN      = 17;
    static constexpr uint64_t PULSE_US = 2000;  // 2ms

    void init();
    void on_tick();
    void update() {}  // no-op: alarm maneja el bajado del pin

private:
    static int64_t alarm_cb(alarm_id_t id, void* user_data);
    alarm_id_t alarm_id_ = -1;
};
