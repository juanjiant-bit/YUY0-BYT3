#include "../synth/quantizer.h"
#include "input_router.h"
#include "../led/led_controller.h"
#include "../synth/note_mode.h"
#include "../utils/debug_log.h"
#include "pico/stdlib.h"
#include "../hardware/pin_config.h"
#include <cmath>

void InputRouter::process(CapPadHandler& pads, AdcHandler& adc,
                          Sequencer& seq, StateManager& state,
                          RingBuffer<SequencerEvent, 128>& queue)
{
    uint16_t just_on  = pads.get_just_pressed();
    uint16_t just_off = pads.get_just_released();
    bool     shift    = pads.is_pressed(PAD_SHIFT);

    if (seq.is_step_write_mode() && (just_on & (1u << PAD_SHIFT))) {
        shift_used_for_action_ = false;
        const uint32_t now_ms_shift = to_ms_since_boot(get_absolute_time());
        if (shift_waiting_second_tap_ && (now_ms_shift - last_shift_tap_ms_) <= 320u) {
            seq.clear_current_step();
            shift_used_for_action_ = true;
            shift_waiting_second_tap_ = false;
        } else {
            shift_waiting_second_tap_ = true;
            last_shift_tap_ms_ = now_ms_shift;
        }
    }
    const bool edit_sequence = seq.is_step_write_mode() || seq.is_overdub();
    const bool paste_combo = edit_sequence && shift && pads.is_pressed(PAD_PLAY) && !pads.is_pressed(PAD_REC);
    const bool clear_snap_combo = edit_sequence && shift && !pads.is_pressed(PAD_REC) && !pads.is_pressed(PAD_PLAY);

// SHIFT + SNAP punch FX / sequence edit
if (shift) {
    static const uint8_t snap_pad_phys[8] = {3,4,7,8,9,12,13,14};
    const uint32_t now_ms_snap = to_ms_since_boot(get_absolute_time());

    bool any_snap_just_on = false;
    bool any_snap_just_off = false;
    for (uint8_t slot = 0; slot < 8; ++slot) {
        const uint8_t pad = snap_pad_phys[slot];
        if (just_on & (1u << pad)) {
            any_snap_just_on = true;
            snap_press_ms_[slot] = now_ms_snap;
            snap_saved_[slot] = false;
        }
        if (just_off & (1u << pad)) {
            any_snap_just_off = true;
        }
    }

    bool hold_action_fired = false;
    if (edit_sequence && !note_mode_) {
        for (uint8_t logical = 0; logical < 8; ++logical) {
            const uint8_t pad = snap_pad_phys[logical];
            if (!pads.is_pressed(pad) || snap_saved_[logical]) continue;
            if ((now_ms_snap - snap_press_ms_[logical]) < SNAP_HOLD_ACTION_MS) continue;

            if (paste_combo) {
                seq.paste_page_to_current_page();
                snap_saved_[logical] = true;
                hold_action_fired = true;
                shift_used_for_action_ = true;
            } else if (clear_snap_combo) {
                seq.copy_current_page();
                snap_saved_[logical] = true;
                hold_action_fired = true;
                shift_used_for_action_ = true;
            }
        }
    }

    if (edit_sequence && note_mode_ && pads.is_pressed(PAD_MUTE) && any_snap_just_on) {
        for (uint8_t logical = 0; logical < 8; ++logical) {
            const uint8_t pad = snap_pad_phys[logical];
            if (just_on & (1u << pad)) {
                seq.clear_current_note(logical);
                shift_used_for_action_ = true;
            }
        }
    } else if (edit_sequence && note_mode_ && clear_snap_combo && any_snap_just_on) {
        for (uint8_t logical = 0; logical < 8; ++logical) {
            const uint8_t pad = snap_pad_phys[logical];
            if (just_on & (1u << pad)) {
                seq.toggle_current_note_tie(logical);
                shift_used_for_action_ = true;
            }
        }
    } else if (edit_sequence && !note_mode_ && clear_snap_combo && any_snap_just_off && !hold_action_fired) {
        bool clear_on_release = false;
        for (uint8_t logical = 0; logical < 8; ++logical) {
            const uint8_t pad = snap_pad_phys[logical];
            if (just_off & (1u << pad)) {
                clear_on_release = !snap_saved_[logical];
                snap_saved_[logical] = false;
                snap_press_ms_[logical] = 0;
            }
        }
        if (clear_on_release) {
            seq.clear_current_snapshot_layer();
            shift_used_for_action_ = true;
        }
    } else if (!edit_sequence || note_mode_ || (!hold_action_fired && !clear_snap_combo)) {
        for(uint8_t slot=0; slot<7; ++slot){  // slots 0..6 = punch FX
            uint8_t pad = snap_pad_phys[slot];

            bool on  = (just_on  & (1<<pad));
            bool off = (just_off & (1<<pad));

            if(on){
                queue.push({seq.current_tick(), EVT_FX_ON, (uint8_t)(slot + 1), 1.0f});
                seq.record_fx((uint8_t)(slot + 1), true);
                active_punch_fx_ |= (1u << slot);   // trackear para LEDs
                if (led_ctrl) led_ctrl->set_punch_fx_mask(active_punch_fx_);
                shift_used_for_action_ = true;
            }
            if(off){
                queue.push({seq.current_tick(), EVT_FX_OFF, (uint8_t)(slot + 1), 0.0f});
                seq.record_fx((uint8_t)(slot + 1), false);
                active_punch_fx_ &= ~(1u << slot);  // apagar bit
                if (led_ctrl) led_ctrl->set_punch_fx_mask(active_punch_fx_);
                shift_used_for_action_ = true;
            }
        }
    }
}

    handle_encoder(pads, seq, state);


// SHIFT + SNAP8 = snapshot arp momentáneo
if (shift && !paste_combo && !clear_snap_combo) {
    const uint8_t arp_pad = SNAP_PAD_PHYS[7];
    bool arp_on  = (just_on  & (1u << arp_pad)) != 0;
    bool arp_off = (just_off & (1u << arp_pad)) != 0;

    if (arp_on) {
        snap_arp_active_  = true;
        snap_arp_step_    = 0;
        snap_arp_next_ms_ = to_ms_since_boot(get_absolute_time());
        seq.record_arp();
        shift_used_for_action_ = true;
    }
    if (arp_off) {
        snap_arp_active_ = false;
    }
} else {
    snap_arp_active_ = false;
}
    last_shift_ = shift;

    const bool random_combo = shift && pads.is_pressed(PAD_KICK) && pads.is_pressed(PAD_SNARE);
    uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    if (random_combo && !random_combo_active_) {
        random_combo_active_ = true;
        random_wild_fired_   = false;
        random_combo_ms_     = now_ms;
    }

    // ── Protección randomize accidental ──────────────────────────
    // CONTROLLED solo dispara si la combo se mantiene >= RANDOM_GUARD_MS (400ms).
    // Esto previene disparos por roces accidentales sobre KICK+SNARE.
    // WILD mantiene el threshold en 1000ms (comportamiento anterior).
    if (random_combo_active_ && random_combo && !random_wild_fired_) {
        const uint32_t held = now_ms - random_combo_ms_;
        if (held >= 1000u) {
            state.randomize_all(RandomizeMode::WILD);
            seq.start_random_chain(RandomizeMode::WILD);
            random_wild_fired_ = true;
        }
    }
    if (random_combo_active_ && !random_combo) {
        if (!random_wild_fired_) {
            const uint32_t held = now_ms - random_combo_ms_;
            if (held >= RANDOM_GUARD_MS) {
                // Combo mantenida el tiempo mínimo → CONTROLLED
                state.randomize_all(RandomizeMode::CONTROLLED);
                seq.start_random_chain(RandomizeMode::CONTROLLED);
            }
            // Si held < RANDOM_GUARD_MS: toque demasiado corto, no hacer nada.
        }
        random_combo_active_ = false;
        random_wild_fired_   = false;
    }

    handle_transport(pads, seq, state, shift, just_on, queue);

    if (shift && (just_on & (1u << PAD_MUTE))) {
        if (seq.play_state() == PlayState::PLAYING && !seq.is_step_write_mode()) {
            seq.panic_restore(queue);
        } else {
            state.set_env_loop(!state.get_env_loop());
            LOG_AUDIO("INPUT: Env Loop %s", state.get_env_loop() ? "ON" : "OFF");
        }
        shift_used_for_action_ = true;
    }

    if (note_mode_) {
        handle_note_pads(pads, seq, state, queue, just_on, just_off);
        handle_drums(pads, seq, state, queue);
    } else {
        handle_snapshots(pads, seq, state, queue, just_on);
        handle_drums(pads, seq, state, queue);
    }


// snapshot arp runtime
if (snap_arp_active_) {
    uint32_t now_ms_arp = to_ms_since_boot(get_absolute_time());
    if (now_ms_arp >= snap_arp_next_ms_) {
        snap_arp_next_ms_ = now_ms_arp + 90;  // ~11Hz
        uint8_t slot = (snap_arp_step_ & 0x07u);
        snap_arp_step_++;

        if (!state.get_mute_snap(slot)) {
            state.request_trigger(slot);
            queue.push({seq.current_tick(), EVT_PAD_TRIGGER, slot, 1.0f});
        }
    }
}
    handle_adc(pads, adc, seq, state, queue);

    if (seq.is_step_write_mode() && (just_off & (1u << PAD_SHIFT)) && !shift_used_for_action_) {
        seq.on_manual_advance();
    }

    handle_aftertouch(pads, state, queue, seq);
}

