#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

// Global flag to control prompt reprinting
volatile sig_atomic_t print_prompt = 0;

void sigint_handler(int sig) {
  // Signal-safe flag setting
  print_prompt = 1;
  // ALT: write directly to stdout using write() since it's signal-safe
  // write(STDOUT_FILENO, "\ncpsh> ", 6);
}

void setup_sigint_handler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  // Optionally set sa.sa_flags here
  sigaction(SIGINT, &sa, NULL);
}

int cpsh_cd(char **args) {
  if (args[1] == NULL) {
    // No argument provided, change directory to home directory
    const char *home_dir = getenv("HOME");
    if (home_dir == NULL) {
      fprintf(stderr, "cpsh: unable to get home directory\n");
    } else {
      if (chdir(home_dir) != 0) {
        perror("cpsh");
      }
    }
  } else {
    // Check if the argument starts with a tilde (~)
    if (args[1][0] == '~') {
      // Get the HOME environment variable
      const char *home_dir = getenv("HOME");
      if (home_dir == NULL) {
        fprintf(stderr, "cpsh: unable to get home directory\n");
      } else {
        // Allocate memory for the new path
        // +1 for the null terminator, -1 because we're replacing the tilde
        char *new_path = malloc(strlen(home_dir) + strlen(args[1]));
        if (new_path == NULL) {
          fprintf(stderr, "cpsh: memory allocation error\n");
          return 1;
        }
        // Construct the new path
        strcpy(new_path, home_dir);
        strcat(new_path, args[1] + 1); // Skip the tilde

        // Change directory to the new path
        if (chdir(new_path) != 0) {
          perror("cpsh");
          free(new_path); // in case of error, free the allocated memory
          return 1;
        }
        // Free the allocated memory
        free(new_path);
      }
    } else {
      // Argument provided, change directory to specified path
      if (chdir(args[1]) != 0) {
        perror("cpsh");
      }
    }
  }
  return 1;
}

int cpsh_exit(char **args) {
  return 0; // Return 0 to signal that the shell should exit
}

char *cpsh_read_line() {
  int buffer_size = 1024;
  int position = 0;
  char *buffer =
      malloc(sizeof(char) * buffer_size); // Allocate memory for buffer
  int c;                                  // Character read from input

  if (!buffer) {
    fprintf(stderr, "cpsh: allocation error\n"); // Print an error message
    exit(EXIT_FAILURE);                          // Exit the program
  }

  while (1) // while true
  {
    c = getchar();             // Read a character from input
    if (c == EOF || c == '\n') // If the character is EOF or newline
    {
      buffer[position] = '\0'; // Set the end of the string
      return buffer;           // Return the buffer
    } else {
      buffer[position] = c; // Add the character to the buffer
    }
    position++; // Increment the position

    if (position >= buffer_size) {
      buffer_size += 1024; // Increase the buffer size
      buffer = realloc(buffer,
                       buffer_size); // Allocate some more memory for the buffer

      if (!buffer) // If the buffer is NULL
      {
        fprintf(stderr, "cpsh: allocation error\n"); // Print an error message
        free(buffer);                                // Free the buffer
        exit(EXIT_FAILURE);                          // Exit the program
      }
    }
  }
}

char **cpsh_tokenise(char *line) {
  int buffer_size = 64;
  int position = 0;
  char **tokens =
      malloc(buffer_size * sizeof(char *)); // Allocate memory for tokens
  char *token;
  char delimiters[] = " \t\r\n\a"; // Delimiters for tokenising the line

  if (!tokens) {
    fprintf(stderr, "cpsh: allocation error\n"); // Print an error message
    exit(EXIT_FAILURE);                          // Exit the program
  }
  // tokenize the line
  token = strtok(line, delimiters); // Get the first token
  while (token != NULL)             // While there are tokens
  {
    tokens[position] = token; // Add the token to the tokens array
    position++;               // Increment the position

    if (position >= buffer_size) {
      buffer_size += 64; // Increase the buffer size
      tokens = realloc(
          tokens,
          buffer_size *
              sizeof(char *)); // Allocate more memory to store more tokens

      if (!tokens) // If the tokens are NULL
      {
        fprintf(stderr, "cpsh: allocation error\n"); // Print an error message
        free(tokens);                                // Free the tokens
        exit(EXIT_FAILURE);                          // Exit the program
      }
    }

    token = strtok(NULL, delimiters); // Get the next token
  }

  tokens[position] = NULL; // Set the last token to NULL

  return tokens; // Return the tokens
}

char *get_cwd() {
  char cwd[1024];
  char *home = getenv("HOME");

  if (getcwd(cwd, sizeof(cwd)) == NULL) {
    perror("cpsh");
    return strdup("unknown");
  }

  if (home && strcmp(cwd, home) == 0) {
    return strdup("~");
  }

  if (home && strstr(cwd, home) == cwd) {
    char *relative_path = cwd + strlen(home);
    if (*relative_path == '\0') {
      return strdup("~");
    } else {
      char *result = malloc(strlen(relative_path) + 2);
      sprintf(result, "~%s", relative_path);
      return result;
    }
  }

  return strdup(cwd);
}

int cpsh_execute(char **args) {
  pid_t pid;
  int status;

  if (args[0] == NULL) {
    return 1; // An empty command was entered
  }
  if (strcmp(args[0], "cd") == 0) {
    return cpsh_cd(args); // Call the cpsh_cd function
  }
  if (strcmp(args[0], "exit") == 0) {
    return cpsh_exit(args); // Call the cpsh_exit function
  }

  pid = fork(); // Fork the process

  if (pid == 0) {
    // we are the child process
    if (execvp(args[0], args) == -1) {
      printf("cpsh: command not found: %s \n", args[0]);
    }
    exit(EXIT_FAILURE);
  } else if (pid < 0) {
    printf("cpsh: error forking\n");
  } else {
    // we are the parent process
    waitpid(pid, &status, WUNTRACED);
  }

  return 1;
}

void cpsh_loop(void) {
  char *line;     // line of input
  char **args;    // array of arguments
  int status = 1; // status of the shell

  // Set up the signal handler for SIGINT
  setup_sigint_handler();

  do {
    if (print_prompt) {
      char *cwd = get_cwd();
      printf("\n%s\n", cwd);
      free(cwd);
      printf("\ncpsh> ");
      fflush(stdout);
      print_prompt = 0; // Reset the flag after printing
    } else {
      char *cwd = get_cwd();
      printf("\n%s\n", cwd);
      free(cwd);
      printf("cpsh> ");
      fflush(stdout);
    }
    line = cpsh_read_line();
    args = cpsh_tokenise(line);
    status = cpsh_execute(args);

    free(line); // Free the line variable
    free(args); // Free the args variable
  } while (status);
}

int main(int argc, char **argv) {
  cpsh_loop(); // Call the cpsh_loop function
  return 0;
}