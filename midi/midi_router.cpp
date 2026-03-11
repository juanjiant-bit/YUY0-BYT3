// midi_router.cpp — Bytebeat Machine V1.14
#include "midi_router.h"
#include "pico/stdlib.h"
#include "../utils/debug_log.h"

void MidiRouter::init(UartMidi* midi, const MidiConfig& cfg) {
    cfg_ = cfg;
    for (uint8_t i = 0; i < 8; i++) last_note_[i] = 60 + i;
}

// ── MIDI IN -> acciones internas ─────────────────────────────
void MidiRouter::process_in(UartMidi& midi, StateManager& state,
                              Sequencer& seq,
                              RingBuffer<SequencerEvent, 128>& queue)
{
    // Clock IN
    if (cfg_.clock_in_enable) {
        while (midi.consume_clock_tick()) {
            update_clock_in(seq);
        }
        // Start/Stop via is_running
        // (el Sequencer ya maneja esto via ClockIn nativo,
        //  aqui lo manejamos solo si MIDI es el source activo)
    }

    // Eventos de nota / CC
    MidiEvent ev;
    while (midi.pop_event(ev)) {
        uint8_t type = ev.status & 0xF0;
        uint8_t ch   = (ev.status & 0x0F) + 1;

        // Filtro de canal (omni si rx_channel=0)
        if (cfg_.rx_channel != 0 && ch != cfg_.rx_channel) continue;

        switch (type) {

        case MIDI_NOTE_ON:
            if (ev.data2 > 0) {
                // Nota ON: mapear a snapshot si esta en note_map
                for (uint8_t s = 0; s < 8; s++) {
                    if (cfg_.note_map[s] == ev.data1) {
                        state.request_trigger(s);
                        queue.push({seq.current_tick(),
                                    EVT_PAD_TRIGGER, s, ev.data2 / 127.0f});
                        LOG("MIDI IN: note %u -> snapshot %u", ev.data1, s);
                        break;
                    }
                }
            }
            // Note OFF (velocity 0 = note off en running status)
            break;

        case MIDI_NOTE_OFF:
            // Futuro: gate off si se implementa modo legato
            break;

        case MIDI_CC: {
            const float val = ev.data2 / 127.0f;
            bool handled = false;

            // CC map base: refleja la capa normal de los 6 potes.
            for (uint8_t p = 0; p < 6; p++) {
                if (cfg_.cc_map[p] != 0 && cfg_.cc_map[p] == ev.data1) {
                    const ParamId param = static_cast<ParamId>(p);
                    state.set_patch_param(param, val);
                    queue.push({seq.current_tick(), EVT_PARAM_CHANGE,
                                static_cast<uint8_t>(param), val});
                    LOG("MIDI IN: CC%u -> param %u = %.2f", ev.data1, (unsigned)param, val);
                    handled = true;
                    break;
                }
            }

            if (handled) break;

            // CC especiales alineados al layout actual.
            switch (ev.data1) {
            case 74: // cutoff style CC -> HP filter
                state.set_patch_param(PARAM_HP, val);
                queue.push({seq.current_tick(), EVT_PARAM_CHANGE,
                            static_cast<uint8_t>(PARAM_HP), val});
                handled = true;
                break;
            case 91: // reverb send
                state.set_bus_param(PARAM_REVERB_WET, val);
                queue.push({seq.current_tick(), EVT_PARAM_CHANGE,
                            static_cast<uint8_t>(PARAM_REVERB_WET), val});
                handled = true;
                break;
            case 93: // chorus send
                state.set_bus_param(PARAM_CHORUS, val);
                queue.push({seq.current_tick(), EVT_PARAM_CHANGE,
                            static_cast<uint8_t>(PARAM_CHORUS), val});
                handled = true;
                break;
            case 1: // mod wheel -> stutter rate performático
                state.set_patch_param(PARAM_STUTTER_RATE, val);
                queue.push({seq.current_tick(), EVT_PARAM_CHANGE,
                            static_cast<uint8_t>(PARAM_STUTTER_RATE), val});
                handled = true;
                break;
            case 80: // toggle env loop
                if (ev.data2 >= 64) {
                    state.set_env_loop(!state.get_env_loop());
                    LOG("MIDI IN: CC80 -> env_loop %s", state.get_env_loop() ? "ON" : "OFF");
                }
                handled = true;
                break;
            default:
                break;
            }
            (void)handled;
            break;
        }

        case 0xF0:
            // Mensajes realtime (Start 0xFA, Stop 0xFC, Continue 0xFB)
            // Nota: en el parser de UartMidi estos llegan con ev.status completo
            // y data1/data2 = 0. El type = ev.status & 0xF0 = 0xF0 para todos.
            if (ev.status == MIDI_START) {
                // MIDI Start: equivalente a PLAY
                if (seq.play_state() == PlayState::STOPPED ||
                    seq.play_state() == PlayState::ARMED) {
                    seq.play();
                }
            } else if (ev.status == MIDI_STOP) {
                seq.stop();
            } else if (ev.status == MIDI_CONTINUE) {
                // Continue: reanudar desde posición actual
                if (seq.play_state() == PlayState::STOPPED) {
                    seq.play();
                }
            }
            break;
        default: break;
        }
    }
}

