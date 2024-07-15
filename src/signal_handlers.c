#include "../include/shell.h"

// Global flag to control prompt reprinting
volatile sig_atomic_t print_prompt = 0;

void sigint_handler(int sig) {
  // Signal-safe flag setting
  print_prompt = 1;
}

void setup_sigint_handler() {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = sigint_handler;
  // Optionally set sa.sa_flags here
  sigaction(SIGINT, &sa, NULL);
}