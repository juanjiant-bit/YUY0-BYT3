#pragma once
// led_controller.h — snapshot/transport LED feedback
// Controla la barra WS2812 de 8 LEDs (GRB, 5V, GP22)
// + LED onboard GP25 como metrónomo de tempo
//
// Principios de diseño:
//   - Un LED por snapshot (0-7)
//   - Animaciones no bloqueantes: update() llamado desde Core1 loop
//   - update() solo toca el PIO cuando el buffer cambió (dirty flag)
//   - Sin malloc: todo estático
//   - GP25 (LED onboard) = metrónomo, toggle en quarter-note (cada 24 ticks @ 96 PPQN)
//
// Conexión hardware:
//   GP22 → DIN de la barra WS2812
//   VBUS (5V) → VCC de la barra
//   GND → GND de la barra
//   100Ω entre GP22 y DIN (opcional, reduce reflexiones)
//   100µF entre VCC y GND de la barra (opcional, estabiliza picos)
//
// Formato de color: GRB (estándar WS2812)

#include <cstdint>
#include "hardware/pio.h"
#include "hardware/gpio.h"
#include "../state/encoder_state.h"
#include "ws2812.pio.h"   // generado por pioasm en tiempo de compilación

class LedController {
public:
    static constexpr uint8_t  NUM_LEDS     = 8;
    static constexpr uint8_t  PIN_WS2812   = 22;
    static constexpr uint8_t  PIN_ONBOARD  = 25;
    static constexpr float    WS2812_FREQ  = 800000.0f;

    // ── Colores base (GRB, 0x00GGRRBB) ─────────────────────────
    static constexpr uint32_t COL_OFF      = 0x000000;
    static constexpr uint32_t COL_SNAP     = 0x001A00;  // verde tenue (idle)
    static constexpr uint32_t COL_ACTIVE   = 0x003A00;  // verde medio (slot activo)
    static constexpr uint32_t COL_TRIGGER  = 0x00FF00;  // verde brillante (flash en trigger)
    static constexpr uint32_t COL_REC      = 0x003300;  // rojo pulsante
    static constexpr uint32_t COL_PLAY     = 0x220000;  // verde tenue (beat marker)
    static constexpr uint32_t COL_BEAT     = 0x440000;  // verde beat
    static constexpr uint32_t COL_SHIFT    = 0x080808;  // blanco muy tenue (shift layer)
    static constexpr uint32_t COL_SAVE_SWEEP = 0x4444FF; // blanco frío (save sweep)
    static constexpr uint32_t COL_PAGE_BPM    = 0x220000; // verde
    static constexpr uint32_t COL_PAGE_SWING  = 0x221600; // amarillo
    static constexpr uint32_t COL_PAGE_ROOT   = 0x000022; // azul
    static constexpr uint32_t COL_PAGE_SCALE  = 0x001022; // violeta
    static constexpr uint32_t COL_PAGE_MUTATE = 0x002200; // rojo
    static constexpr uint32_t COL_NOTE_MODE   = 0x000C18; // azul/violeta tenue
    static constexpr uint32_t COL_ENV_LOOP_ON = 0x180A00; // ámbar cálido
    static constexpr uint32_t COL_ENV_LOOP_OFF= 0x040404; // blanco tenue
    static constexpr uint32_t COL_SEQ_SNAP    = 0x001E00; // verde paso con snapshot
    static constexpr uint32_t COL_SEQ_DRUM    = 0x001A10; // rojo paso con drums
    static constexpr uint32_t COL_SEQ_BOTH    = 0x002010; // naranja intersección snap+drum
    static constexpr uint32_t COL_SEQ_NOTE    = 0x080008; // azul paso con nota/tie
    static constexpr uint32_t COL_SEQ_NOTE_DRUM = 0x100010; // violeta nota+drum
    static constexpr uint32_t COL_SEQ_MOTION  = 0x080818; // azul tenue automation/fx/arp
    static constexpr uint32_t COL_SEQ_EMPTY   = 0x020202; // gris tenue step vacío
    static constexpr uint32_t COL_SEQ_PLAYHEAD= 0x202020; // blanco playhead

    void init();