// ── Clock IN: calcular BPM ───────────────────────────────────
void MidiRouter::update_clock_in(Sequencer& seq) {
    if (!cfg_.clock_in_enable) return;
    uint32_t now = time_us_32();
    if (last_clock_us_ != 0) {
        uint32_t interval = now - last_clock_us_;
        // Suavizar intervalo con IIR
        if (clock_interval_ == 0) {
            clock_interval_ = interval;
        } else {
            clock_interval_ = (clock_interval_ * 7 + interval) >> 3;
        }
        // MIDI clock = 24 ticks/quarter note
        // BPM = 60e6 / (interval_us * 24)
        if (clock_interval_ > 0) {
            float bpm = 60000000.0f / ((float)clock_interval_ * 24.0f);
            if (bpm >= 20.0f && bpm <= 300.0f) {
                seq.set_bpm(bpm);
                seq.on_ext_tick();
            }
        }
    }
    last_clock_us_ = now;
}

// ── MIDI OUT ─────────────────────────────────────────────────
void MidiRouter::on_pot_change(UartMidi& midi, uint8_t pot_idx, float val) {
    if (!cfg_.cc_out_enable) return;
    midi.send_pot_cc(pot_idx, val);
}

void MidiRouter::on_snap_trigger(UartMidi& midi, uint8_t slot) {
    if (slot >= 8) return;
    // Note OFF solo del slot activo anterior (no de todos)
    if (note_active_ && active_slot_ < 8 && last_note_[active_slot_] != 0) {
        midi.send_note_off(last_note_[active_slot_]);
    }
    // Note ON para el nuevo slot (nota = 60 + slot = C4..G4)
    uint8_t note = 60 + slot;
    last_note_[slot] = note;
    active_slot_     = slot;
    midi.send_note_on(note, 100);
    note_active_ = true;
    LOG("MIDI OUT: note_on %u (slot %u)", note, slot);
}

void MidiRouter::on_clock_tick(UartMidi& midi) {
    if (cfg_.clock_out_enable) midi.send_clock();
}

void MidiRouter::on_play(UartMidi& midi) {
    if (cfg_.clock_out_enable) midi.send_start();
}

void MidiRouter::on_stop(UartMidi& midi) {
    if (cfg_.clock_out_enable) midi.send_stop();
    // Note OFF al parar
    if (note_active_) {
        for (uint8_t s = 0; s < 8; s++)
            if (last_note_[s]) midi.send_note_off(last_note_[s]);
        note_active_ = false;
    }
}
