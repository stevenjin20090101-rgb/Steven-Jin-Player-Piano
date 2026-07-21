// LVGL 8.3 config for LilyGo T4-S3 piano firmware.
// Tuned for 16-bit color, PSRAM-backed heap, touch input only.

#ifndef LV_CONF_H
#define LV_CONF_H

#include <stdint.h>

/*=================== COLOR / MEMORY ===================*/
#define LV_COLOR_DEPTH     16
#define LV_COLOR_16_SWAP   1

#define LV_MEM_CUSTOM      0
#define LV_MEM_SIZE        (96U * 1024U)
#define LV_MEM_ADR         0
#define LV_MEM_BUF_MAX_NUM 16
#define LV_MEMCPY_MEMSET_STD 0

/*=================== HAL ===================*/
#define LV_TICK_CUSTOM                 1
#define LV_TICK_CUSTOM_INCLUDE         "Arduino.h"
#define LV_TICK_CUSTOM_SYS_TIME_EXPR   (millis())

// 42 ms ≈ 24 Hz refresh. Lower than 50 to feel slightly smoother in eye
// motion while still cutting LVGL/Wire load way below 60 fps. UI text
// updates and touch feedback are imperceptibly different from 30 fps
// to the user, but the bus load reduction matters when BLE MIDI is dense.
#define LV_DISP_DEF_REFR_PERIOD        42
// Was 10 — now 40 ms (25 Hz touch polling). Each poll is an I²C read
// (touch is on the shared bus at 0x5A). At 25 Hz polling rate, touch
// latency feels instant for finger taps but bus contention with the
// dispatch is 4× lower. If you want crisper swipe-to-play later we can
// drop to 20 ms once heavy-load stability is proven.
#define LV_INDEV_DEF_READ_PERIOD       40
#define LV_DPI_DEF                     130

/*=================== FEATURE / DEBUG ===================*/
#define LV_USE_PERF_MONITOR     0
#define LV_USE_MEM_MONITOR      0
#define LV_USE_REFR_DEBUG       0
#define LV_USE_LOG              0
#define LV_USE_ASSERT_NULL      1
#define LV_USE_ASSERT_MALLOC    1
#define LV_USE_ASSERT_STYLE     0
#define LV_USE_ASSERT_MEM_INTEGRITY 0
#define LV_USE_ASSERT_OBJ       0

#define LV_USE_USER_DATA        1
#define LV_ENABLE_GC            0
#define LV_USE_GPU              0

/*=================== DRAWING ===================*/
#define LV_DRAW_COMPLEX                1
#define LV_SHADOW_CACHE_SIZE           0
#define LV_CIRCLE_CACHE_SIZE           4
#define LV_LAYER_SIMPLE_BUF_SIZE       (24 * 1024)
#define LV_LAYER_SIMPLE_FALLBACK_BUF_SIZE (3 * 1024)
#define LV_IMG_CACHE_DEF_SIZE          0
#define LV_GRADIENT_MAX_STOPS          2
#define LV_GRAD_CACHE_DEF_SIZE         0
#define LV_DITHER_GRADIENT             0
#define LV_DISP_ROT_MAX_BUF            (10 * 1024)

/*=================== FONTS ===================*/
#define LV_FONT_MONTSERRAT_8   0
#define LV_FONT_MONTSERRAT_10  0
#define LV_FONT_MONTSERRAT_12  0
#define LV_FONT_MONTSERRAT_14  1
#define LV_FONT_MONTSERRAT_16  1
#define LV_FONT_MONTSERRAT_18  0
#define LV_FONT_MONTSERRAT_20  1
#define LV_FONT_MONTSERRAT_22  0
#define LV_FONT_MONTSERRAT_24  0
#define LV_FONT_MONTSERRAT_26  0
#define LV_FONT_MONTSERRAT_28  1
#define LV_FONT_MONTSERRAT_30  0
#define LV_FONT_MONTSERRAT_32  0
#define LV_FONT_MONTSERRAT_34  0
#define LV_FONT_MONTSERRAT_36  0
#define LV_FONT_MONTSERRAT_38  0
#define LV_FONT_MONTSERRAT_40  0
#define LV_FONT_MONTSERRAT_42  0
#define LV_FONT_MONTSERRAT_44  0
#define LV_FONT_MONTSERRAT_46  0
#define LV_FONT_MONTSERRAT_48  0

