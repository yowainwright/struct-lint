#if defined(CONFIG_A)
static int helper(void) { return 1; }
int main(void) { return helper(); }
#else
int main(void) { return helper(); }
static int helper(void) { return 0; }
#endif
