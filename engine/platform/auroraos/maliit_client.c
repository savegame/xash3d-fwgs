/*
 * maliit_client.c -- GDBus implementation of the portable maliit wrapper.
 *
 * Discovery / connection follows what libmaliit-glib does:
 *   1. If $MALIIT_SERVER_ADDRESS is set, use it verbatim.
 *   2. Otherwise ask the session bus for property
 *      org.maliit.Server.Address.address on org.maliit.server at
 *      /org/maliit/server/address .
 *   3. Open a peer-to-peer GDBusConnection to that address with
 *      G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT.
 *
 * Two DBus interfaces are involved:
 *   - com.meego.inputmethod.uiserver1 at /com/meego/inputmethod/uiserver1
 *     Method calls we make into the maliit server.
 *   - com.meego.inputmethod.inputcontext1 at /com/meego/inputmethod/inputcontext
 *     Object WE expose so the maliit server can push commit/preedit/key
 *     events back to us.
 *
 * Build: pkg-config --cflags --libs glib-2.0 gio-2.0 gio-unix-2.0
 */

#include "maliit_client.h"

#include <gio/gio.h>
#include <glib.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#define MALIIT_LOG_TAG "maliit_client"
#define maliit_log( ... ) g_message( MALIIT_LOG_TAG ": " __VA_ARGS__ )
#define maliit_warn( ... ) g_warning( MALIIT_LOG_TAG ": " __VA_ARGS__ )

/* Well-known names / paths. */
#define ADDR_SERVICE   "org.maliit.server"
#define ADDR_PATH      "/org/maliit/server/address"
#define ADDR_IFACE     "org.maliit.Server.Address"
#define ADDR_PROPERTY  "address"

#define SERVER_PATH    "/com/meego/inputmethod/uiserver1"
#define SERVER_IFACE   "com.meego.inputmethod.uiserver1"

#define CTX_PATH       "/com/meego/inputmethod/inputcontext"
#define CTX_IFACE      "com.meego.inputmethod.inputcontext1"

/* -------------------------------------------------------------------------
 * Input-context introspection XML.
 *
 * These signatures track the maliit-framework input-context DBus protocol
 * used by Sailfish/Aurora maliit-servers. If a specific server rejects a
 * signature at runtime, tune the offending method here — introspection
 * happens once at startup, so the price of over-declaring methods is zero.
 * ------------------------------------------------------------------------- */
static const char CTX_INTROSPECTION_XML[] =
	"<node>"
	"  <interface name='" CTX_IFACE "'>"
	"    <method name='activationLostEvent'/>"
	"    <method name='imInitiatedHide'/>"
	"    <method name='commitString'>"
	"      <arg name='string'          type='s' direction='in'/>"
	"      <arg name='replaceStart'    type='i' direction='in'/>"
	"      <arg name='replaceLength'   type='i' direction='in'/>"
	"      <arg name='cursorPos'       type='i' direction='in'/>"
	"    </method>"
	"    <method name='updatePreedit'>"
	"      <arg name='string'          type='s'     direction='in'/>"
	"      <arg name='formatList'      type='a(iii)' direction='in'/>"
	"      <arg name='replaceStart'    type='i'     direction='in'/>"
	"      <arg name='replaceLength'   type='i'     direction='in'/>"
	"      <arg name='cursorPos'       type='i'     direction='in'/>"
	"    </method>"
	"    <method name='keyEvent'>"
	"      <arg name='type'            type='i' direction='in'/>"
	"      <arg name='key'             type='i' direction='in'/>"
	"      <arg name='modifiers'       type='i' direction='in'/>"
	"      <arg name='text'            type='s' direction='in'/>"
	"      <arg name='autoRepeat'      type='b' direction='in'/>"
	"      <arg name='count'           type='i' direction='in'/>"
	"      <arg name='requestType'     type='i' direction='in'/>"
	"    </method>"
	"    <method name='updateInputMethodArea'>"
	"      <arg name='x' type='i' direction='in'/>"
	"      <arg name='y' type='i' direction='in'/>"
	"      <arg name='w' type='i' direction='in'/>"
	"      <arg name='h' type='i' direction='in'/>"
	"    </method>"
	"    <method name='setGlobalCorrectionEnabled'>"
	"      <arg name='enabled' type='b' direction='in'/>"
	"    </method>"
	"    <method name='setRedirectKeys'>"
	"      <arg name='enabled' type='b' direction='in'/>"
	"    </method>"
	"    <method name='setDetectableAutoRepeat'>"
	"      <arg name='enabled' type='b' direction='in'/>"
	"    </method>"
	"    <method name='setLanguage'>"
	"      <arg name='language' type='s' direction='in'/>"
	"    </method>"
	"    <method name='setSelection'>"
	"      <arg name='start'  type='i' direction='in'/>"
	"      <arg name='length' type='i' direction='in'/>"
	"    </method>"
	"    <method name='notifyExtendedAttributeChanged'>"
	"      <arg name='id'         type='i' direction='in'/>"
	"      <arg name='target'     type='s' direction='in'/>"
	"      <arg name='targetItem' type='s' direction='in'/>"
	"      <arg name='attribute'  type='s' direction='in'/>"
	"      <arg name='value'      type='v' direction='in'/>"
	"    </method>"
	"    <method name='copy'/>"
	"    <method name='paste'/>"
	"    <method name='preeditRectangle'>"
	"      <arg name='x'     type='i' direction='out'/>"
	"      <arg name='y'     type='i' direction='out'/>"
	"      <arg name='w'     type='i' direction='out'/>"
	"      <arg name='h'     type='i' direction='out'/>"
	"      <arg name='valid' type='b' direction='out'/>"
	"    </method>"
	"  </interface>"
	"</node>";

