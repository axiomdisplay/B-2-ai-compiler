#include <cstdio>

#include "TestHarness.h"

int main() {
  std::setvbuf(stdout, nullptr, _IONBF, 0);
  return b2::test::runAll();
}
