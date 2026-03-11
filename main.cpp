// main.cpp — Bytebeat Machine V1.14
// Core0: audio engine (bytebeat + lead + drums + DSP)
// Core1: inputs + sequencer + encoder + flash + MIDI
//
// BOOT
//   Carga snapshots desde flash automáticamente.
//
// SAVE
//   SHIFT + REC + PLAY
//
// ENCODER MODES
//   BPM / SWING / ROOT / SCALE / MUTATE
//
// POTS (NORMAL)
//   P0 Macro      P1 Tonal      P2 Spread
//   P3 Drive      P4 TimeDiv    P5 SnapGate
//
// POTS (SHIFT)
//   P0 Glide      P1 EnvAttack  P2 EnvRelease
//   P3 StutterRate P4 Grain      P5 HP Filter
//
// POTS (SHIFT + REC)
//   P0 ReverbRoom P1 ReverbWet  P2 Chorus
//   P3 DrumDecay  P4 DrumColor  P5 DuckAmount
//
// PERFORMANCE PADS
//   SHIFT + PLAY            -> toggle Note Mode
//   SHIFT + C/F/A/D/G/B/E   -> punch FX
//   SHIFT + H               -> snapshot arp
//   MUTE + KICK/SNARE/HAT   -> mute por drum
//   SHIFT + KICK + SNARE    -> randomize / mutate global
//
// PINOUT:
//   GP0      MIDI OUT (UART0 TX) via BC547 + 220Ω
//   GP1      MIDI IN  (UART0 RX) via 6N138 (pin8 Vcc→3V3 requerido)
//   GP2/3/4  MUX S0/S1/S2  (74HC4051)
//   GP5/6/7  ROW 0/1/2     (pad capacitivo drive, via 1MOhm)
//   GP8/9/13/14/15  COL 0-4 (pad sense, sin pull)
//   GP10     I2S BCK  -> PCM5102 BCK
//   GP11     I2S LRCK -> PCM5102 LCK/WS
//   GP12     I2S DIN  -> PCM5102 DIN
//   GP16     CLOCK IN  (10kΩ serie + clamp BAT43)
//   GP17     CLOCK OUT (470Ω serie)
//   GP26     ADC0 (mux COM del 74HC4051)
//
// NOTA: UART0 (GP0/GP1) = MIDI. Debug solo por USB (stdio_uart=0 en cmake).
// PAGE FEEDBACK: los modos del encoder se muestran en la barra WS2812
// durante 1 segundo al cambiar de página; no hay LED RGB dedicado.
//
// AUDIO BACKEND: por default se usa PCM5102 por I2S (PIO).

#include "pico/stdlib.h"

#include "pico/multicore.h"
#include "hardware/clocks.h"
#include "audio/audio_engine.h"
#include "audio/audio_output_i2s.h"
#include "audio/audio_output_pwm.h"
#include "io/cap_pad_handler.h"
#include "io/adc_handler.h"
#include "io/input_router.h"
#include "state/state_manager.h"
#include "state/flash_store.h"
#include "sequencer/sequencer.h"
#include "sequencer/clock_in.h"
#include "sequencer/clock_out.h"
#include "midi/uart_midi.h"
#include "midi/midi_router.h"
#include "utils/ring_buffer.h"
#include "utils/debug_log.h"
#include "led/led_controller.h"
#include "sequencer/event_types.h"

// ── Heartbeat de prueba (quitar cuando el hardware esté listo) ──
#ifdef NO_PAD_HARDWARE
#define HEARTBEAT_PIN PIN_ONBOARD_LED
#endif

// ── Compartido Core0/Core1 ────────────────────────────────────
static RingBuffer<SequencerEvent, 128> g_event_queue;
static StateManager                   g_state;

// ── Core0 ─────────────────────────────────────────────────────
static AudioOutputI2S  g_audio_out;
// Fallback rápido para bring-up analógico:
// static AudioOutputPWM  g_audio_out;
static AudioEngine     g_audio_engine;

