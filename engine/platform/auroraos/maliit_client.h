/*
 * maliit_client.h -- portable C wrapper around Maliit input-method framework.
 *
 * Public API is plain C with function-pointer callbacks; the implementation
 * (maliit_client.c) uses GLib/GDBus internally. The wrapper is designed to be
 * dropped into any project — no engine-specific types leak into this header.
 *
 * Target: Aurora OS / Sailfish OS (peer-to-peer maliit DBus bus).
 *
 * Typical use:
 *   maliit_callbacks_t cb = {0};
 *   cb.commit_string   = on_commit;
 *   cb.key_event       = on_key;
 *   cb.visibility      = on_visibility;
 *   maliit_client_init(&cb);
 *   ...
 *   // in main loop:
 *   maliit_client_pump();
 *   ...
 *   maliit_client_show();
 *   ...
 *   maliit_client_shutdown();
 */

#ifndef MALIIT_CLIENT_H
#define MALIIT_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum
{
	MALIIT_CONTENT_FREE_TEXT = 0,
	MALIIT_CONTENT_NUMBER    = 1,
	MALIIT_CONTENT_PHONE     = 2,
	MALIIT_CONTENT_EMAIL     = 3,
	MALIIT_CONTENT_URL       = 4,
	MALIIT_CONTENT_CUSTOM    = 5
} maliit_content_type_t;

/* Key event flags mirror what the maliit server sends. */
typedef enum
{
	MALIIT_KEY_PRESS   = 6,   /* QEvent::KeyPress   */
	MALIIT_KEY_RELEASE = 7    /* QEvent::KeyRelease */
} maliit_key_event_type_t;

typedef struct maliit_callbacks_s
{
	void *user_data;

	/* Text committed by the IM. utf8 is NUL-terminated, valid only during call. */
	void ( *commit_string ) ( void *user_data, const char *utf8,
	                          int32_t replace_start, int32_t replace_length,
	                          int32_t cursor_pos );

	/* Preedit (composing) text update. */
	void ( *preedit_string ) ( void *user_data, const char *utf8,
	                           int32_t replace_start, int32_t replace_length,
	                           int32_t cursor_pos );

	/*
	 * Synthetic key event forwarded by the IM (e.g. Backspace, Enter,
	 * arrow keys). qt_key uses Qt::Key values; qt_modifiers uses
	 * Qt::KeyboardModifier flags. text is the produced UTF-8 (may be empty).
	 */
	void ( *key_event ) ( void *user_data, int type, int qt_key,
	                      int qt_modifiers, const char *text,
	                      bool auto_repeat, int32_t count );

	/* Keyboard shown/hidden. Fires on our own show/hide and on IM-initiated hide. */
	void ( *visibility ) ( void *user_data, bool visible );

	/* Reported occupied screen area (screen coords, pixels). */
	void ( *area_changed ) ( void *user_data, int32_t x, int32_t y,
	                         int32_t w, int32_t h );
} maliit_callbacks_t;

/*
 * Lifecycle. Callbacks struct is copied internally; caller may free it.
 * Returns false if maliit server is unreachable — in that case the client is
 * left in an inert state and all further calls are no-ops.
 */
bool maliit_client_init( const maliit_callbacks_t *cb );
void maliit_client_shutdown( void );

/*
 * Drain internal GLib event queue. MUST be called regularly from the same
 * thread that called maliit_client_init (typically the main loop, once per
 * frame). Non-blocking.
 */
void maliit_client_pump( void );

/* True if the wrapper is initialised and connected to the maliit server. */
bool maliit_client_is_ready( void );

/* Show / hide the on-screen keyboard. */
void maliit_client_show( void );
void maliit_client_hide( void );
bool maliit_client_is_visible( void );

/*
 * Focus + widget state. Call maliit_client_focus_in() when your text field
 * gains focus (this batches contentType, predictions, etc. into a single
 * updateWidgetInformation call). Call maliit_client_focus_out() on blur.
 * The server will not deliver text unless a focus-in has occurred.
 */
void maliit_client_focus_in( void );
void maliit_client_focus_out( void );

/* Configuration knobs. Take effect on next focus_in / show. */
void maliit_client_set_content_type( maliit_content_type_t type );
void maliit_client_set_prediction( bool enabled );
void maliit_client_set_correction( bool enabled );
void maliit_client_set_autocaps( bool enabled );
void maliit_client_set_hidden_text( bool hidden );

/*
 * Convenience: shortcut for "this is a password field". Equivalent to
 * setting hidden_text=true, prediction=false, correction=false,
 * autocaps=false, content_type=FREE_TEXT.
 */
void maliit_client_set_password_mode( bool password );

/* Cursor rectangle in screen coords (used by IM for popup placement). */
void maliit_client_set_cursor_rect( int32_t x, int32_t y, int32_t w, int32_t h );

/* Reset the IM (clears composing text). */
void maliit_client_reset( void );

/*
 * Surrounding text hint (helps prediction/correction). Optional.
 * text is UTF-8; cursor_pos is a byte offset into text.
 */
void maliit_client_set_surrounding_text( const char *text, int32_t cursor_pos );

#ifdef __cplusplus
}
#endif

#endif /* MALIIT_CLIENT_H */
