#pragma once
/* Starting the user's terminal emulator (Terminal launcher, Files "open terminal here"). */
#include <stdbool.h>

bool terminal_available(void);
/* Opens a terminal, in `dir` when given. Shows a toast and returns false when none is installed. */
bool terminal_open(const char *dir);
