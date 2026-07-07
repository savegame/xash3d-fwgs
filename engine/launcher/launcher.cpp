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

// -- maliit debug panel (remove once wrapper is validated) --
#include "maliit_client.h"
// -- /maliit debug panel --

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>

#include <string>
#include <vector>
#include <algorithm>

namespace {

SDL_Window    *g_window  = nullptr;
SDL_GLContext  g_context = nullptr;
bool           g_owned   = false; // true while launcher still owns the window/context

// ----------------------------------------------------------------------
// Resource path picker state
// ----------------------------------------------------------------------
struct PickerState
{
	std::string current_dir; // directory currently shown in the browser
	std::string selected;    // last confirmed pick (the resource root)
	bool        browser_open = false;
	bool        valid_pick   = false; // selected points at a working HL gamedir
};

PickerState g_picker;

// ----------------------------------------------------------------------
// Maliit debug panel state — remove after wrapper is validated.
// ----------------------------------------------------------------------
struct MaliitDbg
{
	bool  initialised   = false;
	bool  init_failed   = false;
	bool  visible       = false;
	bool  password_mode = false;
	int   content_type  = MALIIT_CONTENT_FREE_TEXT;
	int   area_x = 0, area_y = 0, area_w = 0, area_h = 0;
	char  input_buf[256] = {0};
	std::vector<std::string> log; // rolling event log
};
MaliitDbg g_maliit;

void MaliitDbgLog( const char *fmt, ... )
{
	char buf[512];
	va_list ap; va_start( ap, fmt );
	vsnprintf( buf, sizeof( buf ), fmt, ap );
	va_end( ap );
	g_maliit.log.emplace_back( buf );
	if( g_maliit.log.size() > 64 )
		g_maliit.log.erase( g_maliit.log.begin(),
			g_maliit.log.begin() + ( g_maliit.log.size() - 64 ));
}

// Maliit callbacks — run on the same thread that calls maliit_client_pump.
void MaliitDbg_OnCommit( void *, const char *utf8, int32_t rs, int32_t rl, int32_t cp )
{
	MaliitDbgLog( "commit: '%s' rs=%d rl=%d cp=%d", utf8, rs, rl, cp );
	// Append committed text to the input buffer for visual feedback.
	size_t used = strlen( g_maliit.input_buf );
	size_t room = sizeof( g_maliit.input_buf ) - 1 - used;
	if( room > 0 )
		strncat( g_maliit.input_buf, utf8, room );
}

void MaliitDbg_OnPreedit( void *, const char *utf8, int32_t rs, int32_t rl, int32_t cp )
{
	MaliitDbgLog( "preedit: '%s' rs=%d rl=%d cp=%d", utf8, rs, rl, cp );
}

void MaliitDbg_OnKey( void *, int type, int qtkey, int mods, const char *text,
                      bool autorep, int32_t count )
{
	MaliitDbgLog( "key: type=%d key=0x%x mods=0x%x text='%s' rep=%d cnt=%d",
		type, qtkey, mods, text, autorep, count );
	// Qt::Key_Backspace = 0x01000003 — react to it here for visual feedback.
	if( type == MALIIT_KEY_PRESS && qtkey == 0x01000003 )
	{
		size_t n = strlen( g_maliit.input_buf );
		if( n > 0 ) g_maliit.input_buf[n-1] = '\0';
	}
}

void MaliitDbg_OnVisibility( void *, bool v )
{
	MaliitDbgLog( "visibility: %s", v ? "shown" : "hidden" );
	g_maliit.visible = v;
}

void MaliitDbg_OnArea( void *, int32_t x, int32_t y, int32_t w, int32_t h )
{
	MaliitDbgLog( "area: x=%d y=%d w=%d h=%d", x, y, w, h );
	g_maliit.area_x = x; g_maliit.area_y = y;
	g_maliit.area_w = w; g_maliit.area_h = h;
}

void MaliitDbg_Init( void )
{
	maliit_callbacks_t cb = {};
	cb.commit_string  = MaliitDbg_OnCommit;
	cb.preedit_string = MaliitDbg_OnPreedit;
	cb.key_event      = MaliitDbg_OnKey;
	cb.visibility     = MaliitDbg_OnVisibility;
	cb.area_changed   = MaliitDbg_OnArea;
	if( maliit_client_init( &cb ))
	{
		g_maliit.initialised = true;
		MaliitDbgLog( "maliit_client_init: OK" );
	}
	else
	{
		g_maliit.init_failed = true;
		MaliitDbgLog( "maliit_client_init: FAILED (server unreachable)" );
	}
}

void MaliitDbg_Shutdown( void )
{
	if( g_maliit.initialised )
		maliit_client_shutdown();
	g_maliit.initialised = false;
}

void MaliitDbg_Pump( void )
{
	if( g_maliit.initialised )
		maliit_client_pump();
}

void DrawTab_Maliit( void )
{
	const float fs = ImGui::GetFontSize();

	if( g_maliit.init_failed )
	{
		ImGui::TextColored( ImVec4( 1, 0.4f, 0.4f, 1 ),
			"maliit-server недоступен — проверь MALIIT_SERVER_ADDRESS "
			"или что maliit-server запущен." );
	}
	else if( !g_maliit.initialised )
	{
		ImGui::TextUnformatted( "maliit-client не инициализирован." );
	}
	else
	{
		ImGui::TextColored( ImVec4( 0.4f, 0.9f, 0.4f, 1 ),
			"maliit-client готов. Клавиатура: %s.",
			g_maliit.visible ? "показана" : "скрыта" );
	}

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	ImGui::TextUnformatted( "Тест ввода:" );
	if( ImGui::InputText( "##maliit_input", g_maliit.input_buf,
	                      sizeof( g_maliit.input_buf )))
	{
		// User edited via physical keyboard — nothing to do; the buffer
		// is the source of truth for commits from the IM as well.
	}
	// When the input field gets keyboard focus, tell maliit we want text.
	if( ImGui::IsItemActivated() && g_maliit.initialised )
	{
		maliit_client_focus_in();
		maliit_client_show();
	}
	if( ImGui::IsItemDeactivated() && g_maliit.initialised )
	{
		maliit_client_focus_out();
		maliit_client_hide();
	}

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	if( ImGui::Button( "Показать клавиатуру" ))
		if( g_maliit.initialised ) maliit_client_show();
	ImGui::SameLine();
	if( ImGui::Button( "Скрыть клавиатуру" ))
		if( g_maliit.initialised ) maliit_client_hide();
	ImGui::SameLine();
	if( ImGui::Button( "Reset IM" ))
		if( g_maliit.initialised ) maliit_client_reset();

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	if( ImGui::Checkbox( "Password mode (hidden + no predict)",
	                     &g_maliit.password_mode ))
	{
		if( g_maliit.initialised )
		{
			maliit_client_set_password_mode( g_maliit.password_mode );
			maliit_client_focus_in(); // flush new settings
		}
	}

	const char *ct_names[] = { "FreeText", "Number", "Phone", "Email", "URL", "Custom" };
	if( ImGui::Combo( "Content type", &g_maliit.content_type,
	                  ct_names, IM_ARRAYSIZE( ct_names )))
	{
		if( g_maliit.initialised )
		{
			maliit_client_set_content_type(
				(maliit_content_type_t)g_maliit.content_type );
			maliit_client_focus_in();
		}
	}

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));
	ImGui::Text( "IM area: %d,%d %dx%d",
		g_maliit.area_x, g_maliit.area_y,
		g_maliit.area_w, g_maliit.area_h );

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));
	ImGui::TextUnformatted( "Event log:" );
	ImGui::BeginChild( "##maliit_log", ImVec2( 0, fs * 12.f ), true );
	for( const auto &line : g_maliit.log )
		ImGui::TextUnformatted( line.c_str() );
	if( ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.f )
		ImGui::SetScrollHereY( 1.0f );
	ImGui::EndChild();

	if( ImGui::Button( "Clear log" )) g_maliit.log.clear();
}