/* -------------------------------------------------------------------------
 * State.
 * ------------------------------------------------------------------------- */
typedef struct
{
	maliit_callbacks_t cb;

	GDBusConnection *conn;
	guint            ctx_reg_id;
	GDBusNodeInfo   *ctx_node;

	/* Latched widget state — flushed on focus_in via updateWidgetInformation. */
	maliit_content_type_t content_type;
	gboolean prediction;
	gboolean correction;
	gboolean autocaps;
	gboolean hidden_text;

	gboolean visible;
	gboolean focused;
	gboolean ready;

	gint32   cursor_x, cursor_y, cursor_w, cursor_h;
} maliit_state_t;

static maliit_state_t g;

/* -------------------------------------------------------------------------
 * Address discovery.
 * ------------------------------------------------------------------------- */
static gchar *maliit_query_address( void )
{
	const gchar *env = g_getenv( "MALIIT_SERVER_ADDRESS" );
	if( env && *env )
		return g_strdup( env );

	GError *err = NULL;
	GDBusConnection *session = g_bus_get_sync( G_BUS_TYPE_SESSION, NULL, &err );
	if( !session )
	{
		maliit_warn( "session bus unreachable: %s", err ? err->message : "?" );
		g_clear_error( &err );
		return NULL;
	}

	GVariant *reply = g_dbus_connection_call_sync(
		session,
		ADDR_SERVICE, ADDR_PATH,
		"org.freedesktop.DBus.Properties", "Get",
		g_variant_new( "(ss)", ADDR_IFACE, ADDR_PROPERTY ),
		G_VARIANT_TYPE( "(v)" ),
		G_DBUS_CALL_FLAGS_NONE, 3000, NULL, &err );

	g_object_unref( session );

	if( !reply )
	{
		maliit_warn( "cannot get maliit address: %s", err ? err->message : "?" );
		g_clear_error( &err );
		return NULL;
	}

	GVariant *inner = NULL;
	g_variant_get( reply, "(v)", &inner );
	gchar *addr = g_variant_dup_string( inner, NULL );
	g_variant_unref( inner );
	g_variant_unref( reply );
	return addr;
}

/* -------------------------------------------------------------------------
 * Incoming method dispatch (server -> us).
 * ------------------------------------------------------------------------- */
