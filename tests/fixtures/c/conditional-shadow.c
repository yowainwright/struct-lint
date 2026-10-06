static int helper(void) { return 1; }
int main(void) {
#if defined(CONFIG_A)
  int helper;
#else
  return helper();
#endif
  return 0;
}
