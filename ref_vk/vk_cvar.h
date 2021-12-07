#pragma once

#include "cvardef.h"

// from engine/common/cvar.h
#define FCVAR_READ_ONLY		(1<<17)	// cannot be set by user at all, and can't be requested by CvarGetPointer from game dlls

#define CVAR_TO_BOOL( x )		((x) && ((x)->value != 0.0f) ? true : false )

void VK_LoadCvars( void );

#define DECLARE_CVAR(X) \
	X(r_lighting_modulate) \
	X(cl_lightstyle_lerping) \
	X(vk_rtx_bounces) \
	X(vk_rtx_prev_frame_blend_factor) \
	X(vk_rtx_light_begin) \
	X(vk_rtx_light_end) \
	X(r_lightmap) \
	X(ui_infotool) \
	X(vk_rtx) \
	X(vk_rtx_extension) \
	X(vk_hdr_output) \
	X(vk_hdr_output_extension) \
	X(vk_hdr_output_max_luminance) \
	X(vk_hdr_output_auto_adjust) \
	X(vk_hdr_output_manual_rtx_adjust_down) \
	X(vk_hdr_output_manual_rtx_adjust_additive_down) \
	X(vk_hdr_output_manual_adjust_ui_down) \
	X(vk_hdr_output_manual_adjust_down) \

#define EXTERN_CVAR(cvar) extern cvar_t *cvar;
DECLARE_CVAR(EXTERN_CVAR)
#undef EXTERN_CVAR
