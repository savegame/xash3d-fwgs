/*
gl_fbo.c - offscreen 3D/2D framebuffers + rotated composite for AuroraOS/Wayland
Copyright (C) 2026

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
*/

#include "gl_local.h"

// FBO-related enums that gl_export.h does not declare yet
#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER              0x8D40
#endif
#ifndef GL_RENDERBUFFER
#define GL_RENDERBUFFER             0x8D41
#endif
#ifndef GL_COLOR_ATTACHMENT0
#define GL_COLOR_ATTACHMENT0        0x8CE0
#endif
#ifndef GL_DEPTH_ATTACHMENT
#define GL_DEPTH_ATTACHMENT         0x8D00
#endif
#ifndef GL_FRAMEBUFFER_COMPLETE
#define GL_FRAMEBUFFER_COMPLETE     0x8CD5
#endif
#ifndef GL_CLAMP_TO_EDGE
#define GL_CLAMP_TO_EDGE            0x812F
#endif

/*
Pipeline:
  3D scene -> FBO_3D (refState.width x refState.height, logical/landscape)
  2D HUD   -> FBO_2D (window_width x window_height, native portrait)
  Composite: bind backbuffer, draw a textured quad with rotation+scale that
             maps FBO_3D onto the backbuffer, then draw FBO_2D on top 1:1.

Active only when gl_fbo cvar is non-zero. When inactive, render path is
identical to upstream.
*/

static cvar_t *gl_fbo;

typedef struct fbo_target_s
{
	GLuint fbo;
	GLuint color;       // texture
	GLuint depth;       // renderbuffer (only for 3D)
	int    w, h;
	qboolean has_depth;
} fbo_target_t;

typedef struct fbo_state_s
{
	qboolean    initialized;
	qboolean    active;          // cached gl_fbo->value at frame start
	fbo_target_t scene;
	fbo_target_t hud;
	int         saved_target;    // 0 (backbuffer), scene.fbo or hud.fbo

	// composite shader
	GLhandleARB prog;
	GLint       u_mvp;
	GLint       u_tex;
	GLint       a_pos;
	GLint       a_uv;
} fbo_state_t;

static fbo_state_t fs;

// =====================================================================
// composite shader (GLSL 100 / desktop 110 compatible)
// =====================================================================
static const char *fbo_vert_src =
	"attribute vec2 a_pos;\n"
	"attribute vec2 a_uv;\n"
	"uniform mat4 u_mvp;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  gl_Position = u_mvp * vec4(a_pos,0.0,1.0);\n"
	"  v_uv = a_uv;\n"
	"}\n";

static const char *fbo_frag_src =
	"#ifdef GL_ES\n"
	"precision mediump float;\n"
	"#endif\n"
	"uniform sampler2D u_tex;\n"
	"varying vec2 v_uv;\n"
	"void main(){\n"
	"  gl_FragColor = texture2D(u_tex, v_uv);\n"
	"}\n";

static GLhandleARB FBO_CompileShader( GLenum type, const char *src )
{
	GLhandleARB sh = pglCreateShaderObjectARB( type );
	GLint ok = 0, len;

	pglShaderSourceARB( sh, 1, (const GLcharARB **)&src, NULL );
	pglCompileShaderARB( sh );
	pglGetObjectParameterivARB( sh, GL_OBJECT_COMPILE_STATUS_ARB, &ok );
	if( !ok )
	{
		char log[1024] = {0};
		pglGetInfoLogARB( sh, sizeof( log ) - 1, &len, log );
		gEngfuncs.Con_Printf( S_ERROR "FBO: shader compile failed: %s\n", log );
		pglDeleteObjectARB( sh );
		return 0;
	}
	return sh;
}

static qboolean FBO_BuildProgram( void )
{
	GLhandleARB vs, fs_, p;
	GLint ok = 0, len;

	vs = FBO_CompileShader( GL_VERTEX_SHADER_ARB, fbo_vert_src );
	if( !vs ) return false;
	fs_ = FBO_CompileShader( GL_FRAGMENT_SHADER_ARB, fbo_frag_src );
	if( !fs_ ) { pglDeleteObjectARB( vs ); return false; }

	p = pglCreateProgramObjectARB();
	pglAttachObjectARB( p, vs );
	pglAttachObjectARB( p, fs_ );
	// fixed attribute locations
	pglBindAttribLocationARB( p, 0, "a_pos" );
	pglBindAttribLocationARB( p, 1, "a_uv" );
	pglLinkProgramARB( p );
	pglDeleteObjectARB( vs );
	pglDeleteObjectARB( fs_ );

	pglGetObjectParameterivARB( p, GL_OBJECT_LINK_STATUS_ARB, &ok );
	if( !ok )
	{
		char log[1024] = {0};
		pglGetInfoLogARB( p, sizeof( log ) - 1, &len, log );
		gEngfuncs.Con_Printf( S_ERROR "FBO: program link failed: %s\n", log );
		pglDeleteObjectARB( p );
		return false;
	}

	fs.prog = p;
	fs.a_pos = 0;
	fs.a_uv  = 1;
	fs.u_mvp = pglGetUniformLocationARB( p, "u_mvp" );
	fs.u_tex = pglGetUniformLocationARB( p, "u_tex" );
	return true;
}