void InputRouter::handle_transport(CapPadHandler& pads, Sequencer& seq,
                                   StateManager& state, bool shift,
                                   uint16_t just_on,
                                   RingBuffer<SequencerEvent, 128>& queue)
{
    if (just_on & (1u << PAD_PLAY)) {
        const bool save_combo = shift && pads.is_pressed(PAD_REC);
        const bool arm_combo  = !shift && pads.is_pressed(PAD_REC);

        if (save_combo) {
            // Reservado para SAVE (SHIFT+REC+PLAY) en main.cpp
        } else if (arm_combo) {
            seq.arm_record();
        } else if (shift && seq.is_step_write_mode() && !pads.is_pressed(SNAP_PAD_PHYS[0]) && !pads.is_pressed(SNAP_PAD_PHYS[1]) && !pads.is_pressed(SNAP_PAD_PHYS[2]) && !pads.is_pressed(SNAP_PAD_PHYS[3]) && !pads.is_pressed(SNAP_PAD_PHYS[4]) && !pads.is_pressed(SNAP_PAD_PHYS[5]) && !pads.is_pressed(SNAP_PAD_PHYS[6]) && !pads.is_pressed(SNAP_PAD_PHYS[7])) {
            seq.duplicate_previous_step_into_next();
            shift_used_for_action_ = true;
        } else if (shift) {
            note_mode_ = !note_mode_;
            state.set_note_mode(note_mode_);
            if (!note_mode_) {
                for (uint8_t logical = 0; logical < NOTE_PAD_COUNT; logical++) {
                    if (note_active_[logical]) {
                        queue.push({seq.current_tick(), EVT_NOTE_OFF,
                                    active_note_midi_[logical], 0.0f});
                        if (midi) midi->send_note_off(active_note_midi_[logical]);
                        note_active_[logical] = false;
                        active_note_midi_[logical] = 0;
                    }
                }
                state.clear_note_pitch();
            }
            shift_used_for_action_ = true;
            LOG_AUDIO("INPUT: Note Mode %s", note_mode_ ? "ON" : "OFF");
            if (led_ctrl) led_ctrl->on_note_mode_toggle(note_mode_);
        } else {
            if (seq.play_state() == PlayState::STOPPED) seq.play();
            else seq.stop();
        }
    }

    if (just_on & (1u << PAD_REC)) {
        if (!shift && pads.is_pressed(PAD_PLAY)) {
            seq.arm_record();
        } else if (!shift) {
            seq.rec_toggle();
        }
    }

    // SHIFT + REC held = segunda capa de pots.
    // Se mantiene mientras ambos pads estén presionados.
    shift_rec_mode_ = shift && pads.is_pressed(PAD_REC);
}