    // ── Llamar desde Core1 loop ─────────────────────────────────
    // tick_ppqn: tick actual del sequencer (96 PPQN interno)
    // active_slot: snapshot activo (0-7)
    // is_playing: sequencer en PLAYING o RECORDING
    // is_rec: REC activo
    // shift_held: SHIFT pad presionado
    void update(uint32_t tick_ppqn,
                uint8_t  active_slot,
                bool     is_playing,
                bool     is_rec,
                bool     shift_held,
                bool     note_mode_active,
                bool     env_loop_active,
                bool     sequence_view_active,
                uint8_t  sequence_len,
                uint8_t  sequence_page_base,
                uint8_t  playhead_step,
                uint8_t  write_step,
                uint8_t  page_snap_mask,
                uint8_t  page_note_mask,
                uint8_t  page_drum_mask,
                uint8_t  page_motion_mask,
                bool     manual_step_write,
                bool     armed_record,
                uint8_t  preroll_steps_left);

    // ── Eventos one-shot (llamar desde input_router o main) ─────
    void on_snapshot_trigger(uint8_t slot);        // flash en ese LED
    void on_flash_save();                          // sweep blanco todos los LEDs
    void on_note_on(uint8_t slot);                 // flash breve en Note Mode
    void on_note_mode_toggle(bool active);         // flash global Note Mode on/off
    void on_env_loop_toggle(bool active);          // flash global Env Loop on/off
    void show_encoder_state(const EncoderState& state); // página + valor por 1 segundo

    // ── Punch FX: notificar qué FX están activos ────────────────
    // fx_mask: bitmask de FX activos (bit 0 = slot 0 = FX_STUTTER, etc.)
    // Se muestra como overlay de color cian sobre los LEDs de snapshot
    // mientras haya algún FX activo.
    void set_punch_fx_mask(uint8_t fx_mask);

    // ── HOME: feedback visual del hold progresivo ────────────────
    // progress 0.0..1.0 = cuánto del hold se completó (para animar los LEDs)
    // level: 0 = nada, 1 = SOFT confirmado, 2 = FULL confirmado
    void on_home_progress(float progress, uint8_t level);

private:
    // PIO state — WS2812 usa PIO1 para no colisionar con I2S en PIO0
    PIO      pio_      = pio1;
    uint     sm_       = 0;
    uint     offset_   = 0;
    bool     pio_ok_   = false;

    // LED buffer (GRB)
    uint32_t buf_[NUM_LEDS] = {};
    bool     dirty_         = true;

    // Estado interno de animaciones
    uint8_t  active_slot_   = 0;
    bool     is_playing_    = false;
    bool     is_rec_        = false;
    bool     shift_held_      = false;
    bool     note_mode_active_ = false;
    bool     env_loop_active_  = false;
    bool     sequence_view_active_ = false;
    bool     manual_step_write_ = false;
    bool     armed_record_ = false;
    uint8_t  preroll_steps_left_ = 0;
    uint8_t  sequence_len_ = 0;
    uint8_t  sequence_page_base_ = 0;
    uint8_t  playhead_step_ = 0;
    uint8_t  write_step_ = 0;
    uint8_t  page_snap_mask_ = 0;
    uint8_t  page_note_mask_ = 0;
    uint8_t  page_drum_mask_ = 0;
    uint8_t  page_motion_mask_ = 0;

    // Flash por trigger: por slot, contador descendente
    // Cada tick del update() lo decrementa
    uint8_t  trigger_flash_[NUM_LEDS] = {};
    static constexpr uint8_t FLASH_TICKS = 12;  // ~6 updates @ 50ms

    // Save sweep
    bool     save_sweep_    = false;
    uint8_t  save_step_     = 0;
    uint32_t save_last_ms_  = 0;
    static constexpr uint8_t  SAVE_SWEEP_STEPS = NUM_LEDS + 4;
    static constexpr uint32_t SAVE_STEP_MS     = 40;

    // Metrónomo onboard
    bool     metro_state_   = false;
    uint32_t last_tick_     = 0xFFFFFFFF;

    // Overlay de página del encoder
    bool         page_overlay_active_ = false;
    EncoderState page_state_          = {};
    uint32_t     page_until_ms_       = 0;
    static constexpr uint32_t PAGE_SHOW_MS = 1000;

    bool         note_flash_active_   = false;
    bool         note_flash_on_       = false;
    uint32_t     note_flash_until_ms_ = 0;
    bool         env_flash_active_    = false;
    bool         env_flash_on_        = false;
    uint32_t     env_flash_until_ms_  = 0;
    static constexpr uint32_t TOGGLE_FLASH_MS = 220;

