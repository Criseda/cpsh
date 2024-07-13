#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

int bsh_exit(char **args) {
  return 0;  // Return 0 to signal that the shell should exit
}

char *bsh_read_line() {
  int buffer_size = 1024;
  int position = 0;
  char *buffer =
      malloc(sizeof(char) * buffer_size);  // Allocate memory for buffer
  int c;                                   // Character read from input

  if (!buffer) {
    fprintf(stderr, "bsh: allocation error\n");  // Print an error message
    exit(EXIT_FAILURE);                          // Exit the program
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
      buffer_size += 1024;  // Increase the buffer size
      buffer = realloc(
          buffer, buffer_size);  // Allocate some more memory for the buffer

      if (!buffer)  // If the buffer is NULL
      {
        fprintf(stderr, "bsh: allocation error\n");  // Print an error message
        exit(EXIT_FAILURE);                          // Exit the program
      }
    }
  }
}

char **bsh_tokenise(char *line) {
  int buffer_size = 64;
  int position = 0;
  char **tokens =
      malloc(buffer_size * sizeof(char *));  // Allocate memory for tokens
  char *token;
  char delimiters[] = " \t\r\n\a";  // Delimiters for tokenising the line

  if (!tokens) {
    fprintf(stderr, "bsh: allocation error\n");  // Print an error message
    exit(EXIT_FAILURE);                          // Exit the program
  }
  // tokenize the line
  token = strtok(line, delimiters);  // Get the first token
  while (token != NULL)              // While there are tokens
  {
    tokens[position] = token;  // Add the token to the tokens array
    position++;                // Increment the position

    if (position >= buffer_size) {
      buffer_size += 64;  // Increase the buffer size
      tokens = realloc(
          tokens,
          buffer_size *
              sizeof(char *));  // Allocate more memory to store more tokens

      if (!tokens)  // If the tokens are NULL
      {
        fprintf(stderr, "bsh: allocation error\n");  // Print an error message
        exit(EXIT_FAILURE);                          // Exit the program
      }
    }

    token = strtok(NULL, delimiters);  // Get the next token
  }

  tokens[position] = NULL;  // Set the last token to NULL

  return tokens;  // Return the tokens
}

int bsh_execute(char **args) {
  pid_t pid;
  int status;

  if (args[0] == NULL) {
    return 1;  // An empty command was entered
  }
  if (strcmp(args[0], "exit") == 0) {
    return bsh_exit(args);  // Call the bsh_exit function
  }

  pid = fork();  // Fork the process

  if (pid == 0) {
    // we are the child process
    if (execvp(args[0], args) == -1) {
      printf("bsh: %s: command not found\n", args[0]);
    }
    exit(EXIT_FAILURE);
  } else if (pid < 0) {
    printf("bsh: error forking\n");
  } else {
    // we are the parent process
    waitpid(pid, &status, WUNTRACED);
  }

  return 1;
}

void bsh_loop(void) {
  char *line;      // line of input
  char **args;     // array of arguments
  int status = 1;  // status of the shell

  do {
    printf("> ");
    line = bsh_read_line();      // Fix the assignment of line variable
    args = bsh_tokenise(line);   // Fix the assignment of args variable
    status = bsh_execute(args);  // Fix the assignment of status variable

    free(line);  // Free the line variable
    free(args);  // Free the args variable
  } while (status);
}

int main(int argc, char **argv) {
  bsh_loop();  // Call the bsh_loop function
  return 0;
}