void InputRouter::handle_snapshots(CapPadHandler& pads, Sequencer& seq,
                                   StateManager& state, RingBuffer<SequencerEvent, 128>& queue,
                                   uint16_t just_on)
{
    // V1.14 block7+: SHIFT + SNAP se maneja antes en process() para FX / edición.
    if (pads.is_pressed(PAD_SHIFT)) return;
    for (uint8_t slot = 0; slot < NOTE_PAD_COUNT; ++slot) {
        const uint8_t pad = SNAP_PAD_PHYS[slot];
        const bool newly_pressed = (just_on & (1u << pad)) != 0;

        if (newly_pressed) {
            state.request_trigger(slot);
            queue.push({seq.current_tick(), EVT_PAD_TRIGGER, slot, 1.0f});
            seq.record_snapshot(slot);
            if (led_ctrl) led_ctrl->on_snapshot_trigger(slot);
        }

        // Mantener el estado limpio por si quedaron flags de una implementación previa.
        if (!pads.is_pressed(pad)) {
            snap_saved_[slot] = false;
            snap_press_ms_[slot] = 0;
        }
    }
}

void InputRouter::handle_note_pads(CapPadHandler& pads, Sequencer& seq,
                                   StateManager& state,
                                   RingBuffer<SequencerEvent, 128>& queue,
                                   uint16_t just_on, uint16_t just_off)
{
    if (pads.is_pressed(PAD_SHIFT)) return;
    ScaleId scale = (ScaleId)state.get_scale_id();
    uint8_t root  = state.get_root();

    for (uint8_t logical = 0; logical < NOTE_PAD_COUNT; logical++) {
        const uint8_t pad = SNAP_PAD_PHYS[logical];
        const bool pressed  = ((just_on  >> pad) & 1u) != 0;
        const bool released = ((just_off >> pad) & 1u) != 0;

        if (pressed) {
            const uint8_t note = NoteMode::pad_to_midi(logical, scale, root);
            const float   vel  = 0.75f + pads.get_pressure(pad) * 0.25f;
            const float   ratio = NoteMode::midi_to_pitch_ratio(note);

            active_note_midi_[logical] = note;
            note_active_[logical]      = true;

            state.set_note_pitch(ratio);
            queue.push({seq.current_tick(), EVT_NOTE_ON, note, vel});
            seq.record_note(logical, note, vel);
            if (midi) midi->send_note_on(note, (uint8_t)(vel * 127.0f));

            LOG_AUDIO("NOTE ON pad=%u logical=%u note=%u(%s) ratio=%.3f",
                      pad, logical, note, NoteMode::midi_note_name(note), (double)ratio);
        }

        if (released && note_active_[logical]) {
            const uint8_t note = active_note_midi_[logical];
            note_active_[logical] = false;

            bool any_active = false;
            for (uint8_t p = 0; p < NOTE_PAD_COUNT; p++) {
                if (note_active_[p]) {
                    any_active = true;
                    break;
                }
            }
            if (!any_active) {
                state.clear_note_pitch();
            }

            queue.push({seq.current_tick(), EVT_NOTE_OFF, note, 0.0f});
            if (midi) midi->send_note_off(note);
        }
    }
}

