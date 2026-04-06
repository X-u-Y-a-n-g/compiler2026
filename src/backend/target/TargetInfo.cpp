#include "compiler/backend/target/TargetInfo.h"

namespace compiler::backend::target {

TargetInfo TargetInfo::fromName(const std::string& name) {
  if (name == "armv7" || name == "arm") {
    return TargetInfo{"armv7", "armv7-none-eabi", 4, 8};
  }
  if (name == "riscv64" || name == "riscv") {
    return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16};
  }
  return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16};
}

}  // namespace compiler::backend::target
