#ifndef _GT911_TOUCH_H
#define _GT911_TOUCH_H
#include <stdio.h>

class gt911_touch
{
public:
    gt911_touch(int8_t sda_pin, int8_t scl_pin, int8_t rst_pin = -1, int8_t int_pin = -1);

    void begin();
    bool begin_safe();  // returns false on failure instead of aborting
    bool is_ready() { return _ready; }
    bool getTouch(uint16_t *x, uint16_t *y);
    void set_rotation(uint8_t r);

private:
    int8_t _sda, _scl, _rst, _int;
    bool _ready = false;
    // Debounce state for getTouch() -- see its .cpp comment.
    uint8_t _press_streak = 0;
    uint16_t _cand_x = 0, _cand_y = 0;
    bool _is_pressed = false;
    uint8_t _release_streak = 0;
};

#endif
