#pragma once

// Global shared range control — up to 4 user-configurable levels
#define RANGE_MAX_LEVELS 4

float range_get_nm();
int range_get_index();
void range_cycle();                              // advance to next level, wrapping
const char* range_label();                       // e.g. "50nm"
void range_set_default(int nm);                  // snap to nearest level
void range_set_index(int idx);                   // jump directly to a level index (clamped), e.g. resuming a persisted choice
// Load user presets (sorted descending internally). Keeps the current range
// if it is still a preset; otherwise keeps the same slot (so editing the
// active preset, e.g. 5 -> 2 nm, lands on the new value).
void range_set_levels(const int *nm_values, int count);

float range_max_nm();                            // widest level in effect

// Apply an edit of the presets in effect (Settings, or the active
// location's own): keeps the current range, and if the active preset itself
// was edited (exactly one new value), follows it to the new value even when
// its rank changed (5 -> 30). No-op when old == new.
void range_apply_edited_presets(const int old_presets[4], const int new_presets[4]);

// Load the active location's presets (or Settings') if they differ from the
// levels in effect -- call after switching locations. Returns true if the
// levels changed (caller may persist last_range_idx).
bool range_sync_active_presets();