void InputRouter::handle_drums(CapPadHandler& pads, Sequencer& seq,
                               StateManager& state,
                               RingBuffer<SequencerEvent, 128>& queue)
{
    struct DrumMap { uint8_t pad; DrumId id; };
    static constexpr DrumMap drums[] = {
        {PAD_KICK, DRUM_KICK},
        {PAD_SNARE, DRUM_SNARE},
        {PAD_HAT, DRUM_HAT}
    };

    bool* mute_flags[3] = { nullptr, nullptr, nullptr };  // reemplazado por state.get/toggle_mute_drum(i)

    const bool random_combo = pads.is_pressed(PAD_SHIFT) &&
                              pads.is_pressed(PAD_KICK) &&
                              pads.is_pressed(PAD_SNARE);
    const bool mute_held = pads.is_pressed(PAD_MUTE) && !pads.is_pressed(PAD_SHIFT);
    const bool clear_drum_layer = pads.is_pressed(PAD_SHIFT) && (seq.is_step_write_mode() || seq.is_overdub()) &&
                                  !random_combo && !mute_held && !pads.is_pressed(PAD_PLAY) && !pads.is_pressed(PAD_REC);

    uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    for (uint8_t i = 0; i < 3; i++) {
        uint8_t pad   = drums[i].pad;
        DrumId  id    = drums[i].id;
        bool just_on  = pads.just_pressed(pad);
        bool just_off = pads.just_released(pad);
        bool pressed  = pads.is_pressed(pad);

        if (clear_drum_layer && just_on) {
            seq.clear_current_drum_layer();
            shift_used_for_action_ = true;
            continue;
        }

        if (mute_held && just_on) {
            state.toggle_mute_drum(i);
            drum_rolling_[i] = false;
            continue;
        }

        if (random_combo && (pad == PAD_KICK || pad == PAD_SNARE)) {
            drum_rolling_[i] = false;
            continue;
        }

        if (just_on) {
            drum_press_ms_[i] = now_ms;
            drum_rolling_[i] = false;
        }

        if (pressed && !drum_rolling_[i]) {
            if (now_ms - drum_press_ms_[i] >= ROLL_THRESHOLD_MS) {
                drum_rolling_[i] = true;
                if (!state.get_mute_drum(i)) {
                    queue.push({seq.current_tick(), EVT_DRUM_HIT, (uint8_t)id, 1.0f});
                    seq.record_drum(id);
                    queue.push({seq.current_tick(), EVT_ROLL_ON,  (uint8_t)id, 1.0f});
                }
            }
        }

        if (just_off) {
            if (drum_rolling_[i]) {
                drum_rolling_[i] = false;
                if (!state.get_mute_drum(i)) {
                    queue.push({seq.current_tick(), EVT_ROLL_OFF, (uint8_t)id, 0.0f});
                }
            } else {
                if (!state.get_mute_drum(i)) {
                    queue.push({seq.current_tick(), EVT_DRUM_HIT, (uint8_t)id, 1.0f});
                    seq.record_drum(id);
                }
            }
        }
    }
}

