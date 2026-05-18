#include "../../../include/compiler/backend/target/TargetInfo.h"

namespace compiler::backend::target {

TargetInfo TargetInfo::fromName(const std::string& name) {
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
  return TargetInfo{"riscv64", "riscv64-unknown-elf", 8, 16, riscvIntArgs, riscvFloatArgs,
                    riscvCalleeSaved, riscvAllocatable, riscvScratchInt, riscvScratchFloat};
}

}  // namespace compiler::backend::target
