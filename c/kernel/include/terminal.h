/*
 * ClaudeOS Terminal
 */

#ifndef TERMINAL_H
#define TERMINAL_H

/* Initialize the terminal */
void terminal_init(void);

/* Process pending input events and update terminal state.
 * Call this in the main loop.
 * Returns 1 if display needs refresh, 0 otherwise. */
int terminal_update(void);

/* Draw the terminal to the framebuffer */
void terminal_draw(void);

/* Increment uptime counter (call from main loop) */
void terminal_tick(void);

/* Check if terminal wants to close (return to home) */
int terminal_should_close(void);

/* Clear the close flag */
void terminal_clear_close(void);

#endif /* TERMINAL_H */