// SDL touch-drag → ImGui scroll. We DO NOT translate finger motion into
// mouse motion (would cause buttons to drag-select). Instead a drag
// becomes MouseWheel pulses applied to whatever ImGui window is hovered;
// a tap (no significant motion) emits a single mouse-button down+up at
// the original finger position.
struct TouchState
{
	bool  active     = false;
	float start_x    = 0.f, start_y = 0.f;
	float last_x     = 0.f, last_y = 0.f;
	float total_dist = 0.f;
};
TouchState g_touch;
float      g_scroll_pending_px = 0.f;

// ----------------------------------------------------------------------
// Validation: does `dir` look like a Half-Life resource root?
// ----------------------------------------------------------------------
bool PathExists( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0;
}

bool IsDir( const std::string &p )
{
	struct stat st;
	return stat( p.c_str(), &st ) == 0 && S_ISDIR( st.st_mode );
}

bool ValidateResourceDir( const std::string &dir )
{
	if( !IsDir( dir )) return false;
	if( PathExists( dir + "/valve/liblist.gam" )) return true;
	if( PathExists( dir + "/valve/gameinfo.txt" )) return true;
	// Tolerate just having a "valve" subdir — engine will complain later
	// but the user clearly picked something Half-Life-shaped.
	if( IsDir( dir + "/valve" )) return true;
	return false;
}

