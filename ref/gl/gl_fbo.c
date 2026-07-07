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
#include <GLES3/gl3.h>

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
// r_3d_scale is owned by the engine (vid_common.c) now — it is applied
// inside VID_SetDisplayTransform so refState.width/height already carries
// the scaled scene FBO dimensions by the time we see it here.

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
	GLuint      prog;
	GLuint      vbo;             // static fullscreen quad
	GLuint      vao;             // attribute layout for prog
	GLint       u_mvp;
	GLint       u_tex;
	GLint       u_gamma;
	GLint       u_brightness;
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
	"  // xash bakes gamma into textures at upload time via texgammatable;\n"
	"  // do NOT apply an extra gamma curve at composite or the picture\n"
	"  // gets washed out. If the on-device image is still too dark, the\n"
	"  // fault is somewhere in the texture upload path on gles3compat.\n"
	"  gl_FragColor = texture2D(u_tex, v_uv);\n"
	"}\n";

static GLhandleARB FBO_CompileShader( GLenum type, const char *src )
{
	GLuint sh = glCreateShader( type );
	GLint ok = 0;

	glShaderSource( sh, 1, &src, NULL );
	glCompileShader( sh );
	glGetShaderiv( sh, GL_COMPILE_STATUS, &ok );
	if( !ok )
	{
		char log[1024] = {0};
		glGetShaderInfoLog( sh, sizeof( log ) - 1, NULL, log );
		gEngfuncs.Con_Printf( S_ERROR "FBO: shader compile failed: %s\n", log );
		glDeleteShader( sh );
		return 0;
	}
	return sh;
}

static qboolean FBO_BuildProgram( void )
{
	GLuint vs, fs_, p;
	GLint ok = 0;

	vs = FBO_CompileShader( GL_VERTEX_SHADER, fbo_vert_src );
	if( !vs ) return false;
	fs_ = FBO_CompileShader( GL_FRAGMENT_SHADER, fbo_frag_src );
	if( !fs_ ) { glDeleteShader( vs ); return false; }

	p = glCreateProgram();
	glAttachShader( p, vs );
	glAttachShader( p, fs_ );
	glBindAttribLocation( p, 0, "a_pos" );
	glBindAttribLocation( p, 1, "a_uv" );
	glLinkProgram( p );
	glDeleteShader( vs );
	glDeleteShader( fs_ );

	glGetProgramiv( p, GL_LINK_STATUS, &ok );
	if( !ok )
	{
		char log[1024] = {0};
		glGetProgramInfoLog( p, sizeof( log ) - 1, NULL, log );
		gEngfuncs.Con_Printf( S_ERROR "FBO: program link failed: %s\n", log );
		glDeleteProgram( p );
		return false;
	}

	fs.prog = p;
	fs.a_pos = 0;
	fs.a_uv  = 1;
	fs.u_mvp = glGetUniformLocation( p, "u_mvp" );
	fs.u_tex = glGetUniformLocation( p, "u_tex" );
	fs.u_gamma      = -1;
	fs.u_brightness = -1;

	// Mali GLES3 silently refuses client-array glVertexAttribPointer (no
	// error, no draw). Allocate our own VBO + VAO so the quad always goes
	// through the buffered path.
	{
		static const GLfloat verts[] = {
			-1.f, -1.f,  0.f, 0.f,
			 1.f, -1.f,  1.f, 0.f,
			-1.f,  1.f,  0.f, 1.f,
			 1.f,  1.f,  1.f, 1.f,
		};
		GLint prev_vao = 0, prev_vbo = 0;
		glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &prev_vao );
		glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prev_vbo );

		glGenBuffers( 1, &fs.vbo );
		glBindBuffer( GL_ARRAY_BUFFER, fs.vbo );
		glBufferData( GL_ARRAY_BUFFER, sizeof( verts ), verts, GL_STATIC_DRAW );

		glGenVertexArrays( 1, &fs.vao );
		glBindVertexArray( fs.vao );
		glEnableVertexAttribArray( fs.a_pos );
		glEnableVertexAttribArray( fs.a_uv );
		glVertexAttribPointer( fs.a_pos, 2, GL_FLOAT, GL_FALSE,
			sizeof( GLfloat ) * 4, (const void *)0 );
		glVertexAttribPointer( fs.a_uv,  2, GL_FLOAT, GL_FALSE,
			sizeof( GLfloat ) * 4, (const void *)( sizeof( GLfloat ) * 2 ));

		glBindVertexArray( (GLuint)prev_vao );
		glBindBuffer( GL_ARRAY_BUFFER, (GLuint)prev_vbo );
	}

	gEngfuncs.Con_Printf( S_NOTE "FBO[link]: prog=%u u_mvp=%d u_tex=%d a_pos=%d a_uv=%d vao=%u vbo=%u\n",
		fs.prog, fs.u_mvp, fs.u_tex, fs.a_pos, fs.a_uv, fs.vao, fs.vbo );
	return true;
}