// =====================================================================
// target creation / resize
// =====================================================================
static void FBO_DestroyTarget( fbo_target_t *t )
{
	if( t->fbo )   pglDeleteFramebuffers( 1, &t->fbo );
	if( t->color ) pglDeleteTextures( 1, &t->color );
	if( t->depth ) pglDeleteRenderbuffers( 1, &t->depth );
	memset( t, 0, sizeof( *t ));
}

static qboolean FBO_CreateTarget( fbo_target_t *t, int w, int h, qboolean depth )
{
	GLenum status;

	if( w <= 0 || h <= 0 )
		return false;

	FBO_DestroyTarget( t );

	pglGenFramebuffers( 1, &t->fbo );
	pglBindFramebuffer( GL_FRAMEBUFFER, t->fbo );

	pglGenTextures( 1, &t->color );
	pglBindTexture( GL_TEXTURE_2D, t->color );
	pglTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	pglTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	pglFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0 );

	if( depth )
	{
		pglGenRenderbuffers( 1, &t->depth );
		pglBindRenderbuffer( GL_RENDERBUFFER, t->depth );
		pglRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h );
		pglFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth );
		pglBindRenderbuffer( GL_RENDERBUFFER, 0 );
	}

	status = pglCheckFramebufferStatus( GL_FRAMEBUFFER );
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );

	if( status != GL_FRAMEBUFFER_COMPLETE )
	{
		gEngfuncs.Con_Printf( S_ERROR "FBO: target %dx%d incomplete (0x%x)\n", w, h, status );
		FBO_DestroyTarget( t );
		return false;
	}

	t->w = w;
	t->h = h;
	t->has_depth = depth;
	return true;
}

// =====================================================================
// public API
// =====================================================================
void R_FBO_Init( void )
{
	if( fs.initialized )
		return;

	gl_fbo = gEngfuncs.Cvar_Get( "gl_fbo", "1", FCVAR_ARCHIVE,
		"render via offscreen FBO (1 = on, 0 = legacy direct rendering)" );

	if( !pglGenFramebuffers || !pglCreateProgramObjectARB )
	{
		gEngfuncs.Con_Printf( S_WARN "FBO: required GL functions missing, disabling\n" );
		gEngfuncs.Cvar_Set( "gl_fbo", "0" );
	}
	else if( !FBO_BuildProgram( ))
	{
		gEngfuncs.Cvar_Set( "gl_fbo", "0" );
	}

	// MSAA on the default framebuffer is incompatible with our offscreen
	// path. Disable it when FBO is active.
	if( gl_fbo->value && glConfig.max_multisamples > 1 )
	{
		gEngfuncs.Con_Printf( S_NOTE "FBO: disabling MSAA (incompatible)\n" );
		gEngfuncs.Cvar_Set( "gl_msaa_samples", "0" );
	}

	fs.initialized = true;
}

void R_FBO_Shutdown( void )
{
	if( !fs.initialized )
		return;

	FBO_DestroyTarget( &fs.scene );
	FBO_DestroyTarget( &fs.hud );
	if( fs.prog )
	{
		pglDeleteObjectARB( fs.prog );
		fs.prog = 0;
	}
	memset( &fs, 0, sizeof( fs ));
}

qboolean R_FBO_IsActive( void )
{
	return fs.active;
}

