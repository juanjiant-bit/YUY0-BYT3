#pragma once
// midi_router.h — Bytebeat Machine V1.14
// Bidireccional:
//   IN:  MidiEvent  -> SequencerEvent (note->snapshot, CC->param, clock->BPM)
//   OUT: SequencerEvent/pot changes -> MIDI CC/note/clock
//
// Integracion en Core1 loop:
//   midi_.poll_rx();
//   midi_router_.process_in(midi_, state, seq, queue);
//   midi_router_.process_out(midi_, seq, pots);
//
#include "uart_midi.h"
#include "../state/state_manager.h"
#include "../sequencer/sequencer.h"
#include "../utils/ring_buffer.h"

class MidiRouter {
public:
    void init(UartMidi* midi, const MidiConfig& cfg);

    // Procesar MIDI IN -> acciones internas
    void process_in (UartMidi& midi, StateManager& state,
                     Sequencer& seq,
                     RingBuffer<SequencerEvent, 128>& queue);

    // Enviar estado interno -> MIDI OUT
    // Llamar cuando cambian pots o se dispara un snapshot
    void on_pot_change  (UartMidi& midi, uint8_t pot_idx, float val);
    void on_snap_trigger(UartMidi& midi, uint8_t slot);
    void on_clock_tick  (UartMidi& midi);  // llamar cada 24 PPQN
    void on_play        (UartMidi& midi);
    void on_stop        (UartMidi& midi);

private:
    // Clock IN: calcular BPM desde intervalos entre ticks
    void     update_clock_in(Sequencer& seq);
    uint32_t last_clock_us_  = 0;
    uint32_t clock_interval_ = 0;  // us entre ticks

    // Note ON activas (para enviar note OFF al trigger siguiente)
    uint8_t  last_note_[8] = {};  // nota enviada por cada slot
    uint8_t  active_slot_  = 0xFF; // slot activo actual (0xFF = ninguno)
    bool     note_active_  = false;

    MidiConfig cfg_ = {};
};