// =====================================================================
// target creation / resize
// =====================================================================
static void FBO_DestroyTarget( fbo_target_t *t )
{
	if( t->fbo )   glDeleteFramebuffers( 1, &t->fbo );
	if( t->color ) glDeleteTextures( 1, &t->color );
	if( t->depth ) glDeleteRenderbuffers( 1, &t->depth );
	memset( t, 0, sizeof( *t ));
}

static qboolean FBO_CreateTarget( fbo_target_t *t, int w, int h, qboolean depth )
{
	printf("FBO_CreateTarget: Call create FBO target: %ix%i\n", w, h);
	GLenum status;

	if( w <= 0 || h <= 0 )
		return false;

	FBO_DestroyTarget( t );

	glGenFramebuffers( 1, &t->fbo );
	glBindFramebuffer( GL_FRAMEBUFFER, t->fbo );

	glGenTextures( 1, &t->color );
	glBindTexture( GL_TEXTURE_2D, t->color );
	glTexImage2D( GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE );
	glTexParameteri( GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE );
	glFramebufferTexture2D( GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, t->color, 0 );

	if( depth )
	{
		glGenRenderbuffers( 1, &t->depth );
		glBindRenderbuffer( GL_RENDERBUFFER, t->depth );
		glRenderbufferStorage( GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h );
		glFramebufferRenderbuffer( GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, t->depth );
		glBindRenderbuffer( GL_RENDERBUFFER, 0 );
	}

	status = glCheckFramebufferStatus( GL_FRAMEBUFFER );
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );

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
	printf("R_FBO_Init: Start init FBO\n");
	if( fs.initialized )
		return;

	gl_fbo = gEngfuncs.Cvar_Get( "gl_fbo", "1", FCVAR_ARCHIVE,
		"render via offscreen FBO (1 = on, 0 = legacy direct rendering)" );
	if( !FBO_BuildProgram( ))
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
	if( fs.vao ) { glDeleteVertexArrays( 1, &fs.vao ); fs.vao = 0; }
	if( fs.vbo ) { glDeleteBuffers( 1, &fs.vbo ); fs.vbo = 0; }
	if( fs.prog )
	{
		glDeleteProgram( fs.prog );
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
	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
}

static void FBO_EnsureSize( void )
{
	// Scene FBO size = refState.width/height directly. VID_SetDisplayTransform
	// has already applied both the rotation swap and the r_3d_scale multiplier
	// on the engine side, so gpGlobals->width/height IS the scene FBO
	// dimensions. Composite/input layers translate to/from window pixels via
	// refState.window_width/height and refState.scale_x/y.
	int sw = gpGlobals->width;
	int sh = gpGlobals->height;

	if( fs.scene.w != sw || fs.scene.h != sh )
		FBO_CreateTarget( &fs.scene, sw, sh, true );
}

// Called at the start of every frame. Caches gl_fbo and (re)allocates targets.
void R_FBO_FrameBegin( void )
{
	fs.active = false;
	if( !fs.initialized || !gl_fbo || gl_fbo->value <= 0.0f || !fs.prog )
		return;

	FBO_EnsureSize();
	if( !fs.scene.fbo )
		return;

	fs.active = true;
	fs.saved_target = 0;
}

void R_FBO_BindScene( void )
{
	if( !fs.active ) return;
	glBindFramebuffer( GL_FRAMEBUFFER, fs.scene.fbo );
	// Lock alpha at 1.0 across the whole 3D pass. Studio models and various
	// world shaders output whatever alpha they feel like (often 0), which
	// otherwise leaks into the backbuffer and the Wayland compositor sees
	// the window as translucent through those pixels.
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE );
	fs.saved_target = fs.scene.fbo;
}

int R_FBO_GetSceneWidth( void )
{
	if( fs.active && fs.scene.w ) return fs.scene.w;
	return gpGlobals->width;
}

int R_FBO_GetSceneHeight( void )
{
	if( fs.active && fs.scene.h ) return fs.scene.h;
	return gpGlobals->height;
}

void R_FBO_Bind2D( void )
{
	if( !fs.active ) return;
	// Single-FBO pipeline: 2D renders into the scene FBO right on top of
	// the 3D world. Sharing the same target avoids all the alpha/blend
	// mismatches we had with a separate HUD FBO (HL sprites' bilinear
	// half-alpha halos, damage vignettes, magenta bleed-through, etc.).
	// Alpha mask stays the same as during the 3D pass so backbuffer alpha
	// remains 1.0 for the compositor.
	glBindFramebuffer( GL_FRAMEBUFFER, fs.scene.fbo );
	glColorMask( GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE );
	fs.saved_target = fs.scene.fbo;
}

int R_FBO_Get2DWidth( void )
{
	// 2D lives inside scene FBO — return scene dimensions so R_Set2DMode
	// builds the right ortho/viewport and HUD elements land where they
	// should on the (possibly r_3d_scale'd) scene texture.
	if( fs.active && fs.scene.w ) return fs.scene.w;
	return gpGlobals->width;
}