    // ── Punch FX overlay ─────────────────────────────────────────
    // fx_mask: bitmask de FX activos (bit N = slot N activo)
    // Cuando un FX está activo, su LED correspondiente se tinta cian.
    uint8_t      punch_fx_mask_ = 0;
    // Colores por FX slot (GRB) — diferenciados para identificación rápida
    static constexpr uint32_t COL_FX_STUTTER = 0x003030; // cian frío
    static constexpr uint32_t COL_FX_FREEZE  = 0x002040; // azul hielo
    static constexpr uint32_t COL_FX_OCT_DN  = 0x001040; // azul índigo
    static constexpr uint32_t COL_FX_OCT_UP  = 0x002820; // turquesa claro
    static constexpr uint32_t COL_FX_VIBRATO = 0x002840; // azul-cian
    static constexpr uint32_t COL_FX_RPT4    = 0x001830; // azul marino
    static constexpr uint32_t COL_FX_RPT8    = 0x001820; // verde-azul
    static constexpr uint32_t FX_COLORS[7] = {
        COL_FX_STUTTER, COL_FX_FREEZE, COL_FX_OCT_DN, COL_FX_OCT_UP,
        COL_FX_VIBRATO, COL_FX_RPT4,   COL_FX_RPT8
    };

    // ── HOME feedback visual ──────────────────────────────────────
    // Durante el hold del encoder, los LEDs hacen un fill progresivo:
    //   0 → 700 ms  : LEDs se llenan de izquierda a derecha (naranja → blanco)
    //   700ms        : flash blanco rápido = SOFT confirmado
    //   700 → 1500ms : fill continúa hasta blanco brillante
    //   1500ms       : flash blanco total = FULL confirmado
    float        home_progress_  = 0.0f;
    uint8_t      home_level_     = 0;       // 0=idle, 1=soft, 2=full
    bool         home_flash_     = false;
    uint32_t     home_flash_until_ms_ = 0;
    static constexpr uint32_t HOME_FLASH_MS = 120;

    // ── Helpers ─────────────────────────────────────────────────
    void     flush();
    void     set_led(uint8_t i, uint32_t grb);
    uint32_t dim(uint32_t grb, uint8_t factor);  // factor 0-255
    void     update_metronome(uint32_t tick_ppqn);
    void     update_leds(uint32_t tick_ppqn);
    uint32_t page_color(EncoderMode mode) const;
    uint8_t  page_value_leds() const;
    uint8_t  page_single_led() const;
    uint32_t add_tint(uint32_t base, uint32_t tint, uint8_t amount) const;
};

// ── Implementación inline (header-only para simplicidad) ────────

inline void LedController::init() {
    // LED onboard
    gpio_init(PIN_ONBOARD);
    gpio_set_dir(PIN_ONBOARD, GPIO_OUT);
    gpio_put(PIN_ONBOARD, 0);

    // PIO para WS2812 — usa PIO1 (PIO0 reservado para I2S audio)
    if (!pio_can_add_program(pio_, &ws2812_program)) {
        // PIO1 lleno — no debería ocurrir en uso normal
        pio_ok_ = false;
        return;
    }
    offset_ = pio_add_program(pio_, &ws2812_program);
    sm_     = pio_claim_unused_sm(pio_, true);
    ws2812_program_init(pio_, sm_, offset_, PIN_WS2812, WS2812_FREQ);
    pio_ok_ = true;

    // Apagar todos los LEDs al inicio
    for (uint8_t i = 0; i < NUM_LEDS; i++) buf_[i] = COL_OFF;
    flush();
}

inline void LedController::set_led(uint8_t i, uint32_t grb) {
    if (i >= NUM_LEDS) return;
    if (buf_[i] != grb) { buf_[i] = grb; dirty_ = true; }
}

inline uint32_t LedController::dim(uint32_t grb, uint8_t factor) {
    if (factor == 0)   return 0;
    if (factor == 255) return grb;
    uint8_t g = (uint8_t)(((grb >> 16) & 0xFF) * factor / 255);
    uint8_t r = (uint8_t)(((grb >>  8) & 0xFF) * factor / 255);
    uint8_t b = (uint8_t)(((grb >>  0) & 0xFF) * factor / 255);
    return ((uint32_t)g << 16) | ((uint32_t)r << 8) | b;
}

