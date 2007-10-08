/**
 * $Id: video_null.c,v 1.5 2007-10-08 11:49:35 nkeynes Exp $
 *
 * Null video output driver (ie no video output whatsoever)
 *
 * Copyright (c) 2005 Nathan Keynes.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "display.h"

render_buffer_t video_null_create_render_buffer( uint32_t hres, uint32_t vres )
{
    return NULL;
}

void video_null_destroy_render_buffer( render_buffer_t buffer )
{
}

gboolean video_null_set_render_target( render_buffer_t buffer )
{
    return TRUE;
}

gboolean video_null_display_render_buffer( render_buffer_t buffer )
{
    return TRUE;
}

gboolean video_null_read_render_buffer( render_buffer_t buffer, unsigned char *target )
{
    return TRUE;
}

gboolean video_null_display_frame_buffer( frame_buffer_t buffer )
{
    return TRUE;
}

gboolean video_null_display_blank( uint32_t colour )
{
    return TRUE;
}

void video_null_display_back_buffer( void )
{
}


struct display_driver display_null_driver = { "null", 
					      NULL,
					      NULL,
					      NULL,
					      video_null_create_render_buffer,
					      video_null_destroy_render_buffer,
					      video_null_set_render_target,
					      video_null_display_frame_buffer,
					      video_null_display_render_buffer,
					      video_null_display_blank,
					      video_null_read_render_buffer };
