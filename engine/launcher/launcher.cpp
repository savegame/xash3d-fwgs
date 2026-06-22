/*
launcher.cpp - AuroraOS in-process ImGui launcher
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

/*
Stage A scope:
  * Create an SDL window + GLES3 context with the same attributes the
    engine will later expect.
  * Initialise ImGui (SDL2 platform + OpenGL3 renderer, ES3 mode).
  * Render one panel with a single "Continue" button.
  * Touch events are translated to mouse events so the upstream SDL2
    backend handles them without modification.
  * On Continue: shut down ImGui (releases its own VAO/VBO/program/font
    texture) but leave the SDL window and GL context alive. Engine code
    in VID_CreateWindow will see Launcher_GetWindow/GLContext non-NULL
    and skip its own creation calls — critical on AuroraOS where the
    compositor terminates apps whose window disappears even briefly.

Later stages will add: resource-path checker, mod selector, About tab.
*/

#include "launcher.h"

#include <SDL.h>
#include <SDL_video.h>
#include <SDL_syswm.h>

#include <GLES3/gl3.h>

#include "imgui.h"
#include "backends/imgui_impl_sdl2.h"
#include "backends/imgui_impl_opengl3.h"

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

namespace {

SDL_Window    *g_window  = nullptr;
SDL_GLContext  g_context = nullptr;
bool           g_owned   = false; // true while launcher still owns the window/context

// Mirror the GL attribute set the engine's gles3compat path will request
// inside R_GetSafeGLConfig (ref/gl/gl_opengl.c). Keep these in sync.
void SetGLAttributesForEngine()
{
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MAJOR_VERSION, 3 );
	SDL_GL_SetAttribute( SDL_GL_CONTEXT_MINOR_VERSION, 0 );
#ifdef SDL_HINT_VIDEO_X11_FORCE_EGL
	SDL_SetHint( SDL_HINT_VIDEO_X11_FORCE_EGL, "1" );
#endif
#ifdef SDL_HINT_OPENGL_ES_DRIVER
	SDL_SetHint( SDL_HINT_OPENGL_ES_DRIVER, "1" );
#endif

	SDL_GL_SetAttribute( SDL_GL_RED_SIZE,     8 );
	SDL_GL_SetAttribute( SDL_GL_GREEN_SIZE,   8 );
	SDL_GL_SetAttribute( SDL_GL_BLUE_SIZE,    8 );
	SDL_GL_SetAttribute( SDL_GL_ALPHA_SIZE,   8 );
	SDL_GL_SetAttribute( SDL_GL_DEPTH_SIZE,  24 );
	SDL_GL_SetAttribute( SDL_GL_STENCIL_SIZE, 8 );
	SDL_GL_SetAttribute( SDL_GL_DOUBLEBUFFER, 1 );
}

// SDL_FINGER* → SDL_MOUSE* shim. The vanilla ImGui SDL2 backend understands
// mouse events only, so we synthesise mouse motion/button events from the
// first finger. Single-touch is enough for our launcher widgets.
void RewriteTouchToMouseEvent( const SDL_Event &in, int win_w, int win_h )
{
	if( in.type != SDL_FINGERDOWN && in.type != SDL_FINGERUP && in.type != SDL_FINGERMOTION )
		return;

	// Tap coordinates arrive normalised [0..1] in window space.
	float fx = in.tfinger.x;
	float fy = in.tfinger.y;
	if( fx > 1.0f || fy > 1.0f )
	{
		// Some drivers (notably AuroraOS) report pixel coords; normalise.
		fx = fx / (float)win_w;
		fy = fy / (float)win_h;
	}
	int px = (int)( fx * win_w );
	int py = (int)( fy * win_h );

	SDL_Event out;
	SDL_zero( out );

	if( in.type == SDL_FINGERMOTION )
	{
		out.type = SDL_MOUSEMOTION;
		out.motion.timestamp = in.tfinger.timestamp;
		out.motion.windowID  = 0;
		out.motion.which     = SDL_TOUCH_MOUSEID;
		out.motion.state     = SDL_BUTTON_LMASK;
		out.motion.x         = px;
		out.motion.y         = py;
		out.motion.xrel      = 0;
		out.motion.yrel      = 0;
		SDL_PushEvent( &out );
		return;
	}

	// Down/up — emit motion first so ImGui hover state lands on the right widget,
	// then the button event itself.
	SDL_Event move;
	SDL_zero( move );
	move.type = SDL_MOUSEMOTION;
	move.motion.timestamp = in.tfinger.timestamp;
	move.motion.windowID  = 0;
	move.motion.which     = SDL_TOUCH_MOUSEID;
	move.motion.state     = SDL_BUTTON_LMASK;
	move.motion.x = px;
	move.motion.y = py;
	SDL_PushEvent( &move );

	out.type = ( in.type == SDL_FINGERDOWN ) ? SDL_MOUSEBUTTONDOWN : SDL_MOUSEBUTTONUP;
	out.button.timestamp = in.tfinger.timestamp;
	out.button.windowID  = 0;
	out.button.which     = SDL_TOUCH_MOUSEID;
	out.button.button    = SDL_BUTTON_LEFT;
	out.button.state     = ( in.type == SDL_FINGERDOWN ) ? SDL_PRESSED : SDL_RELEASED;
	out.button.clicks    = 1;
	out.button.x         = px;
	out.button.y         = py;
	SDL_PushEvent( &out );
}

