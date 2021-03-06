/**
 * $Id$
 *
 * GL-based support functions
 *
 * Copyright (c) 2005 Nathan Keynes.
 * Copyright (c) 2014 Henrik Andersson.
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

#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <glib.h>
#include "pvr2/glutil.h"

gboolean isOpenGLES2()
{
    const char *str = glGetString(GL_VERSION);
    if( strncmp(str, "OpenGL ES 2.", 12) == 0 ) {
        return TRUE;
    }
    return FALSE;
}

gboolean isGLSecondaryColorSupported()
{
    return isGLExtensionSupported("GL_EXT_secondary_color");
}

gboolean isGLVertexBufferSupported()
{
    return isGLExtensionSupported("GL_ARB_vertex_buffer_object");
}

gboolean isGLPixelBufferSupported()
{
    return isGLExtensionSupported("GL_ARB_pixel_buffer_object");
}

gboolean isGLMirroredTextureSupported()
{
    return isGLExtensionSupported("GL_ARB_texture_mirrored_repeat");
}

gboolean isGLBGRATextureSupported()
{
    /* Note: e.g. Tegra 3 reports GL_EXT_bgra, but it doesn't actually work.
     * Need to check this with NVIDIA, in meantime assume GLES2 doesn't have
     * BGRA support */
    return !isOpenGLES2() && isGLExtensionSupported("GL_EXT_bgra");
}

gboolean isGLShaderSupported()
{
    return isOpenGLES2() || (isGLExtensionSupported("GL_ARB_fragment_shader") &&
    isGLExtensionSupported("GL_ARB_vertex_shader") &&
    isGLExtensionSupported("GL_ARB_shading_language_100"));
}

/**
 * Check if there's at least 2 texture units
 */
gboolean isGLMultitextureSupported()
{
    if( !isOpenGLES2() && !isGLExtensionSupported("GL_ARB_multitexture") )
        return FALSE;
    int units = 0;

#if defined(GL_MAX_TEXTURE_UNITS)
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
#elif defined(GL_MAX_TEXTURE_IMAGE_UNITS)
        glGetIntegerv(GL_MAX_TEXTURE_IMAGE_UNITS, &units);
#endif
    return units >= 2;
}

gboolean isGLVertexRangeSupported()
{
    return isGLExtensionSupported("GL_APPLE_vertex_array_range") ||
            isGLExtensionSupported("GL_NV_vertex_array_range");
}

/**
 * Test if a specific extension is supported. From opengl.org
 * @param extension extension name to check for
 * @return TRUE if supported, otherwise FALSE.
 */
gboolean isGLExtensionSupported( const char *extension )
{
    const GLubyte *extensions = NULL;
    const GLubyte *start;
    GLubyte *where, *terminator;

    /* Extension names should not have spaces. */
    where = (GLubyte *) strchr(extension, ' ');
    if (where || *extension == '\0')
        return 0;
    extensions = glGetString(GL_EXTENSIONS);
    if( extensions == NULL ) {
        /* No GL available, so we're pretty sure the extension isn't
         * available either. */
        return FALSE;
    }
    /* It takes a bit of care to be fool-proof about parsing the
       OpenGL extensions string. Don't be fooled by sub-strings,
       etc. */
    start = extensions;
    for (;;) {
        where = (GLubyte *) strstr((const char *) start, extension);
        if (!where)
            break;
        terminator = where + strlen(extension);
        if (where == start || *(where - 1) == ' ')
            if (*terminator == ' ' || *terminator == '\0')
                return TRUE;
        start = terminator;
    }
    return FALSE;
}

int compare_charp( const void *a, const void *b )
{
    const char **ca = (const char **)a;
    const char **cb = (const char **)b;
    return strcmp(*ca, *cb);
}

#define DEFAULT_TERMINAL_COLUMNS 80
#define DEFAULT_COLUMN_WIDTH 34

int glGetMaxColourAttachments()
{
#ifdef GL_MAX_COLOR_ATTACHMENTS
    GLint result = 0;
    glGetIntegerv(GL_MAX_COLOR_ATTACHMENTS, &result);
    return result;
#else
    return 1;
#endif
}


/**
 * Define an orthographic projection matrix
 * Note: row-major order
 */
