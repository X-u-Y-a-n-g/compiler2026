#pragma once

namespace compiler::driver {

class Driver {
 public:
  int run(int argc, const char* const* argv) const;
};

}  // namespace compiler::driver
