#ifndef INPUT_H
#define INPUT_H

#define MAX_COMMAND_LENGTH 1024

char *cpsh_read_line();
char **cpsh_tokenise(char *line);

#endif