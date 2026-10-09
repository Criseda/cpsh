#ifndef LINEEDIT_H
#define LINEEDIT_H

/* Read one line of interactive input after showing `prompt`. Returns a
 * malloc'd line ending in '\n', or NULL at end of input (errno == 0) or on
 * interrupt (errno == EINTR). Uses an editor with history and tab
 * completion when stdin is a terminal. */
char *lineedit_read(const char *prompt);

#endif
