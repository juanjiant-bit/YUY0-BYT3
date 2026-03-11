#pragma once
// bytebeat_node.h — Bytebeat Machine V2.0
// CAMBIOS:
//   + MAX_NODES: 31 → 48 (más profundidad sin heap; 48×5 bytes = 240 bytes estáticos)
//   + NodeType::TONAL_NODE — emite t*k donde k es una constante musical (pitch hint)
//   + NodeType::CLOCK_B    — emite t>>2 (clock lento estable, enriquece ritmo interno)
//   + NodeType::FOLD       — byte-fold: ((x & 0xFF) > 127 ? 255-(x&0xFF) : (x&0xFF))
//                            produce forma de onda triangular en lugar de serrucho
//   + NodeType::GT         — comparador: left > right ? 255 : 0 (gate/pulso digital)
//
// Todos los nodos nuevos son OPCIONALES en ZoneConfig — el generador los elige
// según probabilidad de zona, exactamente igual que los existentes.
//
// COMPATIBILIDAD: EvalContext no cambia (se agrega nada a struct).
//
#include <cstdint>

struct EvalContext {
    uint32_t t;
    float    macro;
    float    tonal;
    uint8_t  zone;
    float    time_div;
    float    spread;
    float    quant_amount;
    uint8_t  scale_id;
    uint8_t  root;
    bool     note_mode_active;
    float    note_pitch_ratio;
    float    drum_color;
    float    drum_decay;
};

enum class NodeType : uint8_t {
    // ── V1 — sin cambios ─────────────────────────────
    T, CONST, MACRO,
    ADD, SUB, MUL,
    AND, OR,  XOR,
    SHR, SHL, MOD,
    NEG,
    // ── V2 — nuevos ──────────────────────────────────
    TONAL_NODE,  // t * const_val  (const_val = constante musical)
                 // Emite un "clock tonal" que da más identidad de pitch
                 // al árbol sin depender del módulo externo de quantizer.
    CLOCK_B,     // t >> 2 — clock lento derivado, más grave, más estable
                 // Útil para bajas frecuencias y bases rítmicas.
    FOLD,        // byte-fold sobre el hijo izquierdo
                 // out = val<=127 ? val : 255-val  (triángulo sobre serrucho)
                 // Produce aliasing más suave y formas triangulares.
    GT,          // left > right ? 255 : 0
                 // Pulso digital duro — útil para percusión y gates.
};

// MAX_NODES aumentado a 48 para árboles más ricos.
// Costo: 48 × sizeof(Node) = 48 × 5 = 240 bytes en stack/estático.
// Con dos grafos en BytebeatGraph (active + incoming) = 480 bytes total.
// Seguro dentro del presupuesto de ~110KB RAM libre.
static constexpr uint8_t MAX_NODES = 48;

struct Node {
    NodeType type;
    int32_t  const_val;
    uint8_t  left;
    uint8_t  right;
};
