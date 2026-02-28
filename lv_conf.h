#ifndef LV_CONF_H
#define LV_CONF_H

#define LV_COLOR_DEPTH 16
#define LV_COLOR_16_SWAP 0

/* Memory */
#define LV_MEM_CUSTOM 1
#define LV_MEM_CUSTOM_INCLUDE <stdlib.h>
#define LV_MEM_CUSTOM_ALLOC malloc
#define LV_MEM_CUSTOM_FREE free
#define LV_MEM_CUSTOM_REALLOC realloc

/* Tick */
#define LV_TICK_CUSTOM 1
#define LV_TICK_CUSTOM_INCLUDE "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR (millis())

/* Display */
#define LV_USE_LOG 1
#define LV_LOG_LEVEL LV_LOG_LEVEL_WARN
#define LV_LOG_PRINTF 1

/* Fonts - enable the ones we need */
#define LV_FONT_MONTSERRAT_14 1
#define LV_FONT_MONTSERRAT_16 1
#define LV_FONT_MONTSERRAT_20 1
#define LV_FONT_MONTSERRAT_24 1
#define LV_FONT_MONTSERRAT_28 1
#define LV_FONT_MONTSERRAT_36 1

/* Widgets */
#define LV_USE_LABEL 1
#define LV_USE_BTN 1
#define LV_USE_IMG 1
#define LV_USE_LINE 1
#define LV_USE_ARC 1
#define LV_USE_TABLE 1
#define LV_USE_CANVAS 1
#define LV_USE_TILEVIEW 1
#define LV_USE_SLIDER 1
#define LV_USE_SWITCH 1
#define LV_USE_TEXTAREA 1
#define LV_USE_KEYBOARD 1
#define LV_USE_LIST 1
#define LV_USE_MSGBOX 1
#define LV_USE_ROLLER 1
#define LV_USE_DROPDOWN 1
#define LV_USE_CHECKBOX 1
#define LV_USE_SPINNER 1

/* Animations */
#define LV_USE_ANIM 1

/* Drawing */
#define LV_DRAW_SW_DRAW_UNIT_CNT 1

/* Image decoders */
#define LV_USE_LODEPNG 1

#endif /* LV_CONF_H */