// ----------------------------------------------------------------------
// Persistent config: stores the last picked resource path.
//   $XDG_CONFIG_HOME/xash3d-fwgs/launcher.conf  (or ~/.config/...)
// ----------------------------------------------------------------------
// On AuroraOS the package sandbox only allows writes under
//   ~/.local/share/<org>/<app>
//   ~/.cache/<org>/<app>
//   ~/.config/<org>/<app>
// Compose these from the build-time defines so a packager can drop in
// their own organisation/application identifier via wscript options.
#ifndef XASH_AURORAOS_ORGNAME
#define XASH_AURORAOS_ORGNAME "org.xash"
#endif
#ifndef XASH_AURORAOS_APPNAME
#define XASH_AURORAOS_APPNAME "xash"
#endif

std::string HomeDir()
{
	const char *home = getenv( "HOME" );
	if( !home || !*home )
	{
		passwd *pw = getpwuid( getuid());
		home = pw ? pw->pw_dir : "/tmp";
	}
	return std::string( home );
}

std::string ConfigDir()
{
	// $XDG_CONFIG_HOME normally maps to ~/.config. On AuroraOS the sandbox
	// constrains writes under ~/.config/<org>/<app>.
	// const char *xdg = getenv( "XDG_CONFIG_HOME" );
	std::string base = HomeDir() + "/.config";
	base += "/" XASH_AURORAOS_ORGNAME "/" XASH_AURORAOS_APPNAME;
	return base;
}

std::string DataDir()
{
	const char *xdg = getenv( "XDG_DATA_HOME" );
	std::string base = ( xdg && *xdg ) ? std::string( xdg )
	                                   : HomeDir() + "/.local/share";
	return base + "/" XASH_AURORAOS_ORGNAME "/" XASH_AURORAOS_APPNAME;
}

std::string CacheDir()
{
	const char *xdg = getenv( "XDG_CACHE_HOME" );
	std::string base = ( xdg && *xdg ) ? std::string( xdg )
	                                   : HomeDir() + "/.cache";
	return base + "/" XASH_AURORAOS_ORGNAME "/" XASH_AURORAOS_APPNAME;
}

void MakeDirsP( const std::string &path )
{
	std::string acc;
	for( size_t i = 1; i <= path.size(); ++i )
	{
		if( i == path.size() || path[i] == '/' )
		{
			acc.assign( path, 0, i );
			if( !acc.empty()) mkdir( acc.c_str(), 0755 );
		}
	}
}

std::string ConfigFile() { return ConfigDir() + "/launcher.conf"; }

// launcher.conf is a trivial key=value store, one entry per line.
// Recognised keys:
//   path       — last picked resource root
//   r_3d_scale — offscreen render scale factor
struct LauncherSettings
{
	std::string path;
	float       r_3d_scale = 0.5f;
};

LauncherSettings g_settings;

