// clock_out.cpp — V1.4.4
#include "clock_out.h"
#include "../utils/debug_log.h"

void ClockOut::init() {
    gpio_init(PIN);
    gpio_set_dir(PIN, GPIO_OUT);
    gpio_put(PIN, 0);
    alarm_id_ = -1;
}

void ClockOut::on_tick() {
    // Race condition original: gpio_get(PIN) puede leer HIGH mientras el alarm
    // ya se disparó pero alarm_id_ aún no se puso a -1 (el callback corre en IRQ).
    // Solución: usar alarm_id_ como fuente de verdad, no el pin.
    if (alarm_id_ >= 0) {
        // Hay un alarm activo → pulso en curso, ignorar este tick.
        // No cancelar: es más seguro dejar que el alarm existente baje el pin.
        return;
    }

    gpio_put(PIN, 1);
    alarm_id_t id = add_alarm_in_us(PULSE_US, alarm_cb,
                                    static_cast<void*>(this), true);
    if (id < 0) {
        // Pool de alarms agotado (SDK tiene 16 slots). Bajar el pin directamente.
        // Esto evita que el pin quede trabado en HIGH si add_alarm falla.
        gpio_put(PIN, 0);
    } else {
        alarm_id_ = id;
    }
}

int64_t ClockOut::alarm_cb(alarm_id_t, void* user_data) {
    // Este callback corre en IRQ. Mínimo trabajo.
    ClockOut* self = static_cast<ClockOut*>(user_data);
    gpio_put(PIN, 0);
    self->alarm_id_ = -1;
    return 0;  // no reschedule
}
