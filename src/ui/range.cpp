#include "range.h"
#include "../data/locations.h"
#include <cstdio>
#include <cstdlib>
#include <cmath>

static float _levels[RANGE_MAX_LEVELS] = {50.0f, 20.0f, 10.0f, 5.0f};
static int _count = 4;
static int _idx = 0;
static char _label_buf[8];

float range_get_nm() {
    return _levels[_idx];
}

int range_get_index() {
    return _idx;
}

void range_cycle() {
    _idx = (_idx + 1) % _count;
}

const char* range_label() {
    snprintf(_label_buf, sizeof(_label_buf), "%dnm", (int)_levels[_idx]);
    return _label_buf;
}

void range_set_levels(const int *nm_values, int count) {
    const float prev_nm = _levels[_idx];
    if (count < 1) count = 1;
    if (count > RANGE_MAX_LEVELS) count = RANGE_MAX_LEVELS;
    _count = count;
    for (int i = 0; i < count; i++)
        _levels[i] = (float)nm_values[i];
    // Sort descending so index 0 = largest range (widest view first)
    for (int i = 0; i < _count - 1; i++)
        for (int j = i + 1; j < _count; j++)
            if (_levels[j] > _levels[i]) {
                float tmp = _levels[i]; _levels[i] = _levels[j]; _levels[j] = tmp;
            }
    if (_idx >= _count) _idx = _count - 1;
    for (int i = 0; i < _count; i++) {
        if (_levels[i] == prev_nm) { _idx = i; break; }
    }
}

float range_max_nm() {
    return _levels[0];
}

// True when `p` (any order) is exactly the set of levels in effect.
static bool levels_match(const int p[4]) {
    if (_count != 4) return false;
    float sorted[4];
    for (int i = 0; i < 4; i++) sorted[i] = (float)p[i];
    for (int i = 0; i < 3; i++)
        for (int j = i + 1; j < 4; j++)
            if (sorted[j] > sorted[i]) { float t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    for (int i = 0; i < 4; i++)
        if (sorted[i] != _levels[i]) return false;
    return true;
}

void range_apply_edited_presets(const int old_presets[4], const int new_presets[4]) {
    bool same = true;
    for (int i = 0; i < 4; i++)
        if (old_presets[i] != new_presets[i]) same = false;
    if (same && levels_match(new_presets)) return;

    const float prev_nm = range_get_nm();
    range_set_levels(new_presets, 4);
    if (range_get_nm() == prev_nm) return;

    int added = -1, n_added = 0;
    for (int i = 0; i < 4; i++) {
        bool was_there = false;
        for (int j = 0; j < 4; j++)
            if (new_presets[i] == old_presets[j]) was_there = true;
        if (!was_there) { added = new_presets[i]; n_added++; }
    }
    if (n_added == 1) range_set_default(added);
}

bool range_sync_active_presets() {
    int p[4];
    locations_active_range_presets(p);
    if (levels_match(p)) return false;
    range_set_levels(p, 4);
    return true;
}

void range_set_index(int idx) {
    if (idx < 0) idx = 0;
    if (idx >= _count) idx = _count - 1;
    _idx = idx;
}

void range_set_default(int nm) {
    int best = 0;
    float best_diff = fabsf((float)nm - _levels[0]);
    for (int i = 1; i < _count; i++) {
        float diff = fabsf((float)nm - _levels[i]);
        if (diff < best_diff) { best_diff = diff; best = i; }
    }
    _idx = best;
}
