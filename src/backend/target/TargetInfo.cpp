#include "../../../include/compiler/backend/target/TargetInfo.h"

namespace compiler::backend::target {

TargetInfo TargetInfo::fromName(const std::string& name) {
  const std::vector<std::string> armIntArgs = {"x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7"};
  const std::vector<std::string> armFloatArgs = {"s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
  const std::vector<std::string> armCalleeSaved = {"x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28"};
  const std::vector<std::string> armAllocatable = {
      "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27", "x28",
      "x0", "x1", "x2", "x3", "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14"};
  const std::vector<std::string> armScratchInt = {"x15", "x16", "x17"};
  const std::vector<std::string> armScratchFloat = {"s16", "s17", "s18"};
  const std::vector<std::string> riscvIntArgs = {"a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"};
  const std::vector<std::string> riscvFloatArgs = {"fa0", "fa1", "fa2", "fa3",
                                                   "fa4", "fa5", "fa6", "fa7"};
  const std::vector<std::string> riscvCalleeSaved = {"s1", "s2", "s3", "s4", "s5", "s6",
                                                     "s7", "s8", "s9", "s10", "s11"};
  const std::vector<std::string> riscvAllocatable = {"s1", "s2", "s3", "s4", "s5", "s6",
                                                     "s7", "s8", "s9", "s10", "s11"};
  const std::vector<std::string> riscvScratchInt = {"t3", "t4"};
  const std::vector<std::string> riscvScratchFloat = {"ft9", "ft10", "ft11"};

  if (name == "riscv64" || name == "riscv") {
    return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16, riscvIntArgs, riscvFloatArgs,
                      riscvCalleeSaved, riscvAllocatable, riscvScratchInt, riscvScratchFloat};
  }
  if (name == "aarch64" || name == "arm64" || name == "arm") {
    return TargetInfo{"aarch64", "aarch64-unknown-linux-gnu", 8, 16, armIntArgs, armFloatArgs,
                      armCalleeSaved, armAllocatable, armScratchInt, armScratchFloat};
  }
  return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16, riscvIntArgs, riscvFloatArgs,
                    riscvCalleeSaved, riscvAllocatable, riscvScratchInt, riscvScratchFloat};
}

}  // namespace compiler::backend::target
