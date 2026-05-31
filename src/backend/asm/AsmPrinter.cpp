#include "../../../include/compiler/backend/asm/AsmPrinter.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>

namespace compiler::backend::asmcode {

namespace {

using compiler::backend::mir::MachineBlock;
using compiler::backend::mir::MachineFunction;
using compiler::backend::mir::MachineInstruction;
using compiler::backend::mir::MachineModule;
using compiler::backend::mir::MachineOperand;
using compiler::backend::mir::MachineOperandKind;
using compiler::backend::mir::ObjectType;
using compiler::backend::mir::ValueType;

constexpr const char* kAddrScratch = "t5";
constexpr const char* kImmScratch = "t6";

bool fitsSigned12(long long value) { return value >= -2048 && value <= 2047; }

bool isPowerOfTwo(std::int64_t value) {
  if (value <= 0) {
    return false;
  }
  const auto unsignedValue = static_cast<std::uint64_t>(value);
  return (unsignedValue & (unsignedValue - 1U)) == 0;
}

unsigned log2PowerOfTwo(std::int64_t value) {
  unsigned shift = 0;
  auto unsignedValue = static_cast<std::uint64_t>(value);
  while (unsignedValue > 1U) {
    unsignedValue >>= 1U;
    ++shift;
  }
  return shift;
}

std::string sanitizeName(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (char ch : text) {
    if (std::isalnum(static_cast<unsigned char>(ch)) != 0) {
      result.push_back(ch);
    } else {
      result.push_back('_');
    }
  }
  return result;
}

std::string blockLabel(const MachineFunction& function, const MachineBlock& block) {
  return ".L" + sanitizeName(function.name) + "_" + sanitizeName(block.name);
}

std::string epilogueLabel(const MachineFunction& function) {
  return ".L" + sanitizeName(function.name) + "_epilogue";
}

std::uint32_t floatBits(double value) {
  const float narrowed = static_cast<float>(value);
  std::uint32_t bits = 0;
  std::memcpy(&bits, &narrowed, sizeof(bits));
  return bits;
}

bool parseIntegerLiteral(const std::string& text, std::int64_t& value) {
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtoll(text.c_str(), &end, 0);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

bool parseFloatLiteral(const std::string& text, double& value) {
  char* end = nullptr;
  errno = 0;
  const auto parsed = std::strtod(text.c_str(), &end);
  if (errno == ERANGE || end == nullptr || *end != '\0') {
    return false;
  }
  value = parsed;
  return true;
}

void emitLi(std::ostringstream& os, const std::string& reg, long long value) {
  os << "  li " << reg << ", " << value << '\n';
}

void emitAddImmediate(std::ostringstream& os, const std::string& dst, const std::string& base,
                      long long immediate, const std::string& scratch) {
  if (fitsSigned12(immediate)) {
    os << "  addi " << dst << ", " << base << ", " << immediate << '\n';
    return;
  }
  emitLi(os, scratch, immediate);
  os << "  add " << dst << ", " << base << ", " << scratch << '\n';
}

void emitAdjustSp(std::ostringstream& os, long long delta) {
  if (delta == 0) {
    return;
  }
  if (fitsSigned12(delta)) {
    os << "  addi sp, sp, " << delta << '\n';
    return;
  }
  emitLi(os, kImmScratch, delta);
  os << "  add sp, sp, " << kImmScratch << '\n';
}

std::string loadMnemonic(const ValueType& type) {
  if (type.isFloat()) {
    return "flw";
  }
  if (type.isInt64() || type.isPointer()) {
    return "ld";
  }
  return "lw";
}

std::string storeMnemonic(const ValueType& type) {
  if (type.isFloat()) {
    return "fsw";
  }
  if (type.isInt64() || type.isPointer()) {
    return "sd";
  }
  return "sw";
}

void emitLoadBaseOffset(std::ostringstream& os, const std::string& mnemonic, const std::string& dst,
                        const std::string& base, long long offset) {
  if (fitsSigned12(offset)) {
    os << "  " << mnemonic << ' ' << dst << ", " << offset << '(' << base << ")\n";
    return;
  }
  emitAddImmediate(os, kAddrScratch, base, offset, kImmScratch);
  os << "  " << mnemonic << ' ' << dst << ", 0(" << kAddrScratch << ")\n";
}

void emitStoreBaseOffset(std::ostringstream& os, const std::string& mnemonic, const std::string& src,
                         const std::string& base, long long offset) {
  if (fitsSigned12(offset)) {
    os << "  " << mnemonic << ' ' << src << ", " << offset << '(' << base << ")\n";
    return;
  }
  emitAddImmediate(os, kAddrScratch, base, offset, kImmScratch);
  os << "  " << mnemonic << ' ' << src << ", 0(" << kAddrScratch << ")\n";
}

const compiler::backend::mir::StackObject& stackObject(const MachineFunction& function, int index) {
  return function.stackObjects.at(static_cast<std::size_t>(index));
}

void emitStackObjectAddress(std::ostringstream& os, const MachineFunction& function, int index,
                            const std::string& dst) {
  emitAddImmediate(os, dst, "s0", -stackObject(function, index).offset, kImmScratch);
}

void emitLoadStackSlot(std::ostringstream& os, const MachineFunction& function, int index,
                       const std::string& dst, const ValueType& type) {
  emitLoadBaseOffset(os, loadMnemonic(type), dst, "s0", -stackObject(function, index).offset);
}

void emitStoreStackSlot(std::ostringstream& os, const MachineFunction& function, int index,
                        const std::string& src, const ValueType& type) {
  emitStoreBaseOffset(os, storeMnemonic(type), src, "s0", -stackObject(function, index).offset);
}

void emitMaterializeAddress(std::ostringstream& os, const MachineFunction& function,
                            const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) {
        os << "  mv " << dst << ", " << operand.text << '\n';
      }
      return;
    case MachineOperandKind::FrameIndex:
      emitStackObjectAddress(os, function, operand.index, dst);
      return;
    case MachineOperandKind::StackObject:
      emitStackObjectAddress(os, function, operand.index, dst);
      return;
    case MachineOperandKind::GlobalAddress:
      os << "  la " << dst << ", " << operand.text << '\n';
      return;
    case MachineOperandKind::StackSlot:
      emitLoadStackSlot(os, function, operand.index, dst, operand.type);
      return;
    default:
      emitLi(os, dst, 0);
      return;
  }
}

void emitLoadIntegerOperand(std::ostringstream& os, const MachineFunction& function,
                            const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::ImmediateInt:
      emitLi(os, dst, operand.intValue);
      return;
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) {
        os << "  mv " << dst << ", " << operand.text << '\n';
      }
      return;
    case MachineOperandKind::FrameIndex:
      emitStackObjectAddress(os, function, operand.index, dst);
      return;
    case MachineOperandKind::StackSlot:
      emitLoadStackSlot(os, function, operand.index, dst, operand.type);
      return;
    case MachineOperandKind::StackObject:
    case MachineOperandKind::GlobalAddress:
      emitMaterializeAddress(os, function, operand, dst);
      return;
    default:
      emitLi(os, dst, 0);
      return;
  }
}

