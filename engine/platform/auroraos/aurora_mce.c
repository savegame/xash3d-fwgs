/*
 * aurora_mce.c -- GDBus implementation of the MCE blanking-pause wrapper.
 *
 * MCE ("Mode Control Entity") is a Nokia-era system daemon inherited by
 * MeeGo → Sailfish → Aurora. It sits on the system bus at
 * com.nokia.mce, exposing /com/nokia/mce/request with a
 * com.nokia.mce.request interface. The relevant method is:
 *
 *   req_display_blanking_pause() -> ()
 *
 * A single call keeps the display awake for roughly 60 seconds. We
 * therefore refresh every REFRESH_INTERVAL_S seconds while the
 * caller keeps blanking prevention enabled — the interval is picked
 * well below the timeout to leave slack for scheduling jitter.
 *
 * Build: pkg-config --cflags --libs glib-2.0 gio-2.0
 */

#include "aurora_mce.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>

#define AURORA_MCE_LOG_TAG "aurora_mce"
#define aurora_mce_log( ... ) g_message( AURORA_MCE_LOG_TAG ": " __VA_ARGS__ )
#define aurora_mce_warn( ... ) g_warning( AURORA_MCE_LOG_TAG ": " __VA_ARGS__ )

#define MCE_SERVICE   "com.nokia.mce"
#define MCE_PATH      "/com/nokia/mce/request"
#define MCE_INTERFACE "com.nokia.mce.request"
#define MCE_REQUEST   "req_display_blanking_pause"

/*
 * MCE grants ~60 seconds per pause request. Refresh at 45 to keep a
 * comfortable margin against scheduler jitter and process stalls.
 */
#define REFRESH_INTERVAL_S 45

static struct
{
	GDBusConnection *conn;
	guint            timer_id;
	bool             inited;
	bool             preventing;
} s;

/* Fire one blanking-pause request. Async, no reply expected. */
static void mce_fire_request( void )
{
	if( !s.conn ) return;

	g_dbus_connection_call(
		s.conn,
		MCE_SERVICE, MCE_PATH, MCE_INTERFACE, MCE_REQUEST,
		NULL, /* args */
		NULL, /* reply type */
		G_DBUS_CALL_FLAGS_NO_AUTO_START,
		2000, /* ms timeout */
		NULL, NULL, NULL );
}

static gboolean mce_timer_cb( gpointer user_data )
{
	(void)user_data;
	if( !s.preventing )
		return G_SOURCE_REMOVE;
	mce_fire_request();
	return G_SOURCE_CONTINUE;
}

/* -------- public API -------- */

bool aurora_mce_init( void )
{
	if( s.inited )
		return true;

	memset( &s, 0, sizeof( s ));

	GError *err = NULL;
	s.conn = g_bus_get_sync( G_BUS_TYPE_SYSTEM, NULL, &err );
	if( !s.conn )
	{
		aurora_mce_warn( "system bus unreachable: %s",
			err ? err->message : "?" );
		g_clear_error( &err );
		return false;
	}

	s.inited = true;
	aurora_mce_log( "connected to system bus, MCE ready" );
	return true;
}

void aurora_mce_shutdown( void )
{
	if( !s.inited ) return;

	if( s.preventing )
		aurora_mce_set_prevent_blanking( false );

	if( s.conn )
	{
		g_object_unref( s.conn );
		s.conn = NULL;
	}
	s.inited = false;
}

void aurora_mce_pump( void )
{
	GMainContext *ctx = g_main_context_default();
	while( g_main_context_pending( ctx ))
		g_main_context_iteration( ctx, FALSE );
}

void aurora_mce_set_prevent_blanking( bool prevent )
{
	if( !s.inited ) return;
	if( s.preventing == prevent ) return;

	s.preventing = prevent;

	if( prevent )
	{
		/* Fire once immediately so the effect starts right away. */
		mce_fire_request();

		if( s.timer_id == 0 )
			s.timer_id = g_timeout_add_seconds(
				REFRESH_INTERVAL_S, mce_timer_cb, NULL );

		aurora_mce_log( "blanking prevention: ON" );
	}
	else
	{
		if( s.timer_id != 0 )
		{
			g_source_remove( s.timer_id );
			s.timer_id = 0;
		}
		aurora_mce_log( "blanking prevention: OFF" );
	}
}

bool aurora_mce_is_preventing_blanking( void )
{
	return s.preventing;
}
