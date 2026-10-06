int main(void) { return helper(); }
#if defined(CONFIG_A)
static int helper(void) { return 1; }
#else
static int helper(void) { return 0; }
#endif