void emitLoadFloatOperand(std::ostringstream& os, const MachineFunction& function,
                          const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::ImmediateFloat:
      emitLi(os, kImmScratch, static_cast<long long>(floatBits(operand.floatValue)));
      os << "  fmv.w.x " << dst << ", " << kImmScratch << '\n';
      return;
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) {
        os << "  fmv.s " << dst << ", " << operand.text << '\n';
      }
      return;
    case MachineOperandKind::FrameIndex:
      emitLoadStackSlot(os, function, operand.index, dst, operand.type);
      return;
    case MachineOperandKind::StackSlot:
      emitLoadStackSlot(os, function, operand.index, dst, operand.type);
      return;
    default:
      emitLi(os, kImmScratch, 0);
      os << "  fmv.w.x " << dst << ", " << kImmScratch << '\n';
      return;
  }
}

void emitLoadFromAddress(std::ostringstream& os, const MachineFunction& function,
                         const MachineOperand& address, const ValueType& valueType,
                         const std::string& dst) {
  emitMaterializeAddress(os, function, address, "t0");
  emitLoadBaseOffset(os, loadMnemonic(valueType), dst, "t0", 0);
}

void emitStoreToAddress(std::ostringstream& os, const MachineFunction& function,
                        const MachineOperand& address, const ValueType& valueType,
                        const std::string& src) {
  emitMaterializeAddress(os, function, address, "t0");
  emitStoreBaseOffset(os, storeMnemonic(valueType), src, "t0", 0);
}

