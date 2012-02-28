/**
 * $Id$
 *
 * JNI wrappers for operating the emulator from Java.
 *
 * Copyright (c) 2012 Nathan Keynes.
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

#include <jni.h>
#include <android/log.h>
#include <libisofs.h>
#include "dreamcast.h"
#include "gui.h"
#include "config.h"
#include "lxpaths.h"
#include "display.h"
#include "gdlist.h"
#include "hotkeys.h"
#include "serial.h"
#include "aica/audio.h"
#include "drivers/video_gl.h"
#include "maple/maple.h"
#include "vmu/vmulist.h"

static char *getStringChars( JNIEnv *env, jstring str )
{
    jboolean iscopy;
    const char *p = (*env)->GetStringUTFChars(env, str, &iscopy);
    char *result = strdup(p);
    (*env)->ReleaseStringUTFChars(env,str,p);
    return result;
}

static const char *appHome = NULL;

JNIEXPORT void JNICALL Java_org_lxdream_Dreamcast_init(JNIEnv * env, jclass obj,  jstring homeDir )
{
    appHome = getStringChars(env, homeDir);
    const char *confFile = g_strdup_printf("%s/lxdreamrc", appHome);
    set_user_data_path(appHome);
    lxdream_set_config_filename( confFile );
    lxdream_make_config_dir( );
    lxdream_load_config( );
    iso_init();
    gdrom_list_init();
    vmulist_init();
    dreamcast_init(1);

    audio_init_driver(NULL);
    display_driver_t display_driver = get_display_driver_by_name(NULL);
    display_set_driver(display_driver);

    hotkeys_init();
    serial_init();
    maple_reattach_all();
    INFO( "%s! ready...", APP_NAME );
}

JNIEXPORT void JNICALL Java_org_lxdream_Dreamcast_setViewSize(JNIEnv * env, jclass obj, jint width, jint height)
{
    gl_set_video_size(width, height);
}

JNIEXPORT void JNICALL Java_org_lxdream_Dreamcast_run(JNIEnv * env, jclass obj)
{
    dreamcast_run();
}

JNIEXPORT void JNICALL Java_org_lxdream_Dreamcast_stop(JNIEnv * env, jclass obj)
{
    dreamcast_stop();
}

gboolean gui_parse_cmdline( int *argc, char **argv[] )
{
    return TRUE;
}

gboolean gui_init( gboolean debug, gboolean fullscreen )
{
    return TRUE;
}

void gui_main_loop( gboolean run ) {
    if( run ) {
        dreamcast_run();
    }
}

gboolean gui_error_dialog( const char *fmt, ... )
{
    return TRUE;
}

void gui_update_state()
{
}

void gui_set_use_grab( gboolean grab )
{
}

void gui_update_io_activity( io_activity_type activity, gboolean active )
{
}

void gui_do_later( do_later_callback_t func )
{
    func();
}