static void ctx_method_call( GDBusConnection      *connection,
                             const gchar          *sender,
                             const gchar          *object_path,
                             const gchar          *interface_name,
                             const gchar          *method_name,
                             GVariant             *parameters,
                             GDBusMethodInvocation *invocation,
                             gpointer              user_data )
{
	(void)connection; (void)sender; (void)object_path;
	(void)interface_name; (void)user_data;

	if( g_str_equal( method_name, "commitString" ) )
	{
		const gchar *s = NULL;
		gint32 rs = 0, rl = 0, cp = -1;
		g_variant_get( parameters, "(&siii)", &s, &rs, &rl, &cp );
		if( g.cb.commit_string )
			g.cb.commit_string( g.cb.user_data, s ? s : "", rs, rl, cp );
		g_dbus_method_invocation_return_value( invocation, NULL );
		return;
	}

	if( g_str_equal( method_name, "updatePreedit" ) )
	{
		const gchar *s = NULL;
		GVariant *formats = NULL;
		gint32 rs = 0, rl = 0, cp = -1;
		g_variant_get( parameters, "(&s@a(iii)iii)",
			&s, &formats, &rs, &rl, &cp );
		if( formats ) g_variant_unref( formats );
		if( g.cb.preedit_string )
			g.cb.preedit_string( g.cb.user_data, s ? s : "", rs, rl, cp );
		g_dbus_method_invocation_return_value( invocation, NULL );
		return;
	}

	if( g_str_equal( method_name, "keyEvent" ) )
	{
		gint32 type, key, mods, count, req;
		const gchar *text = NULL;
		gboolean autorep;
		g_variant_get( parameters, "(iii&sbii)",
			&type, &key, &mods, &text, &autorep, &count, &req );
		if( g.cb.key_event )
			g.cb.key_event( g.cb.user_data, type, key, mods,
			                text ? text : "", autorep, count );
		g_dbus_method_invocation_return_value( invocation, NULL );
		return;
	}

	if( g_str_equal( method_name, "updateInputMethodArea" ) )
	{
		gint32 x, y, w, h;
		g_variant_get( parameters, "(iiii)", &x, &y, &w, &h );
		if( g.cb.area_changed )
			g.cb.area_changed( g.cb.user_data, x, y, w, h );
		g_dbus_method_invocation_return_value( invocation, NULL );
		return;
	}

	if( g_str_equal( method_name, "imInitiatedHide" )
	 || g_str_equal( method_name, "activationLostEvent" ) )
	{
		g.visible = FALSE;
		if( g.cb.visibility )
			g.cb.visibility( g.cb.user_data, FALSE );
		g_dbus_method_invocation_return_value( invocation, NULL );
		return;
	}

	if( g_str_equal( method_name, "preeditRectangle" ) )
	{
		/* We don't track a preedit rect; report invalid. */
		g_dbus_method_invocation_return_value( invocation,
			g_variant_new( "(iiiib)", 0, 0, 0, 0, FALSE ) );
		return;
	}

	/* Silently ack everything else the server may push at us. */
	g_dbus_method_invocation_return_value( invocation, NULL );
}

static const GDBusInterfaceVTable ctx_vtable = {
	ctx_method_call,
	NULL, /* get_property */
	NULL, /* set_property */
	{ NULL }
};

/* -------------------------------------------------------------------------
 * Server calls.
 * ------------------------------------------------------------------------- */
static void server_call( const gchar *method, GVariant *args )
{
	if( !g.ready || !g.conn )
	{
		if( args ) g_variant_unref( g_variant_ref_sink( args ) );
		return;
	}

	g_dbus_connection_call(
		g.conn,
		NULL, /* peer-to-peer: no destination */
		SERVER_PATH, SERVER_IFACE, method,
		args,
		NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL, NULL );
}

/* Build a{sv} widget state dict from our latched knobs. */
static GVariant *build_widget_state( void )
{
	GVariantBuilder b;
	g_variant_builder_init( &b, G_VARIANT_TYPE( "a{sv}" ) );

	#define ADD_B( k, v ) g_variant_builder_add( &b, "{sv}", (k), \
		g_variant_new_boolean( (v) ) )
	#define ADD_I( k, v ) g_variant_builder_add( &b, "{sv}", (k), \
		g_variant_new_int32( (v) ) )

	ADD_B( "focusState",               g.focused );
	ADD_I( "contentType",              (gint32)g.content_type );
	ADD_B( "predictionEnabled",        g.prediction );
	ADD_B( "correctionEnabled",        g.correction );
	ADD_B( "autocapitalizationEnabled", g.autocaps );
	ADD_B( "hiddenText",               g.hidden_text );
	ADD_B( "visualizationPriority",    FALSE );
	ADD_I( "inputMethodMode",          0 /* Normal */ );
	ADD_I( "cursorPosition",           0 );
	ADD_B( "hasSelection",             FALSE );

	g_variant_builder_add( &b, "{sv}", "cursorRectangle",
		g_variant_new( "(iiii)",
			g.cursor_x, g.cursor_y, g.cursor_w, g.cursor_h ) );

	#undef ADD_B
	#undef ADD_I

	return g_variant_builder_end( &b );
}

/* -------------------------------------------------------------------------
 * Public API.
 * ------------------------------------------------------------------------- */