void emitStoreResult(std::ostringstream& os, const MachineFunction& function,
                     const MachineOperand& result, const std::string& src) {
  if (result.kind == MachineOperandKind::PhysicalRegister) {
    if (result.type.isFloat()) {
      if (result.text != src) {
        os << "  fmv.s " << result.text << ", " << src << '\n';
      }
      return;
    }
    if (result.type.isBool()) {
      os << "  andi " << result.text << ", " << src << ", 1\n";
      return;
    }
    if (result.type.isInt32()) {
      os << "  sext.w " << result.text << ", " << src << '\n';
      return;
    }
    if (result.text != src) {
      os << "  mv " << result.text << ", " << src << '\n';
    }
    return;
  }
  if (result.kind == MachineOperandKind::StackSlot) {
    emitStoreStackSlot(os, function, result.index, src, result.type);
    return;
  }
  if (result.kind == MachineOperandKind::FrameIndex) {
    emitStoreBaseOffset(os, storeMnemonic(result.type), src, "s0",
                        -stackObject(function, result.index).offset);
  }
}

void emitCopyInstruction(std::ostringstream& os, const MachineFunction& function,
                         const MachineInstruction& instruction) {
  if (!instruction.hasResult || instruction.operands.size() != 1) {
    return;
  }

  const auto& source = instruction.operands[0];
  const auto& result = instruction.result;
  if (result.type.isFloat()) {
    emitLoadFloatOperand(os, function, source, "ft0");
    emitStoreResult(os, function, result, "ft0");
    return;
  }

  emitLoadIntegerOperand(os, function, source, "t0");
  emitStoreResult(os, function, result, "t0");
}

void emitIntegerCompare(std::ostringstream& os, const MachineInstruction& instruction) {
  const auto& opcode = instruction.opcode;
  if (opcode == "icmp.eq") {
    os << "  xor t2, t0, t1\n";
    os << "  seqz t2, t2\n";
  } else if (opcode == "icmp.ne") {
    os << "  xor t2, t0, t1\n";
    os << "  snez t2, t2\n";
  } else if (opcode == "icmp.slt") {
    os << "  slt t2, t0, t1\n";
  } else if (opcode == "icmp.sle") {
    os << "  slt t2, t1, t0\n";
    os << "  xori t2, t2, 1\n";
  } else if (opcode == "icmp.sgt") {
    os << "  slt t2, t1, t0\n";
  } else if (opcode == "icmp.sge") {
    os << "  slt t2, t0, t1\n";
    os << "  xori t2, t2, 1\n";
  }
}

void emitFloatCompare(std::ostringstream& os, const MachineInstruction& instruction) {
  const auto& opcode = instruction.opcode;
  if (opcode == "fcmp.oeq") {
    os << "  feq.s t2, ft0, ft1\n";
  } else if (opcode == "fcmp.one") {
    os << "  feq.s t2, ft0, ft1\n";
    os << "  xori t2, t2, 1\n";
  } else if (opcode == "fcmp.olt") {
    os << "  flt.s t2, ft0, ft1\n";
  } else if (opcode == "fcmp.ole") {
    os << "  fle.s t2, ft0, ft1\n";
  } else if (opcode == "fcmp.ogt") {
    os << "  flt.s t2, ft1, ft0\n";
  } else if (opcode == "fcmp.oge") {
    os << "  fle.s t2, ft1, ft0\n";
  }
}

