#pragma once

#include <stdint.h>

// Kept completely separate from the Pool bot.  The Solitaire controller only
// runs while the game's own SolitaireUIController is alive.
void solitaire_init(uintptr_t il2cppBase);
void solitaire_capture_ui(void* solitaireUi);
void solitaire_tick();
void solitaire_set_enabled(bool enabled);
bool solitaire_is_enabled();
bool solitaire_is_ready();

// Versioned loader performance channel. Values are clamped natively; changing
// them never bypasses exact-state validation or the one-action-in-flight rule.
void solitaire_set_performance_profile(int profile, int sliceNodes, int sliceMs,
                                       int arenaNodes, int cardSafetyMs,
                                       int talonSafetyMs);