inline void LedController::flush() {
    if (!pio_ok_ || !dirty_) return;
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        // WS2812 espera GRB en los bits [23:0] del word 32-bit
        pio_sm_put_blocking(pio_, sm_, buf_[i] << 8u);
    }
    dirty_ = false;
}

inline void LedController::update_metronome(uint32_t tick_ppqn) {
    // Quarter-note = 24 ticks externos = 96 ticks internos / 4
    // INT_PPQN = 96, quarter cada 24 ticks
    static constexpr uint32_t QUARTER = 24;
    uint32_t beat = tick_ppqn / QUARTER;
    if (beat != last_tick_) {
        last_tick_  = beat;
        metro_state_ = !metro_state_;
        gpio_put(PIN_ONBOARD, metro_state_ ? 1 : 0);
    }
}


inline uint32_t LedController::page_color(EncoderMode mode) const {
    switch (mode) {
    case EncoderMode::BPM:    return COL_PAGE_BPM;
    case EncoderMode::SWING:  return COL_PAGE_SWING;
    case EncoderMode::ROOT:   return COL_PAGE_ROOT;
    case EncoderMode::SCALE:  return COL_PAGE_SCALE;
    case EncoderMode::MUTATE: return COL_PAGE_MUTATE;
    default:                  return COL_SHIFT;
    }
}
inline uint8_t LedController::page_value_leds() const {
    switch (page_state_.mode) {
    case EncoderMode::BPM: {
        int bpm = (int)page_state_.bpm;
        if (bpm < 60) bpm = 60;
        if (bpm > 200) bpm = 200;
        return (uint8_t)(1 + (bpm - 60) * 7 / 140);
    }
    case EncoderMode::SWING: {
        float v = page_state_.swing_amount / 0.65f;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return (uint8_t)(v * 8.0f + 0.999f);
    }
    case EncoderMode::MUTATE: {
        float v = page_state_.mutate_amount;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        return (uint8_t)(v * 8.0f + 0.999f);
    }
    case EncoderMode::SCALE:
        return (uint8_t)((page_state_.scale_id % 8u) + 1u);
    default:
        return 0;
    }
}

inline uint8_t LedController::page_single_led() const {
    switch (page_state_.mode) {
    case EncoderMode::ROOT:
        return (uint8_t)((page_state_.root * NUM_LEDS) / 12u);
    default:
        return 0;
    }
}

inline uint32_t LedController::add_tint(uint32_t base, uint32_t tint, uint8_t amount) const {
    const uint8_t bg = (base >> 16) & 0xFF;
    const uint8_t br = (base >> 8) & 0xFF;
    const uint8_t bb = base & 0xFF;
    const uint8_t tg = (tint >> 16) & 0xFF;
    const uint8_t tr = (tint >> 8) & 0xFF;
    const uint8_t tb = tint & 0xFF;
    auto blend = [amount](uint8_t b, uint8_t t) -> uint8_t {
        uint16_t out = (uint16_t)b + ((uint16_t)t * amount) / 255u;
        return out > 255u ? 255u : (uint8_t)out;
    };
    return ((uint32_t)blend(bg, tg) << 16) | ((uint32_t)blend(br, tr) << 8) | blend(bb, tb);
}

inline void LedController::update(uint32_t tick_ppqn,
                                   uint8_t  active_slot,
                                   bool     is_playing,
                                   bool     is_rec,
                                   bool     shift_held,
                                   bool     note_mode_active,
                                   bool     env_loop_active,
                                   bool     sequence_view_active,
                                   uint8_t  sequence_len,
                                   uint8_t  sequence_page_base,
                                   uint8_t  playhead_step,
                                   uint8_t  write_step,
                                   uint8_t  page_snap_mask,
                                   uint8_t  page_note_mask,
                                   uint8_t  page_drum_mask,
                                   uint8_t  page_motion_mask,
                                   bool     manual_step_write,
                                   bool     armed_record,
                                   uint8_t  preroll_steps_left) {
    active_slot_       = active_slot;
    is_playing_        = is_playing;
    is_rec_            = is_rec;
    shift_held_        = shift_held;
    note_mode_active_  = note_mode_active;
    env_loop_active_   = env_loop_active;
    sequence_view_active_ = sequence_view_active;
    manual_step_write_ = manual_step_write;
    armed_record_ = armed_record;
    preroll_steps_left_ = preroll_steps_left;
    sequence_len_ = sequence_len;
    sequence_page_base_ = sequence_page_base;
    playhead_step_ = playhead_step;
    write_step_ = write_step;
    page_snap_mask_ = page_snap_mask;
    page_note_mask_ = page_note_mask;
    page_drum_mask_ = page_drum_mask;
    page_motion_mask_ = page_motion_mask;

    update_metronome(tick_ppqn);
    update_leds(tick_ppqn);
    flush();
}

