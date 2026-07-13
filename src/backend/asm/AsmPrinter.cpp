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

bool powerOfTwoMagnitude(std::int64_t value, std::uint64_t& magnitude) {
  if (value == 0) {
    return false;
  }
  magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                        : static_cast<std::uint64_t>(value);
  return (magnitude & (magnitude - 1U)) == 0;
}

unsigned log2PowerOfTwoMagnitude(std::uint64_t value) {
  unsigned shift = 0;
  while (value > 1U) {
    value >>= 1U;
    ++shift;
  }
  return shift;
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
      os << "  lla " << dst << ", " << operand.text << '\n';
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

std::string integerOperandRegister(std::ostringstream& os, const MachineFunction& function,
                                   const MachineOperand& operand, const std::string& scratch) {
  if (operand.kind == MachineOperandKind::PhysicalRegister) {
    return operand.text;
  }
  emitLoadIntegerOperand(os, function, operand, scratch);
  return scratch;
}

std::string floatOperandRegister(std::ostringstream& os, const MachineFunction& function,
                                 const MachineOperand& operand, const std::string& scratch) {
  if (operand.kind == MachineOperandKind::PhysicalRegister) {
    return operand.text;
  }
  emitLoadFloatOperand(os, function, operand, scratch);
  return scratch;
}

std::string resultRegister(const MachineOperand& result, const std::string& scratch) {
  return result.kind == MachineOperandKind::PhysicalRegister ? result.text : scratch;
}

void emitStoreResult(std::ostringstream& os, const MachineFunction& function,
                     const MachineOperand& result, const std::string& src);

void commitResult(std::ostringstream& os, const MachineFunction& function,
                  const MachineOperand& result, const std::string& reg) {
  if (result.kind != MachineOperandKind::PhysicalRegister) {
    emitStoreResult(os, function, result, reg);
  }
}

struct ArgumentMove {
  std::string destination;
  MachineOperand source;
};

void emitArgumentMove(std::ostringstream& os, const MachineFunction& function,
                      const ArgumentMove& move, bool isFloat) {
  if (isFloat) {
    emitLoadFloatOperand(os, function, move.source, move.destination);
  } else {
    emitLoadIntegerOperand(os, function, move.source, move.destination);
  }
}

void emitParallelArgumentMoves(std::ostringstream& os, const MachineFunction& function,
                               std::vector<ArgumentMove> moves, bool isFloat) {
  moves.erase(std::remove_if(moves.begin(), moves.end(), [](const ArgumentMove& move) {
                return move.source.kind == MachineOperandKind::PhysicalRegister &&
                       move.source.text == move.destination;
              }),
              moves.end());

  while (!moves.empty()) {
    auto ready = moves.end();
    for (auto candidate = moves.begin(); candidate != moves.end(); ++candidate) {
      const bool destinationStillNeeded =
          std::any_of(moves.begin(), moves.end(), [&](const ArgumentMove& move) {
            return move.source.kind == MachineOperandKind::PhysicalRegister &&
                   move.source.text == candidate->destination;
          });
      if (!destinationStillNeeded) {
        ready = candidate;
        break;
      }
    }

    if (ready != moves.end()) {
      emitArgumentMove(os, function, *ready, isFloat);
      moves.erase(ready);
      continue;
    }

    // A cycle can only be formed by physical argument registers. Preserve one source in a
    // reserved scratch register, then continue with the now-acyclic move set.
    const auto cycleSource = moves.front().source;
    const std::string scratch = isFloat ? "ft2" : "t2";
    if (isFloat) {
      emitLoadFloatOperand(os, function, cycleSource, scratch);
    } else {
      emitLoadIntegerOperand(os, function, cycleSource, scratch);
    }
    for (auto& move : moves) {
      if (move.source.kind == MachineOperandKind::PhysicalRegister &&
          move.source.text == cycleSource.text) {
        move.source = MachineOperand::physicalRegister(scratch, move.source.type);
      }
    }
  }
}

void emitLoadFromAddress(std::ostringstream& os, const MachineFunction& function,
                         const MachineOperand& address, const ValueType& valueType,
                         const std::string& dst) {
  if (address.kind == MachineOperandKind::PhysicalRegister) {
    emitLoadBaseOffset(os, loadMnemonic(valueType), dst, address.text, 0);
    return;
  }
  emitMaterializeAddress(os, function, address, "t0");
  emitLoadBaseOffset(os, loadMnemonic(valueType), dst, "t0", 0);
}

void emitStoreToAddress(std::ostringstream& os, const MachineFunction& function,
                        const MachineOperand& address, const ValueType& valueType,
                        const std::string& src) {
  if (address.kind == MachineOperandKind::PhysicalRegister) {
    emitStoreBaseOffset(os, storeMnemonic(valueType), src, address.text, 0);
    return;
  }
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
  if (result.kind == MachineOperandKind::PhysicalRegister) {
    if (source.kind == MachineOperandKind::PhysicalRegister &&
        source.text == result.text) {
      return;
    }
    if (result.type.isFloat()) {
      emitLoadFloatOperand(os, function, source, result.text);
    } else {
      emitLoadIntegerOperand(os, function, source, result.text);
    }
    return;
  }
  if (result.type.isFloat()) {
    emitLoadFloatOperand(os, function, source, "ft0");
    emitStoreResult(os, function, result, "ft0");
    return;
  }

  emitLoadIntegerOperand(os, function, source, "t0");
  emitStoreResult(os, function, result, "t0");
}

void emitIntegerCompare(std::ostringstream& os, const std::string& opcode,
                        const std::string& dst, const std::string& lhs,
                        const std::string& rhs) {
  if (opcode == "icmp.eq") {
    os << "  xor " << dst << ", " << lhs << ", " << rhs << '\n';
    os << "  seqz " << dst << ", " << dst << '\n';
  } else if (opcode == "icmp.ne") {
    os << "  xor " << dst << ", " << lhs << ", " << rhs << '\n';
    os << "  snez " << dst << ", " << dst << '\n';
  } else if (opcode == "icmp.slt") {
    os << "  slt " << dst << ", " << lhs << ", " << rhs << '\n';
  } else if (opcode == "icmp.sle") {
    os << "  slt " << dst << ", " << rhs << ", " << lhs << '\n';
    os << "  xori " << dst << ", " << dst << ", 1\n";
  } else if (opcode == "icmp.sgt") {
    os << "  slt " << dst << ", " << rhs << ", " << lhs << '\n';
  } else if (opcode == "icmp.sge") {
    os << "  slt " << dst << ", " << lhs << ", " << rhs << '\n';
    os << "  xori " << dst << ", " << dst << ", 1\n";
  }
}

void emitFloatCompare(std::ostringstream& os, const std::string& opcode,
                      const std::string& dst, const std::string& lhs,
                      const std::string& rhs) {
  if (opcode == "fcmp.oeq") {
    os << "  feq.s " << dst << ", " << lhs << ", " << rhs << '\n';
  } else if (opcode == "fcmp.one") {
    os << "  feq.s " << dst << ", " << lhs << ", " << rhs << '\n';
    os << "  xori " << dst << ", " << dst << ", 1\n";
  } else if (opcode == "fcmp.olt") {
    os << "  flt.s " << dst << ", " << lhs << ", " << rhs << '\n';
  } else if (opcode == "fcmp.ole") {
    os << "  fle.s " << dst << ", " << lhs << ", " << rhs << '\n';
  } else if (opcode == "fcmp.ogt") {
    os << "  flt.s " << dst << ", " << rhs << ", " << lhs << '\n';
  } else if (opcode == "fcmp.oge") {
    os << "  fle.s " << dst << ", " << rhs << ", " << lhs << '\n';
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
      const std::string result = resultRegister(instruction.result, "ft0");
      emitLoadBaseOffset(os, "flw", result, "s0", offset);
      commitResult(os, function, instruction.result, result);
    } else {
      const std::string result = resultRegister(instruction.result, "t0");
      emitLoadBaseOffset(os, loadMnemonic(instruction.result.type), result, "s0", offset);
      commitResult(os, function, instruction.result, result);
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
      const std::string sourceReg = floatOperandRegister(os, function, source, "ft0");
      if (frame.kind == MachineOperandKind::FrameIndex) {
        emitStoreBaseOffset(os, storeMnemonic(source.type), sourceReg, "s0",
                            -stackObject(function, frame.index).offset);
      } else {
        emitStoreStackSlot(os, function, frame.index, sourceReg, source.type);
      }
    } else {
      const std::string sourceReg = integerOperandRegister(os, function, source, "t0");
      if (frame.kind == MachineOperandKind::FrameIndex) {
        emitStoreBaseOffset(os, storeMnemonic(source.type), sourceReg, "s0",
                            -stackObject(function, frame.index).offset);
      } else {
        emitStoreStackSlot(os, function, frame.index, sourceReg, source.type);
      }
    }
    return;
  }

  if (opcode == "load") {
    if (instruction.result.type.isFloat()) {
      const std::string resultReg = resultRegister(instruction.result, "ft0");
      emitLoadFromAddress(os, function, instruction.operands[0], instruction.result.type,
                          resultReg);
      commitResult(os, function, instruction.result, resultReg);
    } else {
      const std::string resultReg = resultRegister(instruction.result, "t0");
      emitLoadFromAddress(os, function, instruction.operands[0], instruction.result.type,
                          resultReg);
      commitResult(os, function, instruction.result, resultReg);
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
      const std::string valueReg = floatOperandRegister(os, function, value, "ft0");
      emitStoreToAddress(os, function, address, value.type, valueReg);
    } else {
      const std::string valueReg = integerOperandRegister(os, function, value, "t1");
      emitStoreToAddress(os, function, address, value.type, valueReg);
    }
    return;
  }

  if (opcode == "br") {
    if (instruction.operands.size() == 1) {
      os << "  j " << labels.at(instruction.operands[0].text) << '\n';
      return;
    }
    const std::string condition =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    os << "  bne " << condition << ", x0, " << labels.at(instruction.operands[1].text)
       << '\n';
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
    std::vector<ArgumentMove> integerMoves;
    std::vector<ArgumentMove> floatMoves;
    for (std::size_t i = 1; i < instruction.operands.size(); ++i) {
      const auto& argument = instruction.operands[i];
      if (argument.type.isFloat()) {
        if (floatRegs < 8) {
          floatMoves.push_back(ArgumentMove{"fa" + std::to_string(floatRegs), argument});
          ++floatRegs;
        } else {
          emitLoadFloatOperand(os, function, argument, "ft0");
          emitStoreBaseOffset(os, "fsw", "ft0", "sp", static_cast<long long>(stackOffset));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      } else {
        if (integerRegs < 8) {
          integerMoves.push_back(ArgumentMove{"a" + std::to_string(integerRegs), argument});
          ++integerRegs;
        } else {
          emitLoadIntegerOperand(os, function, argument, "t0");
          emitStoreBaseOffset(os, storeMnemonic(argument.type), "t0", "sp",
                              static_cast<long long>(stackOffset));
          stackOffset += std::max<std::size_t>(8, module.target.pointerSize());
        }
      }
    }
    emitParallelArgumentMoves(os, function, std::move(integerMoves), false);
    emitParallelArgumentMoves(os, function, std::move(floatMoves), true);
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
    const auto& baseOperand = instruction.operands[0];
    const std::string base =
        baseOperand.kind == MachineOperandKind::PhysicalRegister
            ? baseOperand.text
            : (emitMaterializeAddress(os, function, baseOperand, "t0"), std::string("t0"));
    const auto& index = instruction.operands[1];
    const auto scale = instruction.operands[2].intValue;
    const std::string result = resultRegister(instruction.result, "t2");

    if (index.kind == MachineOperandKind::ImmediateInt) {
      const long long offset = index.intValue * scale;
      if (offset == 0) {
        if (result != base) {
          os << "  mv " << result << ", " << base << '\n';
        }
      } else {
        emitAddImmediate(os, result, base, offset, "t1");
      }
      commitResult(os, function, instruction.result, result);
      return;
    }

    const std::string indexReg = integerOperandRegister(os, function, index, "t1");
    if (scale == 0) {
      if (result != base) {
        os << "  mv " << result << ", " << base << '\n';
      }
      commitResult(os, function, instruction.result, result);
      return;
    }
    if (scale == 1) {
      os << "  add " << result << ", " << base << ", " << indexReg << '\n';
      commitResult(os, function, instruction.result, result);
      return;
    }
    if (isPowerOfTwo(scale)) {
      os << "  slli t1, " << indexReg << ", " << log2PowerOfTwo(scale) << '\n';
      os << "  add " << result << ", " << base << ", t1\n";
      commitResult(os, function, instruction.result, result);
      return;
    }

    emitLi(os, "t2", scale);
    os << "  mul t1, " << indexReg << ", t2\n";
    os << "  add " << result << ", " << base << ", t1\n";
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "neg") {
    const std::string source =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    const std::string result = resultRegister(instruction.result, "t1");
    os << "  " << (instruction.result.type.isInt32() ? "negw " : "neg ") << result << ", "
       << source << '\n';
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "fneg") {
    const std::string source =
        floatOperandRegister(os, function, instruction.operands[0], "ft0");
    const std::string result = resultRegister(instruction.result, "ft1");
    os << "  fneg.s " << result << ", " << source << '\n';
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" || opcode == "mod" ||
      opcode == "sdiv" || opcode == "srem" || opcode == "and" || opcode == "or") {
    const std::string lhs =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    const std::string result = resultRegister(instruction.result, "t2");
    const bool wordResult = instruction.result.type.isInt32();
    const bool isDivision = opcode == "div" || opcode == "sdiv";
    const bool isRemainder = opcode == "mod" || opcode == "srem";
    const auto& rhsOperand = instruction.operands[1];
    if ((opcode == "and" || opcode == "or") &&
        rhsOperand.kind == MachineOperandKind::ImmediateInt &&
        rhsOperand.intValue >= 0 && fitsSigned12(rhsOperand.intValue)) {
      os << "  " << (opcode == "and" ? "andi " : "ori ") << result << ", " << lhs
         << ", " << rhsOperand.intValue << '\n';
      if (instruction.result.type.isBool()) {
        os << "  andi " << result << ", " << result << ", 1\n";
      }
      commitResult(os, function, instruction.result, result);
      return;
    }
    std::uint64_t divisorMagnitude = 0;
    if (wordResult && (isDivision || isRemainder) &&
        rhsOperand.kind == MachineOperandKind::ImmediateInt &&
        rhsOperand.intValue >= INT32_MIN && rhsOperand.intValue <= INT32_MAX &&
        powerOfTwoMagnitude(rhsOperand.intValue, divisorMagnitude)) {
      const unsigned shift = log2PowerOfTwoMagnitude(divisorMagnitude);
      if (shift == 0) {
        if (isRemainder) {
          emitLi(os, result, 0);
        } else if (rhsOperand.intValue < 0) {
          os << "  negw " << result << ", " << lhs << '\n';
        } else if (result != lhs) {
          os << "  mv " << result << ", " << lhs << '\n';
        }
      } else {
        os << "  sraiw t1, " << lhs << ", 31\n";
        os << "  srliw t1, t1, " << 32U - shift << '\n';
        const std::string quotient = isRemainder ? "t2" : result;
        os << "  addw " << quotient << ", " << lhs << ", t1\n";
        os << "  sraiw " << quotient << ", " << quotient << ", " << shift << '\n';
        if (isRemainder) {
          os << "  slli t1, t2, " << shift << '\n';
          os << "  subw " << result << ", " << lhs << ", t1\n";
        } else if (rhsOperand.intValue < 0) {
          os << "  negw " << result << ", " << result << '\n';
        }
      }
      commitResult(os, function, instruction.result, result);
      return;
    }
    const std::string rhs = integerOperandRegister(os, function, rhsOperand, "t1");
    if (opcode == "add") {
      os << "  " << (wordResult ? "addw " : "add ") << result << ", " << lhs << ", " << rhs
         << '\n';
    } else if (opcode == "sub") {
      os << "  " << (wordResult ? "subw " : "sub ") << result << ", " << lhs << ", " << rhs
         << '\n';
    } else if (opcode == "mul") {
      os << "  " << (wordResult ? "mulw " : "mul ") << result << ", " << lhs << ", " << rhs
         << '\n';
    } else if (isDivision) {
      os << "  " << (wordResult ? "divw " : "div ") << result << ", " << lhs << ", " << rhs
         << '\n';
    } else if (isRemainder) {
      os << "  " << (wordResult ? "remw " : "rem ") << result << ", " << lhs << ", " << rhs
         << '\n';
    } else if (opcode == "and") {
      os << "  and " << result << ", " << lhs << ", " << rhs << '\n';
    } else if (opcode == "or") {
      os << "  or " << result << ", " << lhs << ", " << rhs << '\n';
    }
    if (instruction.result.type.isBool()) {
      os << "  andi " << result << ", " << result << ", 1\n";
    } else if (wordResult && (opcode == "and" || opcode == "or")) {
      os << "  sext.w " << result << ", " << result << '\n';
    }
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    const std::string lhs =
        floatOperandRegister(os, function, instruction.operands[0], "ft0");
    const std::string rhs =
        floatOperandRegister(os, function, instruction.operands[1], "ft1");
    const std::string result = resultRegister(instruction.result, "ft2");
    if (opcode == "fadd") {
      os << "  fadd.s " << result << ", " << lhs << ", " << rhs << '\n';
    } else if (opcode == "fsub") {
      os << "  fsub.s " << result << ", " << lhs << ", " << rhs << '\n';
    } else if (opcode == "fmul") {
      os << "  fmul.s " << result << ", " << lhs << ", " << rhs << '\n';
    } else {
      os << "  fdiv.s " << result << ", " << lhs << ", " << rhs << '\n';
    }
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode.rfind("icmp.", 0) == 0) {
    const std::string lhs =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    const std::string rhs =
        integerOperandRegister(os, function, instruction.operands[1], "t1");
    const std::string result = resultRegister(instruction.result, "t2");
    emitIntegerCompare(os, opcode, result, lhs, rhs);
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode.rfind("fcmp.", 0) == 0) {
    const std::string lhs =
        floatOperandRegister(os, function, instruction.operands[0], "ft0");
    const std::string rhs =
        floatOperandRegister(os, function, instruction.operands[1], "ft1");
    const std::string result = resultRegister(instruction.result, "t2");
    emitFloatCompare(os, opcode, result, lhs, rhs);
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    const std::string source =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    const std::string result = resultRegister(instruction.result, "t1");
    if (instruction.result.type.isBool()) {
      os << "  andi " << result << ", " << source << ", 1\n";
    } else if (instruction.result.type.isInt32()) {
      os << "  sext.w " << result << ", " << source << '\n';
    } else if (result != source) {
      os << "  mv " << result << ", " << source << '\n';
    }
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "sitofp" || opcode == "uitofp") {
    const std::string source =
        integerOperandRegister(os, function, instruction.operands[0], "t0");
    const std::string result = resultRegister(instruction.result, "ft0");
    if (instruction.operands[0].type.isInt64() || instruction.operands[0].type.isPointer()) {
      os << "  fcvt.s.l " << result << ", " << source << '\n';
    } else {
      os << "  fcvt.s.w " << result << ", " << source << '\n';
    }
    commitResult(os, function, instruction.result, result);
    return;
  }

  if (opcode == "fptosi") {
    const std::string source =
        floatOperandRegister(os, function, instruction.operands[0], "ft0");
    const std::string result = resultRegister(instruction.result, "t0");
    if (instruction.result.type.isInt64()) {
      os << "  fcvt.l.s " << result << ", " << source << ", rtz\n";
    } else {
      os << "  fcvt.w.s " << result << ", " << source << ", rtz\n";
      if (instruction.result.type.isBool()) {
        os << "  andi " << result << ", " << result << ", 1\n";
      }
    }
    commitResult(os, function, instruction.result, result);
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