// ── Core1 ─────────────────────────────────────────────────────
static CapPadHandler   g_pads;
static AdcHandler      g_adc;
static InputRouter     g_router;
static Sequencer       g_seq;
static ClockIn         g_clock_in;
static ClockOut        g_clock_out;
static float           g_int_bpm = 120.0f;  // BPM interno antes de sync EXT
static UartMidi        g_midi;
static MidiRouter      g_midi_router;
static LedController   g_leds;

static repeating_timer_t g_pad_timer;
static repeating_timer_t g_adc_timer;

static bool pad_timer_cb(repeating_timer_t*) { g_pads.scan(); return true; }
static bool adc_timer_cb(repeating_timer_t*) { g_adc.poll();  return true; }

// ── Flash save: SHIFT + REC + PLAY ───────────────────────────
// Gesto: mantener SHIFT y REC, y luego presionar PLAY para guardar a flash.
// Esto evita conflicto con la capa 2 de pots (SHIFT+REC held) y reemplaza
// el viejo triple-tap de REC.
static bool save_combo_prev_ = false;

static void check_flash_save(bool shift, bool rec, bool play) {
    const bool save_combo = shift && rec && play;
    const bool save_combo_just_pressed = save_combo && !save_combo_prev_;
    save_combo_prev_ = save_combo;

    if (save_combo_just_pressed) {
        bool ok = g_state.flash_save();
        LOG("MAIN: flash save (SHIFT+REC+PLAY) %s", ok ? "OK" : "FAIL");
        if (ok) g_leds.on_flash_save();
    }
}

