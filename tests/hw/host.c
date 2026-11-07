/* The suite on the host: the reference output. */
#include <stdio.h>
#include "suite.h"
void out_str(const char *s) { fputs(s, stdout); }
int main(void) { run_suite(); return 0; }