#define LV_FONT_MONTSERRAT_12_SUBPX 0
#define LV_FONT_MONTSERRAT_28_COMPRESSED 0
#define LV_FONT_DEJAVU_16_PERSIAN_HEBREW 0
#define LV_FONT_SIMSUN_16_CJK 0
#define LV_FONT_UNSCII_8 0
#define LV_FONT_UNSCII_16 0

#define LV_FONT_DEFAULT &lv_font_montserrat_14
#define LV_FONT_FMT_TXT_LARGE 0
#define LV_USE_FONT_COMPRESSED 0
#define LV_USE_FONT_SUBPX 0
#define LV_USE_FONT_PLACEHOLDER 1

/*=================== TEXT ===================*/
#define LV_TXT_ENC LV_TXT_ENC_UTF8
#define LV_TXT_BREAK_CHARS " ,.;:-_"
#define LV_TXT_LINE_BREAK_LONG_LEN 0
#define LV_TXT_LINE_BREAK_LONG_PRE_MIN_LEN 3
#define LV_TXT_LINE_BREAK_LONG_POST_MIN_LEN 3
#define LV_TXT_COLOR_CMD "#"
#define LV_USE_BIDI 0
#define LV_USE_ARABIC_PERSIAN_CHARS 0

/*=================== WIDGETS ===================*/
#define LV_USE_ARC          1
#define LV_USE_BAR          1
#define LV_USE_BTN          1
#define LV_USE_BTNMATRIX    1
#define LV_USE_CANVAS       0
#define LV_USE_CHECKBOX     1
#define LV_USE_DROPDOWN     1
#define LV_USE_IMG          1
#define LV_USE_LABEL        1
#define LV_LABEL_TEXT_SELECTION 1
#define LV_LABEL_LONG_TXT_HINT 1
#define LV_USE_LINE         1
#define LV_USE_ROLLER       1
#define LV_ROLLER_INF_PAGES 7
#define LV_USE_SLIDER       1
#define LV_USE_SWITCH       1
#define LV_USE_TEXTAREA     1
#define LV_TEXTAREA_DEF_PWD_SHOW_TIME 1500
#define LV_USE_TABLE        1

/*=================== EXTRA WIDGETS ===================*/
#define LV_USE_ANIMIMG      0
#define LV_USE_CALENDAR     0
#define LV_USE_CHART        0
#define LV_USE_COLORWHEEL   1
#define LV_USE_IMGBTN       0
#define LV_USE_KEYBOARD     0
#define LV_USE_LED          0
#define LV_USE_LIST         0
#define LV_USE_MENU         0
#define LV_USE_METER        0
#define LV_USE_MSGBOX       0
#define LV_USE_SPAN         0
#define LV_USE_SPINBOX      0
#define LV_USE_SPINNER      0
#define LV_USE_TABVIEW      0
#define LV_USE_TILEVIEW     0
#define LV_USE_WIN          0

/*=================== THEME ===================*/
#define LV_USE_THEME_DEFAULT 1
#define LV_THEME_DEFAULT_DARK 1
#define LV_THEME_DEFAULT_GROW 1
#define LV_THEME_DEFAULT_TRANSITION_TIME 80
#define LV_USE_THEME_BASIC 0
#define LV_USE_THEME_MONO  0

/*=================== LAYOUT ===================*/
#define LV_USE_FLEX 1
#define LV_USE_GRID 1

/*=================== OTHERS ===================*/
#define LV_USE_FS_STDIO   0
#define LV_USE_FS_POSIX   0
#define LV_USE_FS_WIN32   0
#define LV_USE_FS_FATFS   0
#define LV_USE_PNG        0
#define LV_USE_BMP        0
#define LV_USE_SJPG       0
#define LV_USE_GIF        0
#define LV_USE_QRCODE     0
#define LV_USE_FREETYPE   0
#define LV_USE_RLOTTIE    0
#define LV_USE_FFMPEG     0

#define LV_USE_SNAPSHOT   0
#define LV_USE_MONKEY     0
#define LV_USE_GRIDNAV    0
#define LV_USE_FRAGMENT   0
#define LV_USE_IMGFONT    0
#define LV_USE_MSG        0
#define LV_USE_IME_PINYIN 0

/*=================== EXAMPLES / DEMOS ===================*/
#define LV_BUILD_EXAMPLES 0
#define LV_USE_DEMO_WIDGETS        0
#define LV_USE_DEMO_KEYPAD_AND_ENCODER 0
#define LV_USE_DEMO_BENCHMARK      0
#define LV_USE_DEMO_STRESS         0
#define LV_USE_DEMO_MUSIC          0

#endif /*LV_CONF_H*/