bool InputRouter::is_drum_bus_param(ParamId id) const
{
    return id == PARAM_DRUM_DECAY || id == PARAM_DRUM_COLOR || id == PARAM_DUCK_AMOUNT;
}

ParamId InputRouter::resolve_pot_param(uint8_t pot_index, bool shift, bool shift_rec) const
{
    static constexpr ParamId kBaseMap[6] = {
        PARAM_MACRO,
        PARAM_TONAL,
        PARAM_SPREAD,
        PARAM_DRIVE,
        PARAM_TIME_DIV,
        PARAM_SNAP_GATE,
    };
    static constexpr ParamId kShiftMap[6] = {
        PARAM_GLIDE,
        PARAM_ENV_ATTACK,
        PARAM_ENV_RELEASE,
        PARAM_STUTTER_RATE,
        PARAM_GRAIN,
        PARAM_HP,
    };
    static constexpr ParamId kShiftRecMap[6] = {
        PARAM_REVERB_ROOM,
        PARAM_REVERB_WET,
        PARAM_CHORUS,
        PARAM_DRUM_DECAY,
        PARAM_DRUM_COLOR,
        PARAM_DUCK_AMOUNT,
    };

    if (pot_index >= 6) return PARAM_MACRO;
    if (shift_rec) return kShiftRecMap[pot_index];
    if (shift)     return kShiftMap[pot_index];
    return kBaseMap[pot_index];
}

