#ifndef SIGNAL_HANDLERS_H
#define SIGNAL_HANDLERS_H

#include <signal.h>

extern volatile sig_atomic_t print_prompt;

void sigint_handler(int sig);
void setup_sigint_handler();

#endif