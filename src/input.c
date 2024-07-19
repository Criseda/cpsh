#include "../include/shell.h"

#define TOKEN_SIZE 64

char *cpsh_read_line() {
  int buffer_size = BUFSIZ;
  int position = 0;
  char *buffer =
      malloc(sizeof(char) * buffer_size);  // Allocate memory for buffer
  int c;                                   // Character read from input

  if (!buffer) {
    fprintf(stderr, "cpsh: allocation error\n");  // Print an error message
    exit(EXIT_FAILURE);                           // Exit the program
  }

  while (1)  // while true
  {
    c = getchar();              // Read a character from input
    if (c == EOF || c == '\n')  // If the character is EOF or newline
    {
      buffer[position] = '\0';  // Set the end of the string
      return buffer;            // Return the buffer
    } else {
      buffer[position] = c;  // Add the character to the buffer
    }
    position++;  // Increment the position

    if (position >= buffer_size) {
      int new_size = buffer_size + 1024;
      char *new_buffer = realloc(buffer, new_size);
      if (!new_buffer) {
        fprintf(stderr, "cpsh: allocation error\n");
        free(buffer);  // Free the original buffer
        exit(EXIT_FAILURE);
      }
      buffer = new_buffer;
      buffer_size = new_size;
    }
  }
}

char **cpsh_tokenise(char *line) {
  int buffer_size = TOKEN_SIZE;
  int position = 0;
  char **tokens =
      malloc(buffer_size * sizeof(char *));  // Allocate memory for tokens
  char *token;
  char delimiters[] = " \t\r\n\a";  // Delimiters for tokenising the line

  if (!tokens) {
    fprintf(stderr, "cpsh: allocation error\n");  // Print an error message
    exit(EXIT_FAILURE);                           // Exit the program
  }
  // tokenize the line
  token = strtok(line, delimiters);  // Get the first token
  while (token != NULL)              // While there are tokens
  {
    tokens[position] = token;  // Add the token to the tokens array
    position++;                // Increment the position

    if (position >= buffer_size) {
      int new_size = buffer_size + TOKEN_SIZE;
      char **new_tokens = realloc(tokens, new_size * sizeof(char *));
      if (!new_tokens) {
        fprintf(stderr, "cpsh: allocation error\n");
        free(tokens);  // Free the original tokens
        exit(EXIT_FAILURE);
      }
      tokens = new_tokens;
      buffer_size = new_size;
    }

    token = strtok(NULL, delimiters);  // Get the next token
  }

  tokens[position] = NULL;  // Set the last token to NULL

  return tokens;  // Return the tokens
}