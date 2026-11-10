/* ps5-memcard on the PS5 home screen: the Media tab shortcut and the toasts. */
#ifndef HOMESCREEN_H
#define HOMESCREEN_H

#define HOMESCREEN_TITLE_ID "MEMC00001"

/* Put the icon where the system UI can read it and make sure the Media tab has
 * the shortcut that opens the page in the console's browser. Safe to call on
 * every start: nothing is reinstalled when it is already there and current. */
void homescreen_setup(void);

/* A notification with the ps5-memcard icon: title on the first line, text
 * below. Falls back to a plain text notification when the toast is refused. */
void homescreen_toast(const char *title, const char *fmt, ...);

#endif
