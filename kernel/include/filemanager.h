/*
 * ClaudeOS File Manager
 * GUI app for browsing and managing files
 */

#ifndef FILEMANAGER_H
#define FILEMANAGER_H

/* Initialize the file manager */
void filemanager_init(void);

/* Update file manager state - returns 1 if needs redraw */
int filemanager_update(void);

/* Draw the file manager */
void filemanager_draw(void);

/* Check if file manager wants to close */
int filemanager_should_close(void);

/* Clear the close flag */
void filemanager_clear_close(void);

#endif /* FILEMANAGER_H */
