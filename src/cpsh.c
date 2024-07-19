#include "../include/common.h"
#include "../include/executor.h"
#include "../include/history.h"
#include "../include/input.h"
#include "../include/prompt.h"
#include "../include/signal_handlers.h"

void cpsh_loop(void) {
  char *line;      // line of input
  char **args;     // array of arguments
  int status = 1;  // status of the shell

  // Load the history from the history file
  history_head = load_history();

  // Set up the signal handler for SIGINT
  setup_sigint_handler();

  do {
    if (print_prompt) {
      cpsh_print_prompt();
      print_prompt = 0;  // Reset the flag after printing
    } else {
      cpsh_print_prompt();
    }
    line = cpsh_read_line();
    // Add the command to the history
    add_to_history(line);
    args = cpsh_tokenise(line);
    status = cpsh_execute(args);

    free(line);  // Free the line variable
    free(args);  // Free the args variable
  } while (status);

  // Save the history to the history file
  save_history(history_head);
  // Free the history linked list
  free_history(history_head);
}