void DrawLauncherUI( bool &keep_running, bool &user_quit, int win_w, int win_h )
{
	// Full-screen ImGui window (no decorations) so touch hits anywhere we want.
	ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
	ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoBringToFrontOnFocus
		| ImGuiWindowFlags_NoSavedSettings;

	ImGui::Begin( "##launcher", nullptr, flags );

	const float fs    = ImGui::GetFontSize();
	const float btn_w = fs * 14.f;
	const float btn_h = fs * 4.f;

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding,  ImVec2( fs * 1.4f, fs * 0.8f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,   ImVec2( fs * 0.5f, fs * 0.8f ));

	ImVec2 hdr = ImGui::CalcTextSize( "Xash3D launcher (AuroraOS)" );
	ImGui::SetCursorPos( ImVec2(( win_w - hdr.x ) * 0.5f, fs * 2.f ));
	ImGui::TextUnformatted( "Xash3D launcher (AuroraOS)" );

	ImGui::SetCursorPos( ImVec2(( win_w - btn_w ) * 0.5f, win_h * 0.5f - btn_h - fs * 0.4f ));
	if( ImGui::Button( "Continue", ImVec2( btn_w, btn_h )))
	{
		keep_running = false;
		user_quit = false;
	}

	ImGui::SetCursorPos( ImVec2(( win_w - btn_w ) * 0.5f, win_h * 0.5f + fs * 0.4f ));
	if( ImGui::Button( "Quit",     ImVec2( btn_w, btn_h )))
	{
		keep_running = false;
		user_quit = true;
	}

	ImGui::PopStyleVar( 2 );
	ImGui::End();
}

} // namespace