inline void LedController::update_leds(uint32_t tick_ppqn) {
    // ── Save sweep ───────────────────────────────────────────────
    if (save_sweep_) {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (now_ms - save_last_ms_ >= SAVE_STEP_MS) {
            save_last_ms_ = now_ms;
            save_step_++;
            if (save_step_ >= SAVE_SWEEP_STEPS) {
                save_sweep_ = false;
                save_step_  = 0;
            }
        }
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            if (save_step_ <= i)
                set_led(i, COL_OFF);
            else if (save_step_ == i + 1)
                set_led(i, COL_SAVE_SWEEP);
            else
                set_led(i, dim(COL_SAVE_SWEEP, 60));
        }
        return;  // override todo mientras dure el sweep
    }

    // ── Flash corto Note Mode / Env Loop ─────────────────────────
    {
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());
        if (note_flash_active_) {
            if ((int32_t)(note_flash_until_ms_ - now_ms) > 0) {
                const uint32_t col = note_flash_on_ ? COL_NOTE_MODE : COL_SHIFT;
                for (uint8_t i = 0; i < NUM_LEDS; i++) set_led(i, col);
                return;
            }
            note_flash_active_ = false;
        }
        if (env_flash_active_) {
            if ((int32_t)(env_flash_until_ms_ - now_ms) > 0) {
                const uint32_t col = env_flash_on_ ? COL_ENV_LOOP_ON : COL_ENV_LOOP_OFF;
                for (uint8_t i = 0; i < NUM_LEDS; i++) set_led(i, col);
                return;
            }
            env_flash_active_ = false;
        }
        if (page_overlay_active_) {
            if ((int32_t)(page_until_ms_ - now_ms) > 0) {
                const uint32_t col = page_color(page_state_.mode);
                const uint8_t fill = page_value_leds();
                const uint8_t marker = page_single_led();
                for (uint8_t i = 0; i < NUM_LEDS; i++) {
                    uint32_t led = dim(col, 24);
                    if (page_state_.mode == EncoderMode::ROOT) {
                        led = (i == marker) ? col : dim(col, 18);
                    } else {
                        if (i < fill) led = col;
                    }
                    set_led(i, led);
                }
                return;
            }
            page_overlay_active_ = false;
        }
    }

    if (armed_record_) {
        const uint8_t step = (uint8_t)((16u - preroll_steps_left_) & 0x0Fu);
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            uint32_t col = dim(COL_SEQ_PLAYHEAD, (i == (step & 0x07u)) ? 255 : ((i < 4u) ? 54 : 20));
            if ((step & 0x03u) == 0u && i == (step & 0x07u)) col = COL_SEQ_PLAYHEAD;
            set_led(i, col);
        }
        return;
    }

    if (sequence_view_active_) {
        const uint8_t local_playhead = (uint8_t)(playhead_step_ >= sequence_page_base_ ? (playhead_step_ - sequence_page_base_) : 0xFF);
        const uint8_t local_write = (uint8_t)(write_step_ >= sequence_page_base_ ? (write_step_ - sequence_page_base_) : 0xFF);
        const bool beat_pulse = is_playing_ && ((tick_ppqn % 24u) < 6u);
        for (uint8_t i = 0; i < NUM_LEDS; i++) {
            const uint8_t abs_step = (uint8_t)(sequence_page_base_ + i);
            uint32_t col = COL_OFF;
            const bool in_len = abs_step < sequence_len_ || (manual_step_write_ && abs_step <= write_step_);
            const uint8_t bit = (uint8_t)(1u << i);
            if (in_len) {
                const bool has_snap = (page_snap_mask_ & bit) != 0;
                const bool has_note = (page_note_mask_ & bit) != 0;
                const bool has_drum = (page_drum_mask_ & bit) != 0;
                const bool has_motion = (page_motion_mask_ & bit) != 0;
                if ((has_snap || has_note) && has_drum) col = has_note && !has_snap ? COL_SEQ_NOTE_DRUM : COL_SEQ_BOTH;
                else if (has_snap) col = COL_SEQ_SNAP;
                else if (has_note) col = COL_SEQ_NOTE;
                else if (has_drum) col = COL_SEQ_DRUM;
                else if (has_motion) col = COL_SEQ_MOTION;
                else col = COL_SEQ_EMPTY;
                if (i == 0) col = add_tint(col, COL_SEQ_PLAYHEAD, 28);
            }
            if (trigger_flash_[i] > 0) {
                uint8_t brightness = (uint8_t)((uint32_t)trigger_flash_[i] * 255 / FLASH_TICKS);
                col = dim(COL_TRIGGER, brightness);
                trigger_flash_[i]--;
            }
            if (local_playhead < NUM_LEDS) {
                if (i < local_playhead) col = dim(col, 92);
                if (i == local_playhead) {
                    col = beat_pulse ? add_tint(col, COL_SEQ_PLAYHEAD, 224)
                                     : add_tint(col, COL_SEQ_PLAYHEAD, 128);
                }
            }
            if (manual_step_write_ && local_write < NUM_LEDS && i == local_write) {
                col = add_tint(col, COL_PAGE_MUTATE, 96);
            }
            if (note_mode_active_) col = add_tint(col, COL_NOTE_MODE, 40);
            if (env_loop_active_ && (tick_ppqn % 24u < 6u)) col = add_tint(col, COL_ENV_LOOP_ON, 30);
            if (shift_held_) col = add_tint(col, COL_SHIFT, 34);
            set_led(i, col);
        }
        return;
    }

    // ── Animación de beat en el LED activo ───────────────────────
    // Cada quarter-note, el LED activo hace un flash breve
    static constexpr uint32_t BEAT_TICKS   = 24;   // quarter-note
    static constexpr uint32_t BEAT_FLASH   = 4;    // duración del flash en ticks
    uint32_t phase = tick_ppqn % BEAT_TICKS;
    bool on_beat = (phase < BEAT_FLASH) && is_playing_;

    // ── Renderizar 8 LEDs ────────────────────────────────────────
    for (uint8_t i = 0; i < NUM_LEDS; i++) {
        uint32_t col;

        // Prioridad 1: flash por trigger
        if (trigger_flash_[i] > 0) {
            uint8_t brightness = (uint8_t)((uint32_t)trigger_flash_[i] * 255 / FLASH_TICKS);
            col = dim(COL_TRIGGER, brightness);
            trigger_flash_[i]--;
            set_led(i, col);
            continue;
        }

        // Prioridad 2: slot activo
        if (i == active_slot_) {
            if (is_rec_ && (tick_ppqn % 48 < 24)) {
                // REC: pulso rojo al BPM (media nota encendido, media apagado)
                col = COL_REC;
            } else if (on_beat && is_playing_) {
                // Beat flash en el slot activo
                col = COL_BEAT;
            } else {
                col = COL_ACTIVE;
            }
        } else {
            // Slots inactivos: tenue
            col = COL_SNAP;
        }

        // ── Note Mode: override total a azul sólido ──────────────
        // En Note Mode los 8 pads SNAP son un teclado distinto.
        // Hacemos el cambio imposible de no ver: LED sólido azul.
        // El slot activo es blanco-azul más brillante.
        if (note_mode_active_) {
            static constexpr uint32_t COL_NM_ACTIVE   = 0x003060; // blanco azulado
            static constexpr uint32_t COL_NM_INACTIVE = 0x001840; // azul medio
            col = (i == active_slot_) ? COL_NM_ACTIVE : COL_NM_INACTIVE;
            // Sobre el sólido, mantener el flash de nota presionada
            if (trigger_flash_[i] > 0) {
                uint8_t b = (uint8_t)((uint32_t)trigger_flash_[i] * 255 / FLASH_TICKS);
                col = dim(0x00A0FF, b);  // blanco-cian brillante al tocar nota
            }
        }

        if (env_loop_active_ && (tick_ppqn % 24u < 6u)) {
            col = add_tint(col, COL_ENV_LOOP_ON, (i == active_slot_) ? 90 : 48);
        }
        if (shift_held_) {
            col = add_tint(col, COL_SHIFT, 72);
        }

        // ── Punch FX overlay ─────────────────────────────────────
        // Si el FX del slot i está activo, tintamos el LED con su color.
        // El mapa FX slot → LED slot es 1:1 (FX 0 = LED 0 = SNAP C etc.)
        if (punch_fx_mask_ & (1u << i)) {
            static constexpr uint32_t FX_COL[8] = {
                0x003030, // 0 STUTTER  cian frío
                0x002040, // 1 FREEZE   azul hielo
                0x001040, // 2 OCT_DOWN índigo
                0x002820, // 3 OCT_UP   turquesa
                0x002840, // 4 VIBRATO  azul-cian
                0x001830, // 5 REPEAT4  azul marino
                0x001820, // 6 REPEAT8  verde-azul
                0x002030, // 7 (spare)
            };
            // Override completo: el FX es más importante que la snapshot view
            col = FX_COL[i];
            // Pequeño pulso para indicar que está activo (no solo encendido)
            const bool fx_pulse = (tick_ppqn % 12u < 6u);
            if (!fx_pulse) col = dim(col, 140);
        }

        // ── HOME hold: fill progresivo sobre todos los LEDs ──────
        if (home_progress_ > 0.0f || home_flash_) {
            if (home_flash_ && to_ms_since_boot(get_absolute_time()) < home_flash_until_ms_) {
                // Flash de confirmación: blanco brillante
                static constexpr uint32_t COL_HOME_FLASH = 0x404040;
                col = (home_level_ >= 2) ? COL_HOME_FLASH
                                         : dim(COL_HOME_FLASH, 160);
            } else {
                home_flash_ = false;
                // Fill de izquierda a derecha: N LEDs encendidos
                float filled_leds = home_progress_ * (float)NUM_LEDS;
                if ((float)i < filled_leds) {
                    // Color: naranja → blanco según nivel
                    const uint32_t col_home = (home_level_ >= 1)
                        ? 0x303030   // blanco suave (SOFT alcanzado)
                        : dim(0x103010, (uint8_t)(home_progress_ * 200.0f + 55.0f));
                    col = add_tint(col, col_home, 180);
                }
            }
        }

        set_led(i, col);
    }
}