bool maliit_client_init( const maliit_callbacks_t *cb )
{
	if( g.ready )
		return true;

	memset( &g, 0, sizeof( g ) );
	if( cb )
		g.cb = *cb;

	g.content_type = MALIIT_CONTENT_FREE_TEXT;
	g.prediction   = TRUE;
	g.correction   = TRUE;
	g.autocaps     = TRUE;
	g.hidden_text  = FALSE;

	gchar *addr = maliit_query_address();
	if( !addr )
		return false;

	maliit_log( "connecting to maliit at %s", addr );

	GError *err = NULL;
	g.conn = g_dbus_connection_new_for_address_sync(
		addr,
		G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT,
		NULL, NULL, &err );
	g_free( addr );

	if( !g.conn )
	{
		maliit_warn( "P2P connect failed: %s", err ? err->message : "?" );
		g_clear_error( &err );
		return false;
	}

	g.ctx_node = g_dbus_node_info_new_for_xml( CTX_INTROSPECTION_XML, &err );
	if( !g.ctx_node )
	{
		maliit_warn( "bad introspection XML: %s", err ? err->message : "?" );
		g_clear_error( &err );
		g_object_unref( g.conn );
		g.conn = NULL;
		return false;
	}

	g.ctx_reg_id = g_dbus_connection_register_object(
		g.conn, CTX_PATH, g.ctx_node->interfaces[0],
		&ctx_vtable, NULL, NULL, &err );

	if( g.ctx_reg_id == 0 )
	{
		maliit_warn( "register input context failed: %s",
			err ? err->message : "?" );
		g_clear_error( &err );
		g_dbus_node_info_unref( g.ctx_node );
		g.ctx_node = NULL;
		g_object_unref( g.conn );
		g.conn = NULL;
		return false;
	}

	g.ready = TRUE;
	maliit_log( "input context registered at %s", CTX_PATH );
	return true;
}

void maliit_client_shutdown( void )
{
	if( !g.ready )
		return;

	if( g.visible )
		maliit_client_hide();

	if( g.conn && g.ctx_reg_id )
		g_dbus_connection_unregister_object( g.conn, g.ctx_reg_id );
	if( g.ctx_node )
		g_dbus_node_info_unref( g.ctx_node );
	if( g.conn )
	{
		g_dbus_connection_close_sync( g.conn, NULL, NULL );
		g_object_unref( g.conn );
	}
	memset( &g, 0, sizeof( g ) );
}

void maliit_client_pump( void )
{
	GMainContext *ctx = g_main_context_default();
	while( g_main_context_pending( ctx ) )
		g_main_context_iteration( ctx, FALSE );
}

bool maliit_client_is_ready( void )   { return g.ready ? true : false; }
bool maliit_client_is_visible( void ) { return g.visible ? true : false; }

void maliit_client_focus_in( void )
{
	if( !g.ready ) return;
	g.focused = TRUE;
	server_call( "activateContext", NULL );
	server_call( "updateWidgetInformation",
		g_variant_new( "(@a{sv}b)", build_widget_state(), TRUE ) );
}

void maliit_client_focus_out( void )
{
	if( !g.ready ) return;
	g.focused = FALSE;
	server_call( "updateWidgetInformation",
		g_variant_new( "(@a{sv}b)", build_widget_state(), TRUE ) );
}

void maliit_client_show( void )
{
	if( !g.ready ) return;
	if( !g.focused ) maliit_client_focus_in();
	server_call( "showInputMethod", NULL );
	g.visible = TRUE;
	if( g.cb.visibility ) g.cb.visibility( g.cb.user_data, true );
}

void maliit_client_hide( void )
{
	if( !g.ready ) return;
	server_call( "hideInputMethod", NULL );
	g.visible = FALSE;
	if( g.cb.visibility ) g.cb.visibility( g.cb.user_data, false );
}

void maliit_client_reset( void )
{
	server_call( "reset", NULL );
}

void maliit_client_set_content_type( maliit_content_type_t t ) { g.content_type = t; }
void maliit_client_set_prediction( bool v )                    { g.prediction  = v ? TRUE : FALSE; }
void maliit_client_set_correction( bool v )                    { g.correction  = v ? TRUE : FALSE; }
void maliit_client_set_autocaps( bool v )                      { g.autocaps    = v ? TRUE : FALSE; }
void maliit_client_set_hidden_text( bool v )                   { g.hidden_text = v ? TRUE : FALSE; }

void maliit_client_set_password_mode( bool password )
{
	g.hidden_text  = password ? TRUE : FALSE;
	g.prediction   = password ? FALSE : TRUE;
	g.correction   = password ? FALSE : TRUE;
	g.autocaps     = password ? FALSE : TRUE;
	g.content_type = MALIIT_CONTENT_FREE_TEXT;
}

void maliit_client_set_cursor_rect( int32_t x, int32_t y, int32_t w, int32_t h )
{
	g.cursor_x = x; g.cursor_y = y;
	g.cursor_w = w; g.cursor_h = h;
}

void maliit_client_set_surrounding_text( const char *text, int32_t cursor_pos )
{
	if( !g.ready ) return;
	GVariantBuilder b;
	g_variant_builder_init( &b, G_VARIANT_TYPE( "a{sv}" ) );
	g_variant_builder_add( &b, "{sv}", "surroundingText",
		g_variant_new_string( text ? text : "" ) );
	g_variant_builder_add( &b, "{sv}", "cursorPosition",
		g_variant_new_int32( cursor_pos ) );
	server_call( "updateWidgetInformation",
		g_variant_new( "(@a{sv}b)", g_variant_builder_end( &b ), FALSE ) );
}