int R_FBO_Get2DHeight( void )
{
	if( fs.active && fs.scene.h ) return fs.scene.h;
	return gpGlobals->height;
}

// =====================================================================
// composite: bind backbuffer, draw scene (rotated) + hud (1:1) as quads
// =====================================================================
static void FBO_CheckGL( const char *where )
{
	GLenum e;
	while(( e = glGetError() ) != GL_NO_ERROR )
		gEngfuncs.Con_Printf( S_ERROR "FBO[GL]: 0x%x at %s\n", e, where );
}

static void FBO_MakeRotMatrix( float m[16], ref_screen_rotation_t rot )
{
	// printf("Call FBO_MakeRotMatrix: %i\n", rot);
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
	glUseProgram( fs.prog );
	glUniformMatrix4fv( fs.u_mvp, 1, GL_FALSE, mvp );

	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, tex );
	glUniform1i( fs.u_tex, 0 );

	glBindVertexArray( fs.vao );

	if( blend )
	{
		glEnable( GL_BLEND );
		glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	}
	else
	{
		glDisable( GL_BLEND );
	}

	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );

	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );
	FBO_CheckGL( "drawquad: draw" );

	{
		static int once = 0;
		if( !once )
		{
			GLint p = 0, bt = 0, vaq = 0, bbuf = 0;
			once = 1;
			glGetIntegerv( GL_CURRENT_PROGRAM, &p );
			glGetIntegerv( GL_TEXTURE_BINDING_2D, &bt );
			glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &vaq );
			glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &bbuf );
			gEngfuncs.Con_Printf( S_NOTE "FBO[draw]: prog=%d tex=%d vao=%d vbo=%d mvp[0..3]=%.2f %.2f %.2f %.2f\n",
				p, bt, vaq, bbuf, mvp[0], mvp[1], mvp[2], mvp[3] );
		}
	}

	glBindVertexArray( 0 );
}

void R_FBO_Composite( void )
{
	// STEP C: blit scene + HUD FBOs to the backbuffer, rotating both by
	// tr.rotation. Both FBOs are landscape-sized (= refState.width/height);
	// the backbuffer is native window orientation (e.g. portrait). The
	// rotation matrix maps the landscape quad onto the portrait window.
	float mvp[16];
	int ww, wh;
	GLint prev_prog = 0, prev_vao = 0, prev_buf = 0;
	static int once = 0;

	if( !fs.active ) return;

	ww = gpGlobals->window_width  ? gpGlobals->window_width  : gpGlobals->width;
	wh = gpGlobals->window_height ? gpGlobals->window_height : gpGlobals->height;

	glGetIntegerv( GL_CURRENT_PROGRAM,      &prev_prog );
	glGetIntegerv( GL_VERTEX_ARRAY_BINDING, &prev_vao );
	glGetIntegerv( GL_ARRAY_BUFFER_BINDING, &prev_buf );

	glBindFramebuffer( GL_FRAMEBUFFER, 0 );
	glViewport( 0, 0, ww, wh );
	glClearColor( 0.f, 0.f, 0.f, 1.f );
	glClear( GL_COLOR_BUFFER_BIT );

	glDisable( GL_DEPTH_TEST );
	glDepthMask( GL_FALSE );
	glDisable( GL_CULL_FACE );

	FBO_MakeRotMatrix( mvp, tr.rotation );

	// Single quad — scene FBO already contains 3D + HUD composited together
	// by the game itself. No second HUD-quad pass, no HUD-specific blend
	// hacks, no alpha halos around HL HUD sprites.
	glEnable( GL_BLEND );
	glBlendFunc( GL_ONE, GL_ONE_MINUS_SRC_ALPHA );
	glUseProgram( fs.prog );
	glUniformMatrix4fv( fs.u_mvp, 1, GL_FALSE, mvp );
	glActiveTexture( GL_TEXTURE0 );
	glBindTexture( GL_TEXTURE_2D, fs.scene.color );
	glUniform1i( fs.u_tex, 0 );
	glBindVertexArray( fs.vao );
	glDrawArrays( GL_TRIANGLE_STRIP, 0, 4 );

	if( !once )
	{
		once = 1;
		FBO_CheckGL( "STEP C: composite" );
		gEngfuncs.Con_Printf( S_NOTE "FBO[C]: win=%dx%d scene=%ux%u rot=%d\n",
			ww, wh, fs.scene.w, fs.scene.h, (int)tr.rotation );
	}

	// restore state for next frame's legacy/shim path
	glBindVertexArray( (GLuint)prev_vao );
	if( prev_buf ) glBindBuffer( GL_ARRAY_BUFFER, (GLuint)prev_buf );
	glUseProgram( (GLuint)prev_prog );
	glBindTexture( GL_TEXTURE_2D, 0 );
	glDepthMask( GL_TRUE );
	glEnable( GL_DEPTH_TEST );
	glDisable( GL_BLEND );

	fs.active = false;
}
