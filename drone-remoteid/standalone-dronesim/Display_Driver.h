#pragma once

#include <lvgl.h>
#include "Display_ST7789.h"
#include "PWR_Key.h"

#ifdef __cplusplus
extern "C" {
#endif

	#define BUTTON_COUNT 100

	typedef struct {
    lv_obj_t* obj;
    int32_t start_x, start_y;
    int32_t target_x, target_y;
} WanderCtx;

extern WanderCtx ctxs[BUTTON_COUNT];

	// Initialize the display driver and LVGL display
	bool init_display(void);

	// Set the display rotation
	void set_display_rotation(lv_display_t* disp, lv_display_rotation_t rotation);


#ifdef __cplusplus
} // extern "C"
#endif