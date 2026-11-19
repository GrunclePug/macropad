#include "check.h"

int failures = 0;
int checks = 0;

void run_input_tests();
void run_hid_tests();
void run_core_tests();
void run_tool_tests();
void run_config_tests();
void run_link_tests();

int main() {
  run_input_tests();
  run_hid_tests();
  run_core_tests();
  run_tool_tests();
  run_config_tests();
  run_link_tests();
  printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