std::string TrimStr( const std::string &s )
{
	size_t a = 0, b = s.size();
	while( a < b && ( s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n' )) ++a;
	while( b > a && ( s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n' )) --b;
	return s.substr( a, b - a );
}

void LoadSettings()
{
	FILE *f = fopen( ConfigFile().c_str(), "rb" );
	if( !f ) return;
	char line[4096];
	// First-line backwards compat: if the file predates key=value it was a
	// bare path. Detect by absence of '='.
	while( fgets( line, sizeof( line ), f ))
	{
		std::string s = TrimStr( line );
		if( s.empty() || s[0] == '#' ) continue;
		size_t eq = s.find( '=' );
		if( eq == std::string::npos )
		{
			// legacy bare-path file
			g_settings.path = s;
			continue;
		}
		std::string k = TrimStr( s.substr( 0, eq ));
		std::string v = TrimStr( s.substr( eq + 1 ));
		if( k == "path" )       g_settings.path = v;
		else if( k == "r_3d_scale" ) g_settings.r_3d_scale = (float)atof( v.c_str());
	}
	fclose( f );
	if( g_settings.r_3d_scale < 0.25f ) g_settings.r_3d_scale = 0.25f;
	if( g_settings.r_3d_scale > 2.0f )  g_settings.r_3d_scale = 2.0f;
}

void SaveSettings()
{
	MakeDirsP( ConfigDir());
	FILE *f = fopen( ConfigFile().c_str(), "wb" );
	if( !f ) return;
	fprintf( f, "path=%s\n",       g_settings.path.c_str());
	fprintf( f, "r_3d_scale=%.3f\n", g_settings.r_3d_scale );
	fclose( f );
}

// Backwards-compat wrappers used by the picker.
std::string LoadConfigPath() { return g_settings.path; }
void SaveConfigPath( const std::string &p )
{
	g_settings.path = p;
	SaveSettings();
}

// Best-effort fallback when no config saved yet.
std::string DefaultStartDir()
{
	const char *home = getenv( "HOME" );
	if( !home || !*home )
	{
		passwd *pw = getpwuid( getuid());
		home = pw ? pw->pw_dir : "/";
	}
	// Aurora maps user storage to ~/Downloads / ~/Documents.
	if( IsDir( std::string( home ) + "/Downloads" )) return std::string( home ) + "/Downloads";
	if( IsDir( std::string( home ) + "/Documents" )) return std::string( home ) + "/Documents";
	return std::string( home );
}

// ----------------------------------------------------------------------
// Touch event handling — tap vs drag decision.
// ----------------------------------------------------------------------
void ProcessTouchEvent( const SDL_Event &in, int win_w, int win_h )
{
	if( in.type != SDL_FINGERDOWN && in.type != SDL_FINGERUP && in.type != SDL_FINGERMOTION )
		return;

	float fx = in.tfinger.x;
	float fy = in.tfinger.y;
	if( fx > 1.0f || fy > 1.0f )
	{
		fx /= (float)win_w;
		fy /= (float)win_h;
	}
	float px = fx * (float)win_w;
	float py = fy * (float)win_h;

	const float TAP_THRESHOLD = 16.f;

	if( in.type == SDL_FINGERDOWN )
	{
		g_touch.active     = true;
		g_touch.start_x    = px;
		g_touch.start_y    = py;
		g_touch.last_x     = px;
		g_touch.last_y     = py;
		g_touch.total_dist = 0.f;

		// Move ImGui mouse to finger position so hover-test picks the right
		// window for any subsequent scroll deltas.
		SDL_Event mv = {};
		mv.type = SDL_MOUSEMOTION;
		mv.motion.timestamp = in.tfinger.timestamp;
		mv.motion.which     = SDL_TOUCH_MOUSEID;
		mv.motion.x         = (int)px;
		mv.motion.y         = (int)py;
		SDL_PushEvent( &mv );
	}
	else if( in.type == SDL_FINGERMOTION && g_touch.active )
	{
		float dx = px - g_touch.last_x;
		float dy = py - g_touch.last_y;
		g_touch.total_dist += sqrtf( dx * dx + dy * dy );
		g_touch.last_x = px;
		g_touch.last_y = py;
		// Once the finger has clearly moved we treat the gesture as scroll.
		if( g_touch.total_dist >= TAP_THRESHOLD )
			g_scroll_pending_px += dy;
	}
	else if( in.type == SDL_FINGERUP && g_touch.active )
	{
		if( g_touch.total_dist < TAP_THRESHOLD )
		{
			// Tap — emit down+up at original position so ImGui registers a click.
			SDL_Event d = {};
			d.type = SDL_MOUSEBUTTONDOWN;
			d.button.timestamp = in.tfinger.timestamp;
			d.button.which     = SDL_TOUCH_MOUSEID;
			d.button.button    = SDL_BUTTON_LEFT;
			d.button.state     = SDL_PRESSED;
			d.button.clicks    = 1;
			d.button.x         = (int)g_touch.start_x;
			d.button.y         = (int)g_touch.start_y;
			SDL_PushEvent( &d );

			SDL_Event u = d;
			u.type = SDL_MOUSEBUTTONUP;
			u.button.state = SDL_RELEASED;
			SDL_PushEvent( &u );
		}
		g_touch.active = false;
	}
}

void ApplyPendingScroll()
{
	if( g_scroll_pending_px == 0.f ) return;
	ImGuiIO &io = ImGui::GetIO();
	// Convert pixel delta to wheel ticks. ImGui scrolls about
	// GetFontSize() pixels per tick; tuning factor for finger feel.
	const float px_per_tick = 28.f;
	io.MouseWheel += g_scroll_pending_px / px_per_tick;
	g_scroll_pending_px = 0.f;
}

// ----------------------------------------------------------------------
// Directory listing helper (subdirectories only).
// ----------------------------------------------------------------------
std::vector<std::string> ListSubdirs( const std::string &dir )
{
	std::vector<std::string> out;
	DIR *d = opendir( dir.c_str());
	if( !d ) return out;
	dirent *ent;
	while(( ent = readdir( d )))
	{
		const char *n = ent->d_name;
		if( n[0] == '.' && ( n[1] == 0 || ( n[1] == '.' && n[2] == 0 ))) continue; // skip . and ..
		if( n[0] == '.' ) continue; // skip hidden
		std::string full = dir + "/" + n;
		if( IsDir( full )) out.push_back( n );
	}
	closedir( d );
	std::sort( out.begin(), out.end(), []( const std::string &a, const std::string &b )
	{
		return strcasecmp( a.c_str(), b.c_str()) < 0;
	});
	return out;
}

std::string ParentOf( const std::string &dir )
{
	if( dir.empty() || dir == "/" ) return "/";
	size_t s = dir.find_last_of( '/' );
	if( s == std::string::npos ) return "/";
	if( s == 0 ) return "/";
	return dir.substr( 0, s );
}

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

void DrawDirectoryBrowser( int win_w, int win_h )
{
	if( !g_picker.browser_open ) return;

	const float fs = ImGui::GetFontSize();
	ImGui::SetNextWindowPos(  ImVec2( fs * 0.5f, fs * 0.5f ));
	ImGui::SetNextWindowSize( ImVec2( win_w - fs, win_h - fs ));
	ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar
		| ImGuiWindowFlags_NoResize
		| ImGuiWindowFlags_NoMove
		| ImGuiWindowFlags_NoCollapse
		| ImGuiWindowFlags_NoSavedSettings;
	ImGui::Begin( "##browser", nullptr, flags );

	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 0.6f, fs * 0.5f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.4f, fs * 0.6f ));

	ImGui::Text( "Текущий путь:" );
	ImGui::TextWrapped( "%s", g_picker.current_dir.c_str());
	ImGui::Spacing();

	const float row_h = fs * 3.f;

	// Action buttons span the full width of the browser window and stack
	// vertically — fits any phone orientation without horizontal overflow.
	if( ImGui::Button( "Выбрать эту папку", ImVec2( -1, row_h )))
	{
		g_picker.selected   = g_picker.current_dir;
		g_picker.valid_pick = ValidateResourceDir( g_picker.selected );
		g_picker.browser_open = false;
	}
	if( ImGui::Button( "Вверх", ImVec2( -1, row_h )))
		g_picker.current_dir = ParentOf( g_picker.current_dir );
	if( ImGui::Button( "Отмена", ImVec2( -1, row_h )))
		g_picker.browser_open = false;

	ImGui::Separator();

	// Scrollable list of subdirectories — every entry is a big touch target.
	ImGui::BeginChild( "##dir_list", ImVec2( 0, 0 ), false,
		ImGuiWindowFlags_AlwaysVerticalScrollbar );

	auto subs = ListSubdirs( g_picker.current_dir );
	for( const auto &name : subs )
	{
		if( ImGui::Button( name.c_str(), ImVec2( -1, row_h )))
		{
			std::string next = g_picker.current_dir;
			if( next != "/" ) next += "/";
			next += name;
			g_picker.current_dir = next;
		}
	}

	ImGui::EndChild();

	ImGui::PopStyleVar( 2 );
	ImGui::End();
}

void DrawTab_Game( bool &keep_running, bool &user_quit, int win_w, int win_h )
{
	const float fs    = ImGui::GetFontSize();
	const float btn_w = fs * 16.f;
	const float btn_h = fs * 3.5f;

	ImGui::Text( "Путь к ресурсам:" );
	ImGui::TextWrapped( "%s", g_picker.selected.empty()
		? "(не выбран)"
		: g_picker.selected.c_str());

	ImGui::Spacing();
	if( ImGui::Button( "Выбрать папку...", ImVec2( fs * 14.f, btn_h )))
	{
		g_picker.current_dir = g_picker.selected.empty()
			? DefaultStartDir()
			: g_picker.selected;
		g_picker.browser_open = true;
	}

	ImGui::Spacing();
	if( g_picker.selected.empty())
	{
		ImGui::TextColored( ImVec4( 0.9f, 0.7f, 0.2f, 1.f ),
			"Выберите папку с игрой Half-Life (содержит valve/...)" );
	}
	else if( g_picker.valid_pick )
	{
		ImGui::TextColored( ImVec4( 0.4f, 0.9f, 0.4f, 1.f ),
			"Ресурсы найдены." );
	}
	else
	{
		ImGui::TextColored( ImVec4( 0.95f, 0.4f, 0.4f, 1.f ),
			"В выбранной папке не найдено valve/liblist.gam." );
	}

	ImGui::Dummy( ImVec2( 0, fs * 1.f ));

	const bool can_continue = !g_picker.selected.empty() && g_picker.valid_pick;
	ImGui::BeginDisabled( !can_continue );
	ImGui::SetCursorPosX(( win_w - btn_w ) * 0.5f );
	if( ImGui::Button( "Начать игру", ImVec2( btn_w, btn_h * 1.2f )))
	{
		keep_running = false;
		user_quit    = false;
	}
	ImGui::EndDisabled();

	ImGui::SetCursorPosX(( win_w - btn_w ) * 0.5f );
	if( ImGui::Button( "Выход", ImVec2( btn_w, btn_h )))
	{
		keep_running = false;
		user_quit    = true;
	}
}

void DrawTab_Settings()
{
	const float fs = ImGui::GetFontSize();

	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));
	ImGui::TextWrapped( "Разрешение рендера (3D scale)" );
	ImGui::TextWrapped(
		"Множитель размера FBO относительно окна. 0.5 = половина разрешения "
		"(быстрее), 1.0 = полное, 2.0 = supersample. Меняется на следующем "
		"запуске игры." );
	ImGui::Dummy( ImVec2( 0, fs * 0.5f ));

	ImGui::PushItemWidth( -fs * 4.f );
	ImGui::SliderFloat( "##r_3d_scale", &g_settings.r_3d_scale, 0.25f, 2.0f, "%.2f" );
	ImGui::PopItemWidth();

	ImGui::Spacing();
	// Snap to sensible steps via preset buttons — big finger targets.
	const float btn_h = fs * 3.f;
	const float btn_w = (( ImGui::GetContentRegionAvail().x - fs * 2.f ) / 5.f );
	auto preset = [&]( const char *label, float value )
	{
		if( ImGui::Button( label, ImVec2( btn_w, btn_h )))
			g_settings.r_3d_scale = value;
	};
	preset( "0.25", 0.25f ); ImGui::SameLine();
	preset( "0.50", 0.50f ); ImGui::SameLine();
	preset( "0.75", 0.75f ); ImGui::SameLine();
	preset( "1.0",  1.00f ); ImGui::SameLine();
	preset( "2.0",  2.00f );

	ImGui::Dummy( ImVec2( 0, fs ));
	ImGui::TextColored( ImVec4( 0.6f, 0.8f, 1.f, 1.f ),
		"Текущее значение: %.2f", g_settings.r_3d_scale );
}