extern "C" {

launcher_result_t Launcher_Run( void )
{
	// SDL_INIT_VIDEO was already done by Platform_Init before we got here.
	// If it wasn't (e.g. someone wires us in earlier), bring it up.
	if( SDL_WasInit( SDL_INIT_VIDEO ) == 0 )
	{
		if( SDL_Init( SDL_INIT_VIDEO | SDL_INIT_EVENTS ) < 0 )
		{
			fprintf( stderr, "Launcher: SDL_Init failed: %s\n", SDL_GetError() );
			return LAUNCHER_QUIT;
		}
	}

	SetGLAttributesForEngine();

	Uint32 win_flags = SDL_WINDOW_OPENGL | SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_ALLOW_HIGHDPI;
	g_window = SDL_CreateWindow( "Xash3D",
		SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
		1080, 1920, // ignored because of FULLSCREEN_DESKTOP, but must be > 0
		win_flags );
	if( !g_window )
	{
		fprintf( stderr, "Launcher: SDL_CreateWindow failed: %s\n", SDL_GetError() );
		return LAUNCHER_QUIT;
	}

	g_context = SDL_GL_CreateContext( g_window );
	if( !g_context )
	{
		fprintf( stderr, "Launcher: SDL_GL_CreateContext failed: %s\n", SDL_GetError() );
		SDL_DestroyWindow( g_window );
		g_window = nullptr;
		return LAUNCHER_QUIT;
	}

	SDL_GL_MakeCurrent( g_window, g_context );
	SDL_GL_SetSwapInterval( 1 );

	g_owned = true;

	IMGUI_CHECKVERSION();
	ImGui::CreateContext();
	ImGuiIO &io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	io.IniFilename = nullptr; // do not write imgui.ini next to the binary

	ImGui::StyleColorsDark();

	// Read the physical DPI so we can express UI dimensions in millimetres.
	// SDL_GetDisplayDPI returns dots-per-inch; we convert to dots-per-mm.
	// Fall back to a sensible default for desktop SDL drivers that don't
	// report DPI.
	float hdpi = 96.f, vdpi = 96.f;
	SDL_GetDisplayDPI( SDL_GetWindowDisplayIndex( g_window ), nullptr, &hdpi, &vdpi );
	if( vdpi <= 1.f ) vdpi = 96.f;
	const float dots_per_mm = vdpi / 25.4f;

	// 3 mm glyph height target (with Bold weight). Clamp so a buggy/zero
	// DPI doesn't produce a microscopic or absurdly large font.
	float font_px = 3.0f * dots_per_mm;
	if( font_px < 14.f ) font_px = 14.f;
	if( font_px > 96.f ) font_px = 96.f;

	// Try to load the AuroraOS system Bold font. If absent, ImGui falls
	// back to its built-in proggy font (scaled).
	static const char *kFontPath = "/usr/share/fonts/als-hauss-variable/ALSHaussVariable-Bold.ttf";
	static const ImWchar kRanges[] = {
		0x0020, 0x00FF, // Latin-1 + punctuation
		0x0400, 0x04FF, // Cyrillic
		0x2010, 0x205E, // General Punctuation (en-dash, ellipsis, etc.)
		0,
	};

	{
		struct stat st;
		if( stat( kFontPath, &st ) == 0 )
		{
			ImFontConfig cfg;
			cfg.OversampleH = 2;
			cfg.OversampleV = 1;
			cfg.PixelSnapH  = true;
			io.Fonts->AddFontFromFileTTF( kFontPath, font_px, &cfg, kRanges );
		}
		else
		{
			// No system font — scale built-in font to roughly the target size.
			io.FontGlobalScale = font_px / 13.f;
		}
	}

	// Widget metrics scaled so finger-sized buttons feel right at the
	// device's actual DPI.
	ImGui::GetStyle().ScaleAllSizes( font_px / 13.f );

	ImGui_ImplSDL2_InitForOpenGL( g_window, g_context );
	ImGui_ImplOpenGL3_Init( "#version 300 es" );

	bool keep_running = true;
	bool user_quit    = false;

	while( keep_running )
	{
		int win_w = 0, win_h = 0;
		SDL_GetWindowSize( g_window, &win_w, &win_h );

		SDL_Event ev;
		while( SDL_PollEvent( &ev ))
		{
			ImGui_ImplSDL2_ProcessEvent( &ev );

			if( ev.type == SDL_QUIT )
			{
				keep_running = false;
				user_quit    = true;
			}
			else if( ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP || ev.type == SDL_FINGERMOTION )
			{
				RewriteTouchToMouseEvent( ev, win_w, win_h );
			}
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		DrawLauncherUI( keep_running, user_quit, win_w, win_h );

		ImGui::Render();
		glViewport( 0, 0, win_w, win_h );
		glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

		SDL_GL_SwapWindow( g_window );
	}

	// Show a single "LOADING" frame before we hand control to the engine.
	// FS_Init + first asset load can take a noticeable second or two on a
	// fresh device, and freezing the pressed-button frame looks broken.
	if( !user_quit )
	{
		int win_w = 0, win_h = 0;
		SDL_GetWindowSize( g_window, &win_w, &win_h );

		// Drain any leftover queued events so NewFrame sees a clean state.
		SDL_Event drain;
		while( SDL_PollEvent( &drain ))
			ImGui_ImplSDL2_ProcessEvent( &drain );

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ImGui::NewFrame();

		ImGui::SetNextWindowPos(  ImVec2( 0, 0 ));
		ImGui::SetNextWindowSize( ImVec2( (float)win_w, (float)win_h ));
		ImGuiWindowFlags wflags = ImGuiWindowFlags_NoTitleBar
			| ImGuiWindowFlags_NoResize
			| ImGuiWindowFlags_NoMove
			| ImGuiWindowFlags_NoCollapse
			| ImGuiWindowFlags_NoBringToFrontOnFocus
			| ImGuiWindowFlags_NoSavedSettings
			| ImGuiWindowFlags_NoScrollbar;
		ImGui::Begin( "##loading", nullptr, wflags );
		const char *txt = "ЗАГРУЗКА";
		ImVec2 sz = ImGui::CalcTextSize( txt );
		ImGui::SetCursorPos( ImVec2(( win_w - sz.x ) * 0.5f, ( win_h - sz.y ) * 0.5f ));
		ImGui::TextUnformatted( txt );
		ImGui::End();

		ImGui::Render();
		glViewport( 0, 0, win_w, win_h );
		glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );
		SDL_GL_SwapWindow( g_window );
	}

	// Tear down ImGui — it releases its own VAO/VBO/program/font texture.
	// The SDL window and GL context stay alive and become the engine's.
	ImGui_ImplOpenGL3_Shutdown();
	ImGui_ImplSDL2_Shutdown();
	ImGui::DestroyContext();

	if( user_quit )
	{
		// User asked to quit before the engine ran. Tear down our own
		// window/context.
		if( g_context ) { SDL_GL_DeleteContext( g_context ); g_context = nullptr; }
		if( g_window  ) { SDL_DestroyWindow ( g_window  );  g_window  = nullptr; }
		g_owned = false;
		return LAUNCHER_QUIT;
	}

	// Hand the bare GL context back to a clean state for the engine.
	SDL_GL_MakeCurrent( g_window, g_context );
	return LAUNCHER_CONTINUE;
}

void *Launcher_GetWindow( void )
{
	return g_owned ? (void *)g_window : nullptr;
}

void *Launcher_GetGLContext( void )
{
	return g_owned ? (void *)g_context : nullptr;
}

void Launcher_ReleaseOwnership( void )
{
	// Engine has adopted the window/context; we no longer free them.
	g_owned = false;
}

} // extern "C"