bool isAllZero(const compiler::ir::core::Constant& constant) {
  if (constant.isScalar()) {
    std::int64_t integerValue = 0;
    if (parseIntegerLiteral(constant.literal(), integerValue)) {
      return integerValue == 0;
    }
    double floatValue = 0.0;
    if (parseFloatLiteral(constant.literal(), floatValue)) {
      return floatValue == 0.0;
    }
    return false;
  }
  for (const auto& element : constant.elements()) {
    if (!isAllZero(element)) {
      return false;
    }
  }
  return true;
}

void emitZeroObject(std::ostringstream& os, const ObjectType& type,
                    const compiler::backend::target::TargetInfo& target) {
  os << "  .zero " << type.size(target) << '\n';
}

void emitScalarConstant(std::ostringstream& os, const compiler::ir::core::Constant& constant,
                        const ObjectType& type) {
  if (type.isFloatScalar()) {
    double value = 0.0;
    parseFloatLiteral(constant.literal(), value);
    os << "  .word " << floatBits(value) << '\n';
    return;
  }

  std::int64_t value = 0;
  parseIntegerLiteral(constant.literal(), value);
  if (type.isInt64Scalar()) {
    os << "  .quad " << value << '\n';
  } else {
    os << "  .word " << value << '\n';
  }
}

void emitConstant(std::ostringstream& os, const compiler::ir::core::Constant& constant,
                  const ObjectType& type, const compiler::backend::target::TargetInfo& target) {
  if (!type.isArray()) {
    emitScalarConstant(os, constant, type);
    return;
  }

  const ObjectType elementType = type.elementType();
  const std::size_t expectedCount = type.extents.front().value_or(0);
  const auto& elements = constant.elements();
  const std::size_t count = std::max(expectedCount, elements.size());
  for (std::size_t i = 0; i < count; ++i) {
    if (i < elements.size()) {
      emitConstant(os, elements[i], elementType, target);
    } else {
      emitZeroObject(os, elementType, target);
    }
  }
}

void emitGlobal(std::ostringstream& os, const MachineModule& module,
                const compiler::backend::mir::MachineGlobal& global) {
  const bool allZero = isAllZero(global.initializer);
  if (!global.isConst && allZero) {
    os << ".bss\n";
  } else if (global.isConst) {
    os << ".section .rodata\n";
  } else {
    os << ".data\n";
  }
  os << ".globl " << global.name << '\n';
  os << ".balign " << std::max<std::size_t>(4, global.type.alignment(module.target)) << '\n';
  os << global.name << ":\n";
  if (!global.isConst && allZero) {
    emitZeroObject(os, global.type, module.target);
  } else {
    emitConstant(os, global.initializer, global.type, module.target);
  }
}

