#pragma once
// flash_store.h — Bytebeat Machine V1.10
// V1.10: PackedSnapshot agrega env_release_u8, env_attack_u8, env_loop_u8 (3 bytes)
//        Tamaño 64 → 68 bytes (con padding de alineación). static_assert actualizado.
//        FLASH_VERSION: 2 → 3 (carga V2 antigua sin env fields → defaults 0)
//        FLASH_MAGIC sin cambios (carga V2 detecta version field = 2 → migra).
// Persistencia de snapshots en el último sector Flash del RP2040 (4KB).
//
// LAYOUT (offset 0x1FF000, 4096 bytes):
//   [0..3]   magic  0xBB161100  (bytebeat 1.6.1)
//   [4..7]   CRC32 del bloque de snapshots
//   [8..9]   version = 2
//   [10..11] num_snapshots = 8
//   [12..N]  8 × PackedSnapshot
//   resto    0xFF
//
// REGLAS:
//   Llamar SOLO desde Core1 con Core0 pausado (multicore_lockout).
//   flash_store.save() maneja todo el protocolo de pausa.
//
// VERSIÓN 2 añade: scale_id, root, drum_color, drum_decay.
//
#include <cstdint>
#include "state_manager.h"

static constexpr uint32_t FLASH_TARGET_OFFSET = 0x1FF000u;  // 2MB - 4KB
static constexpr uint32_t FLASH_SECTOR_SIZE   = 4096u;
static constexpr uint32_t FLASH_MAGIC         = 0xBB161100u;  // V1.6.1
static constexpr uint16_t FLASH_VERSION       = 3;            // V1.10 format (env fields)

// ── PackedSnapshot V2 ─────────────────────────────────────────
// 64 bytes — todos los campos de Snapshot que el usuario puede guardar.
// El BytebeatGraph se regenera desde seed en init().
struct PackedSnapshot {
    // V1 fields (offset 0)
    uint32_t seed;
    uint8_t  zone;
    uint8_t  _pad0[3];
    float    macro;
    float    glide_time;
    float    time_div;
    float    tonal;
    float    spread;
    float    filter_cutoff;
    float    fx_amount;
    float    drive;
    float    reverb_room;
    float    reverb_wet;
    uint8_t  valid;
    uint8_t  _pad1[3];
    // V2 fields (offset 52) — añadidos en V1.6.1
    uint8_t  scale_id;
    uint8_t  root;
    uint8_t  _pad2[2];
    float    drum_color;
    float    drum_decay;
    // V1.10 — envelope (empaquetado como uint8_t para mínimo impacto en layout)
    uint8_t  env_release_u8;  // 0-255 mapeado de float 0.0-1.0
    uint8_t  env_attack_u8;   // 0-255 mapeado de float 0.0-1.0
    uint8_t  env_loop;        // 0=off, 1=on
    uint8_t  _pad3;           // alineación
} __attribute__((packed));

static_assert(sizeof(PackedSnapshot) == 68, "PackedSnapshot V3 size mismatch");

struct FlashBlock {
    uint32_t       magic;
    uint32_t       crc32;
    uint16_t       version;
    uint8_t        num_snapshots;
    uint8_t        _pad;
    PackedSnapshot snapshots[8];
} __attribute__((packed));  // 8 × 68 = 544 bytes de snapshots

// ── CRC32 IEEE 802.3 (sin tabla, sin dependencias) ────────────
inline uint32_t crc32_compute(const uint8_t* data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++)
            crc = (crc >> 1) ^ (crc & 1u ? 0xEDB88320u : 0u);
    }
    return ~crc;
}

// ── API ───────────────────────────────────────────────────────
class FlashStore {
public:
    // Cargar snapshots. Retorna true si magic+CRC+version OK.
    // Llamar desde Core1 antes de lanzar Core0.
    static bool load(Snapshot out_snapshots[8]);

    // Guardar snapshots. Pausa Core0, erase, write, reactiva.
    // Solo desde Core1.
    static bool save(const Snapshot in_snapshots[8]);

    // Erase explícito (útil para factory reset)
    static void erase_sector();

private:
    static void pack  (const Snapshot& src, PackedSnapshot& dst);
    static void unpack(const PackedSnapshot& src, Snapshot& dst);
};