void InputRouter::handle_adc(CapPadHandler& pads, AdcHandler& adc, Sequencer& seq,
                             StateManager& state,
                             RingBuffer<SequencerEvent, 128>& queue)
{
    const bool shift     = last_shift_;
    const bool shift_rec = shift && shift_rec_mode_;
    const bool clear_param_mode = shift && !shift_rec && (seq.is_step_write_mode() || seq.is_overdub());

    // ── Soft takeover: detectar cambio de capa ───────────────────
    // 0 = normal, 1 = shift, 2 = shift+rec
    const uint8_t cur_layer = shift_rec ? 2u : (shift ? 1u : 0u);
    if (cur_layer != last_layer_) {
        // La capa cambió: congelar el valor virtual actual y poner todos
        // los pots en CATCHING. Al cruzar la posición física el valor
        // virtual, el pot vuelve a TRACKING y empieza a controlar.
        for (uint8_t i = 0; i < AdcHandler::NUM_POTS; i++) {
            pot_state_[i]     = PotState::CATCHING;
            pot_catch_val_[i] = last_pot_[i];
        }
        last_layer_ = cur_layer;
    }

    for (uint8_t i = 0; i < AdcHandler::NUM_POTS; i++) {
        float val = adc.get(i);

        // ── Soft takeover: lógica de catch ───────────────────────
        if (pot_state_[i] == PotState::CATCHING) {
            const float catch_v = pot_catch_val_[i];
            const bool  crossed = (last_pot_[i] <= catch_v && val >= catch_v) ||
                                  (last_pot_[i] >= catch_v && val <= catch_v) ||
                                  (fabsf(val - catch_v) < 0.015f);
            if (crossed) {
                pot_state_[i] = PotState::TRACKING;
                last_pot_[i]  = val;
            } else {
                last_pot_[i] = val;
                continue;
            }
        }

        // ── Hysteresis normal ────────────────────────────────────
        if (fabsf(val - last_pot_[i]) <= 0.01f) continue;

        const ParamId param = resolve_pot_param(i, shift, shift_rec);
        EventType event_type = EVT_PARAM_CHANGE;
        uint8_t event_target = static_cast<uint8_t>(param);

        if (clear_param_mode && pads.is_pressed(PAD_MUTE)) {
            seq.clear_current_param_lock(param);
            shift_used_for_action_ = true;
            last_pot_[i] = val;
            continue;
        }

        if (shift_rec) {
            state.set_bus_param(param, val);
            if (is_drum_bus_param(param)) {
                event_type = EVT_DRUM_PARAM;
                switch (param) {
                case PARAM_DRUM_DECAY: event_target = DRUM_PARAM_DECAY; break;
                case PARAM_DRUM_COLOR: event_target = DRUM_PARAM_COLOR; break;
                case PARAM_DUCK_AMOUNT: event_target = DRUM_PARAM_DUCK; break;
                default: break;
                }
            }
        } else {
            state.set_patch_param(param, val);
        }

        queue.push({seq.current_tick(), event_type, event_target, val});
        seq.record_param(param, val);
        if (shift) shift_used_for_action_ = true;
        last_pot_[i] = val;
    }
}

