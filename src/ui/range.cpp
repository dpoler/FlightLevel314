#include "range.h"
#include <cstdio>

static const float RANGE_LEVELS[] = {150.0f, 100.0f, 50.0f, 20.0f, 5.0f, 1.0f};
static int _range_idx = 2; // default 50nm
static char _label_buf[8];

float range_get_nm() {
    return RANGE_LEVELS[_range_idx];
}

int range_get_index() {
    return _range_idx;
}

void range_cycle() {
    _range_idx = (_range_idx + 1) % RANGE_NUM_LEVELS;
}

const char* range_label() {
    int nm = (int)RANGE_LEVELS[_range_idx];
    snprintf(_label_buf, sizeof(_label_buf), "%dnm", nm);
    return _label_buf;
}