inline void LedController::on_snapshot_trigger(uint8_t slot) {
    if (slot >= NUM_LEDS) return;
    trigger_flash_[slot] = FLASH_TICKS;
    dirty_ = true;
}

inline void LedController::on_flash_save() {
    save_sweep_ = true;
    save_step_  = 0;
    save_last_ms_ = to_ms_since_boot(get_absolute_time());
    dirty_ = true;
}

inline void LedController::show_encoder_state(const EncoderState& state) {
    page_state_ = state;
    page_overlay_active_ = true;
    page_until_ms_ = to_ms_since_boot(get_absolute_time()) + PAGE_SHOW_MS;
    dirty_ = true;
}

inline void LedController::on_note_mode_toggle(bool active) {
    note_flash_on_ = active;
    note_flash_active_ = true;
    note_flash_until_ms_ = to_ms_since_boot(get_absolute_time()) + TOGGLE_FLASH_MS;
    dirty_ = true;
}

inline void LedController::on_env_loop_toggle(bool active) {
    env_flash_on_ = active;
    env_flash_active_ = true;
    env_flash_until_ms_ = to_ms_since_boot(get_absolute_time()) + TOGGLE_FLASH_MS;
    dirty_ = true;
}

inline void LedController::on_note_on(uint8_t slot) {
    // En Note Mode, el pad presionado hace un flash corto
    if (slot >= NUM_LEDS) return;
    trigger_flash_[slot] = FLASH_TICKS / 2;
    dirty_ = true;
}

inline void LedController::set_punch_fx_mask(uint8_t fx_mask) {
    if (punch_fx_mask_ == fx_mask) return;
    punch_fx_mask_ = fx_mask;
    dirty_ = true;
}

inline void LedController::on_home_progress(float progress, uint8_t level) {
    home_progress_ = progress;
    const uint8_t prev_level = home_level_;
    home_level_ = level;
    // Flash de confirmación al alcanzar cada nivel
    if (level > prev_level && level > 0) {
        home_flash_      = true;
        home_flash_until_ms_ = to_ms_since_boot(get_absolute_time()) + HOME_FLASH_MS;
    }
    if (progress <= 0.0f) {
        home_flash_   = false;
        home_level_   = 0;
    }
    dirty_ = true;
}