void InputRouter::handle_encoder(CapPadHandler& pads, Sequencer& seq, StateManager& state)
{
    if (!encoder_ready_) {
        encoder_.init(ENC_A_PIN, ENC_B_PIN, ENC_SW_PIN);
        encoder_ready_ = true;
    }

    encoder_.update();

    const bool shift_held = pads.is_pressed(PAD_SHIFT);
    const bool rec_held   = pads.is_pressed(PAD_REC);
    const EncoderMode mode_now = state.get_encoder_mode();
    const bool seq_edit = seq.is_step_write_mode() || seq.is_overdub();
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());

    // ── HOME: hold del encoder SW ────────────────────────────────
    // 700ms  → SOFT  (encoder→BPM, bus params → snapshot activo)
    // 1500ms → FULL  (+ mutes + drum params)
    // Feedback visual progresivo en LEDs durante el hold.
    if (encoder_.is_pressed()) {
        // Calcular progreso normalizado entre 0 y HOME_FULL_MS
        // (el encoder trackea el inicio de presión internamente)
        // Obtenemos la duración desde el último flanco de bajada
        // reusando sw_press_ms_ vía un getter auxiliar.
        // Como no tenemos getter directo, estimamos: si is_pressed()
        // es true y el hold aún no se disparó, usamos el tiempo desde
        // que encoder_.update() lo detectó. Simplificación conservadora:
        // usamos un timer local aquí mismo.
        if (!encoder_hold_started_) {
            encoder_hold_start_ms_ = now_ms;
            encoder_hold_started_  = true;
            home_full_fired_       = false;
        }
        const uint32_t held_ms = now_ms - encoder_hold_start_ms_;
        const float progress = (float)held_ms / (float)HOME_FULL_MS;

        // Nivel visual: 0=cargando, 1=SOFT alcanzado, 2=FULL alcanzado
        const uint8_t vis_level = (held_ms >= HOME_FULL_MS) ? 2u
                                : (held_ms >= 700u)         ? 1u
                                                            : 0u;
        if (led_ctrl) led_ctrl->on_home_progress(
            progress > 1.0f ? 1.0f : progress, vis_level);
    } else {
        encoder_hold_started_ = false;
    }

    if (encoder_.read_hold()) {
        // Hold corto (700ms): SOFT reset
        state.home_reset(StateManager::HomeLevel::SOFT);
        // Sincronizar BPM al secuenciador
        seq.set_bpm((float)state.get_encoder_state().bpm);
        if (led_ctrl) {
            led_ctrl->on_home_progress(0.0f, 0);
            led_ctrl->show_encoder_state(state.get_encoder_state());
        }
        home_full_fired_ = false;
    }

    // FULL reset: hold supera HOME_FULL_MS (detectado manualmente)
    if (encoder_hold_started_ && !home_full_fired_) {
        const uint32_t held_ms = now_ms - encoder_hold_start_ms_;
        if (held_ms >= HOME_FULL_MS) {
            state.home_reset(StateManager::HomeLevel::FULL);
            seq.set_bpm((float)state.get_encoder_state().bpm);
            home_full_fired_ = true;
            if (led_ctrl) led_ctrl->on_home_progress(0.0f, 0);
        }
    }

    if (encoder_.read_click()) {
        if (seq_edit && shift_held && !rec_held && mode_now == EncoderMode::MUTATE) {
            seq.reset_current_step_chance();
            if (led_ctrl) {
                EncoderState fake = state.get_encoder_state();
                fake.mode = EncoderMode::MUTATE;
                fake.mutate_amount = seq.current_step_chance();
                led_ctrl->show_encoder_state(fake);
            }
        } else if (mode_now == EncoderMode::MUTATE && rec_held) {
            const float amount = state.get_mutate_amount();
            state.mutate_active_snapshot(amount, shift_held);
            seq.mutate_generative_sequence(amount, shift_held);
        } else {
            state.next_encoder_mode();
            if (led_ctrl) led_ctrl->show_encoder_state(state.get_encoder_state());
        }
    }

    int delta = encoder_.read_delta();
    if (delta != 0) {
        if (seq_edit && shift_held && !rec_held && state.get_encoder_mode() == EncoderMode::MUTATE) {
            seq.adjust_current_step_chance(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) {
                EncoderState fake = state.get_encoder_state();
                fake.mode = EncoderMode::MUTATE;
                fake.mutate_amount = seq.current_step_chance();
                led_ctrl->show_encoder_state(fake);
            }
        } else if (state.get_encoder_mode() == EncoderMode::SWING && shift_held && !rec_held) {
            seq.adjust_groove_template(delta);
            shift_used_for_action_ = true;
            if (led_ctrl) {
                EncoderState fake = state.get_encoder_state();
                fake.mode = static_cast<EncoderMode>(static_cast<uint8_t>(seq.groove_template()) % static_cast<uint8_t>(EncoderMode::COUNT));
                fake.swing_amount = seq.get_swing();
                led_ctrl->show_encoder_state(fake);
            }
        } else {
            state.encoder_delta(delta, shift_held);

            const EncoderMode mode_after = state.get_encoder_mode();
            const EncoderState& enc = state.get_encoder_state();
            if (mode_after == EncoderMode::BPM) {
                seq.set_bpm((float)enc.bpm);
            } else if (mode_after == EncoderMode::SWING) {
                seq.set_swing(enc.swing_amount);
            }
            if (led_ctrl) led_ctrl->show_encoder_state(enc);
        }
    }
}