void DrawTab_About()
{
	// Bump font size for the disclaimer block so it reads well at arm's
	// length on a phone.
	ImGui::SetWindowFontScale( 1.5f );

	ImGui::TextWrapped(
		"Xash3D-FWGS — open-source реимплементация движка GoldSrc от Valve.\n\n"
		"Этот порт собран для AuroraOS / SailfishOS." );
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Лицензия движка: GPLv3.\n"
		"Исходный код: https://github.com/FWGS/xash3d-fwgs\n\n"
		"Half-Life (c) Valve Software. Ресурсы игры не распространяются с этим приложением — приобретайте Half-Life легально (например, в Steam) и укажите путь к установленной игре во вкладке Игра." );
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Программа предоставляется AS IS, без каких-либо гарантий." );
	ImGui::Spacing();
	ImGui::Separator();
	ImGui::Spacing();
	ImGui::TextWrapped(
		"Сторонние компоненты:\n"
		"  Dear ImGui (c) Omar Cornut, MIT license\n"
		"  imfilebrowser.h (c) AirGuanZ, MIT license (если используется)" );

	ImGui::SetWindowFontScale( 1.0f );
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

	const float fs = ImGui::GetFontSize();
	ImGui::PushStyleVar( ImGuiStyleVar_FramePadding, ImVec2( fs * 1.0f, fs * 0.6f ));
	ImGui::PushStyleVar( ImGuiStyleVar_ItemSpacing,  ImVec2( fs * 0.5f, fs * 0.6f ));

	ImVec2 hdr = ImGui::CalcTextSize( "Xash3D launcher (AuroraOS)" );
	ImGui::SetCursorPos( ImVec2(( win_w - hdr.x ) * 0.5f, fs * 1.2f ));
	ImGui::TextUnformatted( "Xash3D launcher (AuroraOS)" );

	if( ImGui::BeginTabBar( "##tabs" ))
	{
		if( ImGui::BeginTabItem( "Игра" ))
		{
			DrawTab_Game( keep_running, user_quit, win_w, win_h );
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Настройки" ))
		{
			DrawTab_Settings();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "О программе" ))
		{
			DrawTab_About();
			ImGui::EndTabItem();
		}
		if( ImGui::BeginTabItem( "Maliit" ))
		{
			DrawTab_Maliit();
			ImGui::EndTabItem();
		}
		ImGui::EndTabBar();
	}

	ImGui::PopStyleVar( 2 );
	ImGui::End();

	DrawDirectoryBrowser( win_w, win_h );
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

	// Load persisted settings (path + r_3d_scale). Prefill the picker if
	// the saved path still points at a valid resource root.
	LoadSettings();
	{
		std::string saved = LoadConfigPath();
		if( !saved.empty() && ValidateResourceDir( saved ))
		{
			g_picker.selected   = saved;
			g_picker.valid_pick = true;
		}
	}

	bool keep_running = true;
	bool user_quit    = false;

	MaliitDbg_Init();

	while( keep_running )
	{
		MaliitDbg_Pump();

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
				ProcessTouchEvent( ev, win_w, win_h );
			}
		}

		ImGui_ImplOpenGL3_NewFrame();
		ImGui_ImplSDL2_NewFrame();
		ApplyPendingScroll();
		ImGui::NewFrame();

		DrawLauncherUI( keep_running, user_quit, win_w, win_h );

		ImGui::Render();
		glViewport( 0, 0, win_w, win_h );
		glClearColor( 0.08f, 0.08f, 0.10f, 1.0f );
		glClear( GL_COLOR_BUFFER_BIT );
		ImGui_ImplOpenGL3_RenderDrawData( ImGui::GetDrawData() );

		SDL_GL_SwapWindow( g_window );
	}

	// Hand the user's pick over to the engine.
	//   XASH3D_BASEDIR — writable root (saves, configs, downloaded mods).
	//     On AuroraOS this MUST stay inside the sandboxed
	//     ~/.local/share/<org>/<app> tree.
	//   XASH3D_RODIR   — read-only root with the game assets the user, actually get from desktop file
	//     picked (e.g. .../Half-Life).
	// The launcher's own config also lives in the sandbox; otherwise
	// AuroraOS silently swallows the write and the path is lost on the
	// next launch.
	if( !user_quit && !g_picker.selected.empty() && g_picker.valid_pick )
	{
		setenv( "XASH3D_BASEDIR", g_picker.selected.c_str(), 1 );
		// setenv( "XASH3D_RODIR",   g_picker.selected.c_str(), 1 );
		// XASH3D_RODIR set as -rodir flag in desktop Exec
		(void)chdir( g_picker.selected.c_str());
		g_settings.path = g_picker.selected;
		SaveSettings();

		// Hand r_3d_scale to the engine via env var; picked up in
		// vid_common.c right after Cvar_RegisterVariable(&r_3d_scale).
		{
			char buf[32];
			snprintf( buf, sizeof( buf ), "%.3f", g_settings.r_3d_scale );
			setenv( "XASH3D_R_3D_SCALE", buf, 1 );
		}
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

	MaliitDbg_Shutdown();

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