// ── Core1 entry point ─────────────────────────────────────────
void core1_main() {
    // Pads capacitivos
    // Cambiar a Preset::STAGE() para entornos con humedad alta
    g_pads.init(CapPadHandler::Preset::DRY());
    g_pads.calibrate();  // ~1s — no tocar pads durante calibracion

    // Heartbeat: init pin GP22 como salida de prueba
#ifdef NO_PAD_HARDWARE
    gpio_init(HEARTBEAT_PIN);
    gpio_set_dir(HEARTBEAT_PIN, GPIO_OUT);
    gpio_put(HEARTBEAT_PIN, 0);
#endif

    g_adc.init();
    g_clock_in.init();
    g_clock_out.init();
    g_leds.init();
    g_seq.set_clock_out(&g_clock_out);
    g_seq.init();

    // Aftertouch: elegir opcion
    //   STUTTER_DEPTH = PAD_STUTTER controla stutter depth + rate
    //   DSP_DRIVE     = cualquier pad controla drive del clipper
    //   MACRO_MORPH   = pads 0-7 morphean macro momentaneamente
    g_router.aftertouch_mode = AftertouchMode::STUTTER_DEPTH;
    g_router.midi     = &g_midi;   // V1.7: Note Mode MIDI output
    g_router.led_ctrl = &g_leds;   // LED feedback

    // Timers: 5ms para pads (scan RC capacitivo), 2ms para ADC
    add_repeating_timer_ms(-5, pad_timer_cb, nullptr, &g_pad_timer);
    add_repeating_timer_ms(-2, adc_timer_cb, nullptr, &g_adc_timer);

    // MIDI: configuracion por defecto
    MidiConfig midi_cfg = {};
    midi_cfg.rx_channel       = 1;     // escuchar canal 1 (0 = omni)
    midi_cfg.tx_channel       = 1;
    // CC IN: pots 0-5 controlables por CC
    midi_cfg.cc_map[0]        = 74;    // CC74 -> pot 0 (Macro by default)
    midi_cfg.cc_map[1]        = 71;    // CC71 -> pot 1
    midi_cfg.cc_map[2]        = 72;    // CC72 -> pot 2
    midi_cfg.cc_map[3]        = 73;    // CC73 -> pot 3
    midi_cfg.cc_map[4]        = 75;    // CC75 -> pot 4
    // Note IN: notas C2-G2 disparan snapshots A-H
    for (uint8_t i = 0; i < 8; i++)
        midi_cfg.note_map[i]  = 36 + i;  // C2=36..G2=43
    // CC OUT: reflejar pots 0-5 como CC
    midi_cfg.cc_out_enable    = true;
    for (uint8_t i = 0; i < 6; i++)
        midi_cfg.cc_out_map[i] = midi_cfg.cc_map[i];
    // Clock I/O
    midi_cfg.clock_out_enable = true;
    midi_cfg.clock_in_enable  = true;

    g_midi.init(midi_cfg);
    g_midi_router.init(&g_midi, midi_cfg);

    ClockSource last_src      = ClockSource::INT;
    uint32_t    last_seq_tick = 0;

    while (true) {
        uint64_t now_us = time_us_64();
        uint32_t now_ms = to_ms_since_boot(get_absolute_time());

        // Clock In — auto-detect EXT/INT
        g_clock_in.update();
        ClockSource desired = g_clock_in.is_ext_sync()
                              ? ClockSource::EXT : ClockSource::INT;
        if (desired != last_src) {
            g_seq.set_clock_source(desired);
            last_src = desired;
            if (desired == ClockSource::INT) {
                // Volvemos a clock interno: restaurar el BPM que tenía el
                // usuario antes de que entrara la señal externa. Sin esto,
                // el secuenciador quedaría corriendo al último BPM EXT.
                g_seq.set_bpm(g_int_bpm);
                LOG("MAIN: clock -> INT | BPM restored=%.1f", g_int_bpm);
            } else {
                LOG("MAIN: clock -> EXT");
            }
        }
        if (desired == ClockSource::INT) {
            // Trackear el BPM interno para poder restaurarlo al salir de EXT.
            // Se actualiza solo cuando el encoder cambia el BPM (a través del
            // estado del sequencer), no en cada loop para no generar overhead.
            const float seq_bpm = g_seq.get_bpm();
            if (seq_bpm != g_int_bpm) g_int_bpm = seq_bpm;
        }
        if (desired == ClockSource::EXT) {
            while (g_clock_in.consume_tick()) {
                g_seq.set_bpm(g_clock_in.get_bpm());
                g_seq.on_ext_tick();
            }
        }

        g_seq.update_int(now_us);
        g_seq.tick(g_event_queue);
        g_clock_out.update();

        // MIDI clock out: emitir en cada tick del sequencer (24 PPQN)
        uint32_t cur_tick = g_seq.current_tick();
        if (cur_tick != last_seq_tick) {
            g_midi_router.on_clock_tick(g_midi);
            last_seq_tick = cur_tick;
        }

        // MIDI clock out: comparar tick actual con el ultimo enviado
        // Sequencer avanza en update_int(); comparar tick para emitir clock

        // Input: pads + pots -> eventos + cambios de estado
        g_router.process(g_pads, g_adc, g_seq, g_state, g_event_queue);

        // ── CV IN (CH6 del MUX) ───────────────────────────────────
        // Si hay señal CV presente, modula el Macro del snapshot activo.
        // Esto es aditivio: suma al valor actual del pot, clampeado a 1.0.
        // Comportamiento: CV corta → poco efecto; CV alta → macro al máximo.
        // El usuario puede deshabilitar el efecto poniendo el pot MACRO en 0.
        if (g_adc.cv_active()) {
            const float cv  = g_adc.get_cv();
            const float pot = g_adc.get(0);  // pot 0 = MACRO en capa normal
            const float combined = pot + cv * (1.0f - pot);  // aditivo con clamp implícito
            g_state.set_patch_param(PARAM_MACRO,
                combined > 1.0f ? 1.0f : combined);
        }

        // Flash save gesture + LED feedback
        bool shift_held = g_pads.is_pressed(PAD_SHIFT);
        bool rec_held   = g_pads.is_pressed(PAD_REC);
        bool play_held  = g_pads.is_pressed(PAD_PLAY);
        check_flash_save(shift_held, rec_held, play_held);

        // ── LED Controller ─────────────────────────────────────────
        // update() cada iteración del loop (~100µs → ~10kHz, pero
        // flush() al PIO solo ocurre cuando dirty_=true, así que
        // el overhead real es mínimo).
        {
            PlayState ps = g_seq.play_state();
            bool playing = (ps == PlayState::PLAYING || ps == PlayState::RECORDING);
            bool is_rec  = (ps == PlayState::RECORDING);
            const bool seq_view = g_seq.has_sequence() || g_seq.is_step_write_mode();
            const uint8_t page_base = g_seq.visible_page_base();
            uint8_t page_snap_mask = 0;
            uint8_t page_note_mask = 0;
            uint8_t page_drum_mask = 0;
            uint8_t page_motion_mask = 0;
            for (uint8_t i = 0; i < 8; ++i) {
                const uint8_t step = (uint8_t)(page_base + i);
                if (g_seq.step_has_snapshot(step)) page_snap_mask |= (1u << i);
                if (g_seq.step_has_note(step))     page_note_mask |= (1u << i);
                if (g_seq.step_has_drum(step))     page_drum_mask |= (1u << i);
                if (g_seq.step_has_motion(step))   page_motion_mask |= (1u << i);
            }
            const uint8_t playhead_step = g_seq.is_step_write_mode()
                                            ? g_seq.current_write_step_index()
                                            : g_seq.last_emitted_step_index();
            g_leds.update(g_seq.current_tick(),
                          g_state.get_active_slot(),
                          playing,
                          is_rec,
                          shift_held,
                          g_state.is_note_mode(),
                          g_state.get_env_loop(),
                          seq_view,
                          g_seq.sequence_length(),
                          page_base,
                          playhead_step,
                          g_seq.current_write_step_index(),
                          page_snap_mask,
                          page_note_mask,
                          page_drum_mask,
                          page_motion_mask,
                          g_seq.is_step_write_mode(),
                          g_seq.is_armed_record(),
                          g_seq.preroll_steps_left());
        }

        // MIDI: poll RX y traducir eventos entrantes
        g_midi.poll_rx();
        g_midi_router.process_in(g_midi, g_state, g_seq, g_event_queue);

        // Heartbeat 1Hz: conmutar GP22 cada 500ms
#ifdef NO_PAD_HARDWARE
        if ((now_ms % 1000) < 500)
            gpio_put(HEARTBEAT_PIN, 1);
        else
            gpio_put(HEARTBEAT_PIN, 0);
#endif

        // Sleep adaptativo: duerme lo que queda de un slot de 500µs.
        // Garantiza que el loop corre al menos a 2kHz sin quemar CPU,
        // pero si una iteración tardó más de 500µs no duerme nada.
        // Reduce la latencia del encoder respecto al sleep fijo de 100µs
        // porque la iteración completa no excede ese slot salvo bajo carga.
        {
            static uint64_t loop_start_us = 0;
            const uint64_t  elapsed = time_us_64() - loop_start_us;
            if (elapsed < 500u) sleep_us(500u - elapsed);
            loop_start_us = time_us_64();
        }
    }
}

// ── Core0 entry point ─────────────────────────────────────────
int main() {
    set_sys_clock_khz(125000, true);
    stdio_init_all();
    sleep_ms(500);
    LOG("=== Bytebeat Machine V1.14 ===");

    g_state.init();
    LOG("MAIN: state init OK");

    g_audio_engine.init(&g_audio_out, &g_state);
    g_audio_engine.set_event_queue(&g_event_queue);

    // Core0 debe registrarse como lockout victim para que
    // FlashStore::save() pueda pausarlo durante escritura Flash
    multicore_lockout_victim_init();
    multicore_launch_core1(core1_main);

    LOG("Core0: audio @ %u Hz, BLOCK_SIZE=%u",
        AudioEngine::SAMPLE_RATE, AudioEngine::BLOCK_SIZE);
    g_audio_engine.run();  // nunca retorna
    return 0;
}
