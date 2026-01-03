/*
 * ClaudeOS Home Screen
 * Shows logo and app icons
 */

#ifndef HOME_H
#define HOME_H

/* Initialize home screen */
void home_init(void);

/* Update home screen (handle touch) - returns 1 if needs redraw */
int home_update(void);

/* Draw home screen */
void home_draw(void);

/* Check if terminal icon was pressed */
int home_terminal_pressed(void);

/* Check if file manager icon was pressed */
int home_files_pressed(void);

/* Reset pressed states */
void home_clear_pressed(void);

/* Set external IP address (from ifconfig.me) */
void home_set_external_ip(const char* ip);

#endif /* HOME_H */
