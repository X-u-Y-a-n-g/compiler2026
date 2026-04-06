#include "compiler/driver/Driver.h"

int main(int argc, const char* const* argv) {
  return compiler::driver::Driver{}.run(argc, argv);
}
