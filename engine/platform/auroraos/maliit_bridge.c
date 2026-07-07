/*
 * maliit_bridge.c -- maliit_client → SDL2 event bridge.
 *
 * commit_string  → SDL_TEXTINPUT (chopped into 31-byte chunks, since
 *                  SDL_TextInputEvent.text is a fixed 32-byte buffer).
 * key_event      → SDL_KEYDOWN / SDL_KEYUP for editing keys (Backspace,
 *                  Enter, arrows, Home/End, Delete, Tab, Escape). All
 *                  printable characters come as commit_string; we do NOT
 *                  fabricate synthetic key events for them.
 *
 * We keep the bridge live for the whole engine lifetime — repeatedly
 * activating/deactivating the connection tends to make maliit-server
 * blink the OSK. focus_in/focus_out is enough.
 */

#include "maliit_bridge.h"
#include "maliit_client.h"

#include <SDL.h>
#include <string.h>
#include <stdio.h>

static struct
{
	bool inited;
	bool text_active;
	bool password_pending;
} s;

/* ---- Qt::Key → SDL_Scancode mapping for editing keys ---- */
static int qt_key_to_scancode( int qt_key )
{
	switch( qt_key )
	{
	case 0x01000000: return SDL_SCANCODE_ESCAPE;
	case 0x01000001: return SDL_SCANCODE_TAB;
	case 0x01000002: return SDL_SCANCODE_TAB;      /* Backtab */
	case 0x01000003: return SDL_SCANCODE_BACKSPACE;
	case 0x01000004: return SDL_SCANCODE_RETURN;
	case 0x01000005: return SDL_SCANCODE_KP_ENTER;
	case 0x01000007: return SDL_SCANCODE_DELETE;
	case 0x01000010: return SDL_SCANCODE_HOME;
	case 0x01000011: return SDL_SCANCODE_END;
	case 0x01000012: return SDL_SCANCODE_LEFT;
	case 0x01000013: return SDL_SCANCODE_UP;
	case 0x01000014: return SDL_SCANCODE_RIGHT;
	case 0x01000015: return SDL_SCANCODE_DOWN;
	case 0x01000016: return SDL_SCANCODE_PAGEUP;
	case 0x01000017: return SDL_SCANCODE_PAGEDOWN;
	}
	return 0;
}

/* ---- callbacks ---- */
static void on_commit( void *ud, const char *utf8, int rs, int rl, int cp )
{
	(void)ud; (void)rs; (void)rl; (void)cp;
	if( !utf8 || !*utf8 ) return;

	const size_t chunk_max = sizeof(((SDL_TextInputEvent *)0)->text) - 1;
	size_t total = strlen( utf8 );
	size_t off = 0;

	while( off < total )
	{
		size_t n = total - off;
		if( n > chunk_max ) n = chunk_max;
		/* Don't cut a multibyte UTF-8 sequence: back off to a lead byte. */
		if( off + n < total )
		{
			while( n > 0 && ( utf8[off + n] & 0xC0 ) == 0x80 )
				n--;
			if( n == 0 ) break; /* pathological */
		}

		SDL_Event ev;
		SDL_zero( ev );
		ev.type = SDL_TEXTINPUT;
		memcpy( ev.text.text, utf8 + off, n );
		ev.text.text[n] = '\0';
		SDL_PushEvent( &ev );

		off += n;
	}
}

static void on_key( void *ud, int type, int qt_key, int qt_mods,
                    const char *text, bool autorep, int count )
{
	(void)ud; (void)qt_mods; (void)text; (void)autorep; (void)count;

	int sc = qt_key_to_scancode( qt_key );
	if( sc == 0 ) return; /* printable — arrives via commit_string */

	SDL_Event ev;
	SDL_zero( ev );
	ev.key.type      = ( type == MALIIT_KEY_PRESS ) ? SDL_KEYDOWN : SDL_KEYUP;
	ev.key.state     = ( type == MALIIT_KEY_PRESS ) ? SDL_PRESSED : SDL_RELEASED;
	ev.key.repeat    = autorep ? 1 : 0;
	ev.key.keysym.scancode = (SDL_Scancode)sc;
	ev.key.keysym.sym      = SDL_GetKeyFromScancode( (SDL_Scancode)sc );
	SDL_PushEvent( &ev );
}

static void on_visibility( void *ud, bool visible )
{
	(void)ud; (void)visible;
	/* Purely informational for now. Could drive VGui layout later. */
}

static void on_area( void *ud, int x, int y, int w, int h )
{
	(void)ud; (void)x; (void)y; (void)w; (void)h;
	/* TODO: expose so HUD/menu can dodge the OSK. */
}

/* ---- public API ---- */
void maliit_bridge_init( void )
{
	if( s.inited ) return;

	maliit_callbacks_t cb;
	memset( &cb, 0, sizeof( cb ));
	cb.commit_string = on_commit;
	cb.key_event     = on_key;
	cb.visibility    = on_visibility;
	cb.area_changed  = on_area;

	if( !maliit_client_init( &cb ))
	{
		fprintf( stderr, "maliit_bridge: init failed — OSK will not appear\n" );
		return;
	}
	s.inited = true;
}

void maliit_bridge_shutdown( void )
{
	if( !s.inited ) return;
	if( s.text_active ) maliit_client_hide();
	maliit_client_shutdown();
	s.inited = false;
	s.text_active = false;
}

void maliit_bridge_pump( void )
{
	if( s.inited ) maliit_client_pump();
}

void maliit_bridge_enable_text_input( bool enable )
{
	if( !s.inited ) maliit_bridge_init();
	if( !s.inited ) return;

	if( enable && !s.text_active )
	{
		maliit_client_set_password_mode( s.password_pending );
		maliit_client_focus_in();
		maliit_client_show();
		s.text_active = true;
	}
	else if( !enable && s.text_active )
	{
		maliit_client_hide();
		maliit_client_focus_out();
		s.text_active = false;
		/* Password hint is single-shot — reset after use. */
		s.password_pending = false;
	}
}

void maliit_bridge_set_password_mode( bool password )
{
	s.password_pending = password;
	if( s.inited && s.text_active )
	{
		maliit_client_set_password_mode( password );
		maliit_client_focus_in();
	}
}