void defineOrthoMatrix( GLfloat *matrix, GLfloat width, GLfloat height, GLfloat znear, GLfloat zfar )
{
    matrix[0] =  2/width;
    matrix[1] =  0;
    matrix[2] =  0;
    matrix[3] =  0;

    matrix[4] =  0;
    matrix[5] = -2/height;
    matrix[6] =  0;
    matrix[7] =  0;

    matrix[8] =  0;
    matrix[9] =  0;
    matrix[10]= -2/(zfar-znear);
    matrix[11]=  0;

    matrix[12]= -1;
    matrix[13]=  1;
    matrix[14]= -(zfar+znear)/(zfar-znear);
    matrix[15]=  1;
}

/**
 * Format a GL extension list (or other space-separated string) nicely, and
 * print to the given output stream.
 */

void fprint_extensions( FILE *out, const char *extensions )
{
    unsigned int i, j, count, maxlen = DEFAULT_COLUMN_WIDTH, columns, per_column, terminal_columns;
    const char *terminal_columns_str = getenv("COLUMNS");
    if( terminal_columns_str == NULL || (terminal_columns = strtol(terminal_columns_str,0,10)) == 0 )
        terminal_columns = DEFAULT_TERMINAL_COLUMNS;

    if( extensions == NULL || extensions[0] == '\0' )
        return;

    gchar *ext_dup = g_strdup(extensions);
    gchar **ext_split = g_strsplit(g_strstrip(ext_dup), " ", 0);
    for( count = 0; ext_split[count] != NULL; count++ ) {
        unsigned len = strlen(ext_split[count]);
        if( len > maxlen )
            maxlen = len;
    }

    columns = terminal_columns / (maxlen+2);
    if( columns == 0 )
        columns = 1;
    per_column = (count+columns-1) / columns;

    qsort(ext_split, count, sizeof(gchar *), compare_charp);

    for( i=0; i<per_column; i++ ) {
        for( j=0; j<columns; j++ ) {
            unsigned idx = i + (j*per_column);
            if( idx < count )
                fprintf( out, "  %-*s", maxlen, ext_split[idx] );
        }
        fprintf( out, "\n" );
    }
    g_strfreev(ext_split);
    g_free(ext_dup);
}