void R_FBO_BindDefault( void )
{
	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

static void FBO_EnsureSize( void )
{
	int sw = gpGlobals->width;
	int sh = gpGlobals->height;
	int hw = gpGlobals->window_width  ? gpGlobals->window_width  : sw;
	int hh = gpGlobals->window_height ? gpGlobals->window_height : sh;

	if( fs.scene.w != sw || fs.scene.h != sh )
		FBO_CreateTarget( &fs.scene, sw, sh, true );

	if( fs.hud.w != hw || fs.hud.h != hh )
		FBO_CreateTarget( &fs.hud, hw, hh, false );
}

// Called at the start of every frame. Caches gl_fbo and (re)allocates targets.
void R_FBO_FrameBegin( void )
{
	fs.active = false;
	if( !fs.initialized || !gl_fbo || gl_fbo->value <= 0.0f || !fs.prog )
		return;

	FBO_EnsureSize();
	if( !fs.scene.fbo || !fs.hud.fbo )
		return;

	fs.active = true;
	fs.saved_target = 0;
}

void R_FBO_BindScene( void )
{
	if( !fs.active ) return;
	pglBindFramebuffer( GL_FRAMEBUFFER, fs.scene.fbo );
	fs.saved_target = fs.scene.fbo;
}

void R_FBO_Bind2D( void )
{
	if( !fs.active ) return;
	pglBindFramebuffer( GL_FRAMEBUFFER, fs.hud.fbo );
	fs.saved_target = fs.hud.fbo;
}

int R_FBO_Get2DWidth( void )
{
	if( fs.active && fs.hud.w ) return fs.hud.w;
	return gpGlobals->window_width ? gpGlobals->window_width : gpGlobals->width;
}

int R_FBO_Get2DHeight( void )
{
	if( fs.active && fs.hud.h ) return fs.hud.h;
	return gpGlobals->window_height ? gpGlobals->window_height : gpGlobals->height;
}

// =====================================================================
// composite: bind backbuffer, draw scene (rotated) + hud (1:1) as quads
// =====================================================================
static void FBO_MakeRotMatrix( float m[16], ref_screen_rotation_t rot )
{
	float c, s;

	memset( m, 0, sizeof( float ) * 16 );
	m[15] = 1.0f;
	m[10] = 1.0f;

	switch( rot )
	{
	case REF_ROTATE_CW:   c =  0; s =  1; break; // +90 CW
	case REF_ROTATE_UD:   c = -1; s =  0; break; // 180
	case REF_ROTATE_CCW:  c =  0; s = -1; break; // -90
	default:              c =  1; s =  0; break; // none
	}
	// column-major 2D rotation in XY plane
	m[0] = c;  m[1] = s;
	m[4] = -s; m[5] = c;
}

static void FBO_DrawQuad( GLuint tex, const float mvp[16], qboolean blend )
{
	// fullscreen NDC quad: pos (xy) + uv (xy)
	static const GLfloat verts[] = {
		-1.f, -1.f,  0.f, 0.f,
		 1.f, -1.f,  1.f, 0.f,
		-1.f,  1.f,  0.f, 1.f,
		 1.f,  1.f,  1.f, 1.f,
	};

	pglUseProgramObjectARB( fs.prog );
	pglUniformMatrix4fvARB( fs.u_mvp, 1, GL_FALSE, mvp );

	pglActiveTextureARB( GL_TEXTURE0_ARB );
	pglBindTexture( GL_TEXTURE_2D, tex );
	pglUniform1iARB( fs.u_tex, 0 );

	pglEnableVertexAttribArrayARB( fs.a_pos );
	pglEnableVertexAttribArrayARB( fs.a_uv );
	pglVertexAttribPointerARB( fs.a_pos, 2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*4, verts );
	pglVertexAttribPointerARB( fs.a_uv,  2, GL_FLOAT, GL_FALSE, sizeof(GLfloat)*4, verts + 2 );

	if( blend )
	{
		pglEnable( GL_BLEND );
		pglBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	}
	else
	{
		pglDisable( GL_BLEND );
	}

	pglDisable( GL_DEPTH_TEST );
	pglDepthMask( GL_FALSE );

	pglDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	pglDisableVertexAttribArrayARB( fs.a_pos );
	pglDisableVertexAttribArrayARB( fs.a_uv );
	pglUseProgramObjectARB( 0 );
}

void R_FBO_Composite( void )
{
	float mvp_scene[16], mvp_hud[16];
	int ww, wh;

	if( !fs.active ) return;

	ww = gpGlobals->window_width  ? gpGlobals->window_width  : gpGlobals->width;
	wh = gpGlobals->window_height ? gpGlobals->window_height : gpGlobals->height;

	pglBindFramebuffer( GL_FRAMEBUFFER, 0 );
	pglViewport( 0, 0, ww, wh );
	pglClearColor( 0.f, 0.f, 0.f, 1.f );
	pglClear( GL_COLOR_BUFFER_BIT );

	// 3D scene rotated to match window orientation
	FBO_MakeRotMatrix( mvp_scene, tr.rotation );
	FBO_DrawQuad( fs.scene.color, mvp_scene, false );

	// 2D HUD always identity (rendered at native window size)
	FBO_MakeRotMatrix( mvp_hud, REF_ROTATE_NONE );
	FBO_DrawQuad( fs.hud.color, mvp_hud, true );

	// leave clean state for next frame
	pglBindTexture( GL_TEXTURE_2D, 0 );
	pglDepthMask( GL_TRUE );
	pglEnable( GL_DEPTH_TEST );
	pglDisable( GL_BLEND );

	fs.active = false;
}