void InputRouter::handle_aftertouch(CapPadHandler& pads, StateManager& state,
                                    RingBuffer<SequencerEvent, 128>& queue,
                                    Sequencer& seq)
{
    static constexpr float DEADZONE  = 0.02f;
    static constexpr float HYST_EMIT = 0.015f;

    for (uint8_t phys = 0; phys < CapPadHandler::NUM_PADS; phys++) {
        float p = pads.get_pressure(phys);
        if (fabsf(p - last_pressure_[phys]) < HYST_EMIT) continue;
        last_pressure_[phys] = p;
        if (p < DEADZONE) p = 0.0f;

        const uint8_t logical = SNAP_SLOT_FROM_PHYS[phys];

        // ── Prioridad 1: Note Mode — velocity en tiempo real ─────
        // La presión modula la velocity de la nota activa en ese pad.
        if (note_mode_ && logical != INVALID_SLOT && note_active_[logical]) {
            queue.push({seq.current_tick(), EVT_AFTERTOUCH,
                        active_note_midi_[logical], p});
            continue;
        }

        // ── Prioridad 2: PAD_MUTE → grain freeze wet ─────────────
        // La palma sobre MUTE congela progresivamente el granulador.
        // Presión 0.0 → grain en valor base del pot (sin congelación adicional).
        // Presión 1.0 → grain wet al máximo → freeze total.
        // El audio engine blendea la presión con el valor base del pot.
        if (phys == PAD_MUTE) {
            queue.push({seq.current_tick(), EVT_AFTERTOUCH, PAD_MUTE, p});
            continue;
        }

        // ── Prioridad 3: SNAP pads → reverb wet momentáneo ───────
        // Cada pad SNAP abre el espacio del reverb mientras está presionado.
        // La presión es aditiva: se suma al base_reverb_wet_ del snapshot,
        // clampeada a 1.0 en el audio engine. Al soltar, vuelve al base.
        // Efecto: pad liviano = seco, pad fondo = reverb total.
        if (logical != INVALID_SLOT) {
            queue.push({seq.current_tick(), EVT_AFTERTOUCH,
                        AT_TARGET_REVERB_SNAP, p});
            continue;
        }

        // Pads de transport y drums: sin aftertouch por ahora.
        (void)state;
    }
}