void glPrintInfo( FILE *out )
{
    fprintf( out, "GL Vendor: %s\n", glGetString(GL_VENDOR) );
    fprintf( out, "GL Renderer: %s\n", glGetString(GL_RENDERER) );
    fprintf( out, "GL Version: %s\n", glGetString(GL_VERSION) );
    if( isGLShaderSupported() ) {
         fprintf( out, "SL Version: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION) );
    }

    fprintf( out, "GL Extensions:\n" );

    fprint_extensions( out, (const gchar *)glGetString(GL_EXTENSIONS) );
    if( display_driver && display_driver->print_info ) {
        fprintf( out, "\n");
        display_driver->print_info(out);
    }
}

gboolean gl_check_error(const char *context)
{
    GLint err = glGetError();
    if( err != 0 ) {
        const char *s;
        switch( err ) {
        case GL_INVALID_ENUM: s = "Invalid enum"; break;
        case GL_INVALID_VALUE: s = "Invalid value"; break;
        case GL_INVALID_OPERATION: s = "Invalid operation"; break;
#ifdef GL_STACK_OVERFLOW
        case GL_STACK_OVERFLOW: s = "Stack overflow"; break;
#endif
#ifdef GL_STACK_UNDERFLOW
        case GL_STACK_UNDERFLOW: s = "Stack underflow"; break;
#endif
        case GL_OUT_OF_MEMORY:   s = "Out of memory"; break;
        default: s = "Unknown error"; break;
        }
        if( context ) {
            WARN( "%s: GL error: %x (%s)", context, err, s );
        } else {
            WARN( "GL error: %x (%s)", err, s );
        }
        return FALSE;
    }
    return TRUE;
}

static int bgra_to_rgba_type( int glFormatType )
{
    switch( glFormatType ) {
    case GL_UNSIGNED_SHORT_1_5_5_5_REV:
        return GL_UNSIGNED_SHORT_5_5_5_1;
    case GL_UNSIGNED_SHORT_4_4_4_4_REV:
        return GL_UNSIGNED_SHORT_4_4_4_4;
    case GL_UNSIGNED_BYTE:
        return GL_UNSIGNED_BYTE;
    default:
        assert( 0 && "Unsupported BGRA format" );
        return glFormatType;
    }
}

/**
 * Convert BGRA data in buffer to RGBA format in-place (for systems that don't natively
 * support BGRA).
 * @return converted format type
 * @param data BGRA pixel data
 * @param nPixels total number of pixels (width*height)
 * @param glFormatType GL format of source data. One of
 *    GL_UNSIGNED_SHORT_1_5_5_5_REV, GL_UNSIGNED_SHORT_4_4_4_4_REV, or GL_UNSIGNED_BYTE
 */
static int bgra_to_rgba( unsigned char *data, unsigned nPixels, int glFormatType )
{
    unsigned i;
    switch( glFormatType ) {
    case GL_UNSIGNED_SHORT_1_5_5_5_REV: {
        uint16_t *p = (uint16_t *)data;
        uint16_t *end = p + nPixels;
        while( p != end ) {
            uint16_t v = *p;
            *p = (v >> 15) | (v<<1);
            p++;
        }
        return GL_UNSIGNED_SHORT_5_5_5_1;
    }
    case GL_UNSIGNED_SHORT_4_4_4_4_REV: { /* ARGB => RGBA */
        uint16_t *p = (uint16_t *)data;
        uint16_t *end = p + nPixels;
        while( p != end ) {
            uint16_t v = *p;
            *p = (v >> 12) | (v<<4);
            p++;
        }
        return GL_UNSIGNED_SHORT_4_4_4_4;
    }
    case GL_UNSIGNED_BYTE: { /* ARGB => ABGR */
        uint32_t *p = (uint32_t *)data;
        uint32_t *end = p + nPixels;
        while( p != end ) {
            uint32_t v = *p;
            *p = (v&0xFF000000) | ((v<<16) & 0x00FF0000) | (v & 0x0000FF00) | ((v>>16) & 0x000000FF);
            p++;
        }
        return GL_UNSIGNED_BYTE;
    }
    default:
        assert( 0 && "Unsupported BGRA format" );
        return glFormatType;
    }
}

void glTexImage2DBGRA( int level, GLint intFormat, int width, int height, GLint format, GLint type, unsigned char *data, int preserveData )
{
    if( format == GL_BGRA && !display_driver->capabilities.has_bgra ) {
        if( preserveData ) {
            size_t size = width * height * (type == GL_UNSIGNED_BYTE ? 4 : 2);
            char buf[size];
            memcpy(buf, data, size);
            GLint rgbaType = bgra_to_rgba( buf, width*height, type );
            glTexImage2D( GL_TEXTURE_2D, level, intFormat, width, height, 0, GL_RGBA, rgbaType,
                    buf );
        } else {
            GLint rgbaType = bgra_to_rgba( data, width*height, type );
            glTexImage2D( GL_TEXTURE_2D, level, intFormat, width, height, 0, GL_RGBA, rgbaType,
                    data );
        }
    } else {
        glTexImage2D( GL_TEXTURE_2D, level, intFormat, width, height, 0, format, type,
                data );
    }
}

void glTexSubImage2DBGRA( int level, int xoff, int yoff, int width, int height, GLint format, GLint type, unsigned char *data, int preserveData )
{
    if( format == GL_BGRA && !display_driver->capabilities.has_bgra ) {
        if( preserveData ) {
            size_t size = width * height * (type == GL_UNSIGNED_BYTE ? 4 : 2);
            char buf[size];
            memcpy(buf, data, size);
            GLint rgbaType = bgra_to_rgba( buf, width*height, type );
            glTexSubImage2D( GL_TEXTURE_2D, level, xoff, yoff, width, height, GL_RGBA, rgbaType,
                    buf );
        } else {
            GLint rgbaType = bgra_to_rgba( data, width*height, type );
            glTexSubImage2D( GL_TEXTURE_2D, level, xoff, yoff, width, height, GL_RGBA, rgbaType,
                    data );
        }
    } else {
        glTexSubImage2D( GL_TEXTURE_2D, level, xoff, yoff, width, height, format, type,
                data );
    }
}