void emitInstruction(std::ostringstream& os, const MachineModule& module, const MachineFunction& function,
                     const MachineInstruction& instruction, const std::string& returnLabel,
                     const std::unordered_map<std::string, std::string>& labels) {
  const auto& opcode = instruction.opcode;

  if (opcode == "stack.adjust") {
    emitAdjustSp(os, instruction.operands[0].intValue);
    return;
  }

  if (opcode == "stack.set_fp") {
    emitAddImmediate(os, "s0", "sp", instruction.operands[0].intValue, kImmScratch);
    return;
  }

  if (opcode == "stack.save") {
    const auto& reg = instruction.operands[0];
    const auto offset = instruction.operands[1].intValue;
    emitStoreBaseOffset(os, storeMnemonic(reg.type), reg.text, "sp", offset);
    return;
  }

  if (opcode == "stack.restore") {
    const auto offset = instruction.operands[0].intValue;
    emitLoadBaseOffset(os, loadMnemonic(instruction.result.type), instruction.result.text, "s0", offset);
    return;
  }

  if (opcode == "stack.param.load") {
    const auto offset = instruction.operands[0].intValue;
    if (instruction.result.type.isFloat()) {
      emitLoadBaseOffset(os, "flw", "ft0", "s0", offset);
      emitStoreResult(os, function, instruction.result, "ft0");
    } else {
      emitLoadBaseOffset(os, loadMnemonic(instruction.result.type), "t0", "s0", offset);
      emitStoreResult(os, function, instruction.result, "t0");
    }
    return;
  }

  if (opcode == "spill.reload" || opcode == "frame.load") {
    const auto& frame = instruction.operands[0];
    if (frame.kind == MachineOperandKind::FrameIndex) {
      emitLoadBaseOffset(os, loadMnemonic(instruction.result.type), instruction.result.text, "s0",
                         -stackObject(function, frame.index).offset);
    } else {
      emitLoadStackSlot(os, function, frame.index, instruction.result.text, instruction.result.type);
    }
    return;
  }

  if (opcode == "spill.store" || opcode == "frame.store") {
    const auto& source = instruction.operands[0];
    const auto& frame = instruction.operands[1];
    if (source.type.isFloat()) {
      emitLoadFloatOperand(os, function, source, "ft0");
      if (frame.kind == MachineOperandKind::FrameIndex) {
        emitStoreBaseOffset(os, storeMnemonic(source.type), "ft0", "s0",
                            -stackObject(function, frame.index).offset);
      } else {
        emitStoreStackSlot(os, function, frame.index, "ft0", source.type);
      }
    } else {
      emitLoadIntegerOperand(os, function, source, "t0");
      if (frame.kind == MachineOperandKind::FrameIndex) {
        emitStoreBaseOffset(os, storeMnemonic(source.type), "t0", "s0",
                            -stackObject(function, frame.index).offset);
      } else {
        emitStoreStackSlot(os, function, frame.index, "t0", source.type);
      }
    }
    return;
  }

  if (opcode == "load") {
    if (instruction.result.type.isFloat()) {
      emitLoadFromAddress(os, function, instruction.operands[0], instruction.result.type, "ft0");
      emitStoreResult(os, function, instruction.result, "ft0");
    } else {
      emitLoadFromAddress(os, function, instruction.operands[0], instruction.result.type, "t0");
      emitStoreResult(os, function, instruction.result, "t0");
    }
    return;
  }

  if (opcode == "copy") {
    emitCopyInstruction(os, function, instruction);
    return;
  }

  if (opcode == "store") {
    const auto& value = instruction.operands[0];
    const auto& address = instruction.operands[1];
    if (value.type.isFloat()) {
      emitLoadFloatOperand(os, function, value, "ft0");
      emitStoreToAddress(os, function, address, value.type, "ft0");
    } else {
      emitLoadIntegerOperand(os, function, value, "t1");
      emitStoreToAddress(os, function, address, value.type, "t1");
    }
    return;
  }

  if (opcode == "br") {
    if (instruction.operands.size() == 1) {
      os << "  j " << labels.at(instruction.operands[0].text) << '\n';
      return;
    }
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    os << "  bne t0, x0, " << labels.at(instruction.operands[1].text) << '\n';
    os << "  j " << labels.at(instruction.operands[2].text) << '\n';
    return;
  }

  if (opcode == "ret") {
    if (!instruction.operands.empty()) {
      if (instruction.operands[0].type.isFloat()) {
        emitLoadFloatOperand(os, function, instruction.operands[0], "fa0");
      } else {
        emitLoadIntegerOperand(os, function, instruction.operands[0], "a0");
      }
    }
    os << "  j " << returnLabel << '\n';
    return;
  }

  if (opcode == "call") {
    std::size_t integerRegs = 0;
    std::size_t floatRegs = 0;
    std::size_t stackOffset = 0;
    for (std::size_t i = 1; i < instruction.operands.size(); ++i) {
      const auto& argument = instruction.operands[i];
      if (argument.type.isFloat()) {
        if (floatRegs < 8) {
          emitLoadFloatOperand(os, function, argument, "fa" + std::to_string(floatRegs));
          ++floatRegs;
        } else {
          emitLoadFloatOperand(os, function, argument, "ft0");
          emitStoreBaseOffset(os, "fsw", "ft0", "sp", static_cast<long long>(stackOffset));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      } else {
        if (integerRegs < 8) {
          emitLoadIntegerOperand(os, function, argument, "a" + std::to_string(integerRegs));
          ++integerRegs;
        } else {
          emitLoadIntegerOperand(os, function, argument, "t0");
          emitStoreBaseOffset(os, storeMnemonic(argument.type), "t0", "sp",
                              static_cast<long long>(stackOffset));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      }
    }
    os << "  call " << instruction.operands[0].text << '\n';
    if (instruction.hasResult) {
      if (instruction.result.type.isFloat()) {
        emitStoreResult(os, function, instruction.result, "fa0");
      } else {
        emitStoreResult(os, function, instruction.result, "a0");
      }
    }
    return;
  }

  if (opcode == "gep" || opcode == "gep.linear" || opcode == "getelementptr" ||
      opcode == "getelementptr.linear") {
    emitMaterializeAddress(os, function, instruction.operands[0], "t0");
    const auto& index = instruction.operands[1];
    const auto scale = instruction.operands[2].intValue;

    if (index.kind == MachineOperandKind::ImmediateInt) {
      const long long offset = index.intValue * scale;
      if (offset == 0) {
        emitStoreResult(os, function, instruction.result, "t0");
      } else {
        emitAddImmediate(os, "t2", "t0", offset, "t1");
        emitStoreResult(os, function, instruction.result, "t2");
      }
      return;
    }

    emitLoadIntegerOperand(os, function, index, "t1");
    if (scale == 0) {
      emitStoreResult(os, function, instruction.result, "t0");
      return;
    }
    if (scale == 1) {
      os << "  add t2, t0, t1\n";
      emitStoreResult(os, function, instruction.result, "t2");
      return;
    }
    if (isPowerOfTwo(scale)) {
      os << "  slli t1, t1, " << log2PowerOfTwo(scale) << '\n';
      os << "  add t2, t0, t1\n";
      emitStoreResult(os, function, instruction.result, "t2");
      return;
    }

    emitLi(os, "t2", scale);
    os << "  mul t1, t1, t2\n";
    os << "  add t2, t0, t1\n";
    emitStoreResult(os, function, instruction.result, "t2");
    return;
  }

  if (opcode == "neg") {
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    os << "  neg t1, t0\n";
    emitStoreResult(os, function, instruction.result, "t1");
    return;
  }

  if (opcode == "fneg") {
    emitLoadFloatOperand(os, function, instruction.operands[0], "ft0");
    os << "  fneg.s ft1, ft0\n";
    emitStoreResult(os, function, instruction.result, "ft1");
    return;
  }

  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" || opcode == "mod" ||
      opcode == "sdiv" || opcode == "srem" || opcode == "and" || opcode == "or") {
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    emitLoadIntegerOperand(os, function, instruction.operands[1], "t1");
    if (opcode == "add") {
      os << "  add t2, t0, t1\n";
    } else if (opcode == "sub") {
      os << "  sub t2, t0, t1\n";
    } else if (opcode == "mul") {
      os << "  mul t2, t0, t1\n";
    } else if (opcode == "div" || opcode == "sdiv") {
      os << "  div t2, t0, t1\n";
    } else if (opcode == "mod" || opcode == "srem") {
      os << "  rem t2, t0, t1\n";
    } else if (opcode == "and") {
      os << "  and t2, t0, t1\n";
    } else if (opcode == "or") {
      os << "  or t2, t0, t1\n";
    }
    if (instruction.result.type.isBool()) {
      os << "  andi t2, t2, 1\n";
    } else if (instruction.result.type.isInt32()) {
      os << "  sext.w t2, t2\n";
    }
    emitStoreResult(os, function, instruction.result, "t2");
    return;
  }

  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    emitLoadFloatOperand(os, function, instruction.operands[0], "ft0");
    emitLoadFloatOperand(os, function, instruction.operands[1], "ft1");
    if (opcode == "fadd") {
      os << "  fadd.s ft2, ft0, ft1\n";
    } else if (opcode == "fsub") {
      os << "  fsub.s ft2, ft0, ft1\n";
    } else if (opcode == "fmul") {
      os << "  fmul.s ft2, ft0, ft1\n";
    } else {
      os << "  fdiv.s ft2, ft0, ft1\n";
    }
    emitStoreResult(os, function, instruction.result, "ft2");
    return;
  }

  if (opcode.rfind("icmp.", 0) == 0) {
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    emitLoadIntegerOperand(os, function, instruction.operands[1], "t1");
    emitIntegerCompare(os, instruction);
    emitStoreResult(os, function, instruction.result, "t2");
    return;
  }

  if (opcode.rfind("fcmp.", 0) == 0) {
    emitLoadFloatOperand(os, function, instruction.operands[0], "ft0");
    emitLoadFloatOperand(os, function, instruction.operands[1], "ft1");
    emitFloatCompare(os, instruction);
    emitStoreResult(os, function, instruction.result, "t2");
    return;
  }

  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    if (instruction.result.type.isBool()) {
      os << "  andi t1, t0, 1\n";
    } else if (instruction.result.type.isInt32()) {
      os << "  sext.w t1, t0\n";
    } else {
      os << "  mv t1, t0\n";
    }
    emitStoreResult(os, function, instruction.result, "t1");
    return;
  }

  if (opcode == "sitofp" || opcode == "uitofp") {
    emitLoadIntegerOperand(os, function, instruction.operands[0], "t0");
    if (instruction.operands[0].type.isInt64() || instruction.operands[0].type.isPointer()) {
      os << "  fcvt.s.l ft0, t0\n";
    } else {
      os << "  fcvt.s.w ft0, t0\n";
    }
    emitStoreResult(os, function, instruction.result, "ft0");
    return;
  }

  if (opcode == "fptosi") {
    emitLoadFloatOperand(os, function, instruction.operands[0], "ft0");
    if (instruction.result.type.isInt64()) {
      os << "  fcvt.l.s t0, ft0, rtz\n";
    } else {
      os << "  fcvt.w.s t0, ft0, rtz\n";
      if (instruction.result.type.isBool()) {
        os << "  andi t0, t0, 1\n";
      } else {
        os << "  sext.w t0, t0\n";
      }
    }
    emitStoreResult(os, function, instruction.result, "t0");
    return;
  }

  os << "  # unsupported opcode: " << opcode << '\n';
}

