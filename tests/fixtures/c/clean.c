#include <stddef.h>

static int helper(void);

int main(void) { return helper(); }

static int helper(void) { return 0; }
