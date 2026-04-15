#include "compiler/backend/target/TargetInfo.h"

namespace compiler::backend::target {

TargetInfo TargetInfo::fromName(const std::string& name) {
  if (name == "aarch64" || name == "arm64" || name == "armv8" || name == "arm") {
    return TargetInfo{"aarch64", "aarch64-linux-gnu", 8, 16};
  }
  if (name == "riscv64" || name == "riscv") {
    return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16};
  }
  return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16};
}

}  // namespace compiler::backend::target