void emitFunction(std::ostringstream& os, const MachineModule& module, const MachineFunction& function) {
  std::unordered_map<std::string, std::string> labels;
  for (const auto& block : function.blocks) {
    labels.emplace(block.name, blockLabel(function, block));
  }
  const std::string retLabel = epilogueLabel(function);

  os << ".globl " << function.name << '\n';
  os << function.name << ":\n";
  for (const auto& instruction : function.prologueInstructions) {
    emitInstruction(os, module, function, instruction, retLabel, labels);
  }

  for (const auto& block : function.blocks) {
    os << labels.at(block.name) << ":\n";
    for (const auto& instruction : block.instructions) {
      emitInstruction(os, module, function, instruction, retLabel, labels);
    }
  }

  os << retLabel << ":\n";
  for (const auto& instruction : function.epilogueInstructions) {
    emitInstruction(os, module, function, instruction, retLabel, labels);
  }
  os << "  ret\n";
}

}  // namespace

std::string AsmPrinter::print(const MachineModule& module) const {
  std::ostringstream os;

  if (module.target.name() != "riscv64") {
    os << "# target backend not implemented for " << module.target.name() << '\n';
    return os.str();
  }

  for (const auto& global : module.globals) {
    emitGlobal(os, module, global);
  }

  os << ".text\n";
  for (const auto& function : module.functions) {
    emitFunction(os, module, function);
  }

  return os.str();
}

}  // namespace compiler::backend::asmcode
