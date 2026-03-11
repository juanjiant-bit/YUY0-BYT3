#pragma once
// snapshot_event.h — canal Core1 → Core0
// V1.10: pots[5] → pots[6] para incluir ENV_RELEASE en el save de snapshot
#include <cstdint>

enum class SnapshotEventType : uint8_t { NONE, TRIGGER, SAVE };

struct SnapshotEvent {
    SnapshotEventType type  = SnapshotEventType::NONE;
    uint8_t           slot  = 0;
    float             pots[6] = {};  // V1.10: pot[5] = env_release
};
