#ifndef PROMPT_H
#define PROMPT_H

/* The prompt to show before reading a line: 1 = PS1, 2 = PS2.
 * Returns a malloc'd string. */
char *prompt_string(int which);

#endif
