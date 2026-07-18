#include "../../../include/compiler/backend/asm/AArch64AsmPrinter.h"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

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

constexpr const char* kAddrScratch = "x16";
constexpr const char* kImmScratch = "x17";
constexpr const char* kHiddenScratch = "x18";
constexpr const char* kFloatScratch = "s16";

bool isNarrow(const ValueType& type) { return type.isBool() || type.isInt32(); }

bool powerOfTwoMagnitude(std::int64_t value, std::uint64_t& magnitude) {
  if (value == 0) return false;
  magnitude = value < 0 ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                        : static_cast<std::uint64_t>(value);
  return (magnitude & (magnitude - 1U)) == 0;
}

unsigned log2PowerOfTwo(std::uint64_t value) {
  unsigned shift = 0;
  while (value > 1U) {
    value >>= 1U;
    ++shift;
  }
  return shift;
}

std::string integerRegister(const std::string& reg, const ValueType& type) {
  if (!isNarrow(type)) return reg;
  if (reg == "xzr") return "wzr";
  if (reg == "sp") return reg;
  if (reg.size() >= 2 && reg[0] == 'x') return "w" + reg.substr(1);
  return reg;
}

std::string doubleRegister(const std::string& reg) {
  return !reg.empty() && reg[0] == 's' ? "d" + reg.substr(1) : reg;
}

std::string sanitize(const std::string& text) {
  std::string result;
  result.reserve(text.size());
  for (const char ch : text) {
    result.push_back(std::isalnum(static_cast<unsigned char>(ch)) != 0 ? ch : '_');
  }
  return result;
}

std::string blockLabel(const MachineFunction& function, const MachineBlock& block) {
  return ".L" + sanitize(function.name) + "_" + sanitize(block.name);
}

std::string epilogueLabel(const MachineFunction& function) {
  return ".L" + sanitize(function.name) + "_epilogue";
}

std::uint32_t floatBits(double value) {
  const float narrowed = static_cast<float>(value);
  std::uint32_t bits = 0;
  std::memcpy(&bits, &narrowed, sizeof(bits));
  return bits;
}

bool parseInteger(const std::string& text, std::int64_t& value) {
  char* end = nullptr;
  errno = 0;
  value = std::strtoll(text.c_str(), &end, 0);
  return errno != ERANGE && end != nullptr && *end == '\0';
}

bool parseFloat(const std::string& text, double& value) {
  char* end = nullptr;
  errno = 0;
  value = std::strtod(text.c_str(), &end);
  return errno != ERANGE && end != nullptr && *end == '\0';
}

void emitImmediate(std::ostringstream& os, const std::string& reg, std::int64_t value,
                   const ValueType& type = ValueType::int64Type()) {
  const bool narrow = isNarrow(type);
  const std::string dst = integerRegister(reg, type);
  const std::uint64_t bits = narrow ? static_cast<std::uint32_t>(value)
                                    : static_cast<std::uint64_t>(value);
  const unsigned pieces = narrow ? 2U : 4U;
  bool emitted = false;
  for (unsigned index = 0; index < pieces; ++index) {
    const unsigned part = static_cast<unsigned>((bits >> (index * 16U)) & 0xffffU);
    if (part == 0 && emitted) continue;
    if (!emitted) {
      os << "  movz " << dst << ", #" << part;
      if (index != 0) os << ", lsl #" << index * 16U;
      os << '\n';
      emitted = true;
    } else if (part != 0) {
      os << "  movk " << dst << ", #" << part << ", lsl #" << index * 16U << '\n';
    }
  }
  if (!emitted) os << "  movz " << dst << ", #0\n";
}

void emitStackAdjust(std::ostringstream& os, long long delta) {
  const char* opcode = delta < 0 ? "sub" : "add";
  std::uint64_t amount = delta < 0 ? static_cast<std::uint64_t>(-(delta + 1)) + 1U
                                   : static_cast<std::uint64_t>(delta);
  while (amount != 0) {
    const auto chunk = std::min<std::uint64_t>(amount, 4095);
    os << "  " << opcode << " sp, sp, #" << chunk << '\n';
    amount -= chunk;
  }
}

void emitAddImmediate(std::ostringstream& os, const std::string& dst, const std::string& base,
                      long long offset) {
  if (offset >= 0 && offset <= 4095) {
    os << "  add " << dst << ", " << base << ", #" << offset << '\n';
  } else if (offset < 0 && offset >= -4095) {
    os << "  sub " << dst << ", " << base << ", #" << -offset << '\n';
  } else {
    emitImmediate(os, kHiddenScratch, offset);
    os << "  add " << dst << ", " << base << ", " << kHiddenScratch << '\n';
  }
}

bool addSubImmediate(std::int64_t value, std::uint64_t& encoded,
                     unsigned& shift) {
  const auto magnitude = value < 0
                             ? static_cast<std::uint64_t>(-(value + 1)) + 1U
                             : static_cast<std::uint64_t>(value);
  if (magnitude <= 4095) {
    encoded = magnitude;
    shift = 0;
    return true;
  }
  if ((magnitude & 4095U) == 0 && (magnitude >> 12U) <= 4095) {
    encoded = magnitude >> 12U;
    shift = 12;
    return true;
  }
  return false;
}

void emitEncodedImmediate(std::ostringstream& os, std::uint64_t value,
                          unsigned shift) {
  os << '#' << value;
  if (shift != 0) os << ", lsl #" << shift;
}

bool emitIntegerAddSubImmediate(std::ostringstream& os,
                                const std::string& opcode,
                                const std::string& destination,
                                const std::string& source,
                                std::int64_t value) {
  // Register 31 denotes SP in this encoding, so a ZR source must use the
  // register form instead.
  if (source == "xzr" || source == "wzr") return false;
  std::uint64_t encoded = 0;
  unsigned shift = 0;
  if (!addSubImmediate(value, encoded, shift)) return false;
  const bool negative = value < 0;
  const std::string selected =
      opcode == "add" ? (negative ? "sub" : "add")
                      : (negative ? "add" : "sub");
  os << "  " << selected << ' ' << destination << ", " << source << ", ";
  emitEncodedImmediate(os, encoded, shift);
  os << '\n';
  return true;
}

bool emitCompareImmediate(std::ostringstream& os, const std::string& lhs,
                          std::int64_t rhs) {
  // In the add/sub-immediate encoding register 31 denotes SP, not ZR.
  if (lhs == "xzr" || lhs == "wzr") return false;
  std::uint64_t encoded = 0;
  unsigned shift = 0;
  if (!addSubImmediate(rhs, encoded, shift)) return false;
  os << "  " << (rhs < 0 ? "cmn" : "cmp") << ' ' << lhs << ", ";
  emitEncodedImmediate(os, encoded, shift);
  os << '\n';
  return true;
}

unsigned accessSize(const ValueType& type) {
  return type.isInt64() || type.isPointer() ? 8U : 4U;
}

std::string loadMnemonic(const ValueType&) { return "ldr"; }
std::string storeMnemonic(const ValueType&) { return "str"; }

void emitLoad(std::ostringstream& os, const std::string& dst, const ValueType& type,
              const std::string& base, long long offset) {
  const unsigned size = accessSize(type);
  const std::string out = type.isFloat() ? dst : integerRegister(dst, type);
  if (offset >= 0 && offset % size == 0 && offset / size <= 4095) {
    os << "  " << loadMnemonic(type) << ' ' << out << ", [" << base;
    if (offset != 0) os << ", #" << offset;
    os << "]\n";
  } else if (offset >= -256 && offset <= 255) {
    os << "  ldur " << out << ", [" << base << ", #" << offset << "]\n";
  } else {
    emitAddImmediate(os, kAddrScratch, base, offset);
    os << "  ldr " << out << ", [" << kAddrScratch << "]\n";
  }
}

void emitStore(std::ostringstream& os, const std::string& src, const ValueType& type,
               const std::string& base, long long offset) {
  const unsigned size = accessSize(type);
  const std::string in = type.isFloat() ? src : integerRegister(src, type);
  if (offset >= 0 && offset % size == 0 && offset / size <= 4095) {
    os << "  " << storeMnemonic(type) << ' ' << in << ", [" << base;
    if (offset != 0) os << ", #" << offset;
    os << "]\n";
  } else if (offset >= -256 && offset <= 255) {
    os << "  stur " << in << ", [" << base << ", #" << offset << "]\n";
  } else {
    const std::string address = src == kAddrScratch ? kHiddenScratch : kAddrScratch;
    emitAddImmediate(os, address, base, offset);
    os << "  str " << in << ", [" << address << "]\n";
  }
}

unsigned scaleShift(std::int64_t scale) {
  unsigned shift = 0;
  for (auto value = static_cast<std::uint64_t>(scale); value > 1;
       value >>= 1U) {
    ++shift;
  }
  return shift;
}

void emitIndexedLoad(std::ostringstream& os, const std::string& dst,
                     const ValueType& type, const std::string& base,
                     const std::string& index, std::int64_t scale) {
  const std::string out = type.isFloat() ? dst : integerRegister(dst, type);
  os << "  ldr " << out << ", [" << base << ", " << index;
  if (scale != 1) os << ", lsl #" << scaleShift(scale);
  os << "]\n";
}

void emitIndexedStore(std::ostringstream& os, const std::string& src,
                      const ValueType& type, const std::string& base,
                      const std::string& index, std::int64_t scale) {
  const std::string in = type.isFloat() ? src : integerRegister(src, type);
  os << "  str " << in << ", [" << base << ", " << index;
  if (scale != 1) os << ", lsl #" << scaleShift(scale);
  os << "]\n";
}

const compiler::backend::mir::StackObject& stackObject(const MachineFunction& function, int index) {
  return function.stackObjects.at(static_cast<std::size_t>(index));
}

const char* frameBaseRegister(const MachineFunction& function) {
  return function.usesFramePointer ? "x29" : "sp";
}

long long frameBaseOffset(const MachineFunction& function, long long cfaOffset) {
  return function.usesFramePointer
             ? cfaOffset
             : static_cast<long long>(function.frameSize) + cfaOffset;
}

void emitAddress(std::ostringstream& os, const MachineFunction& function,
                 const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) os << "  mov " << dst << ", " << operand.text << '\n';
      break;
    case MachineOperandKind::FrameIndex:
    case MachineOperandKind::StackObject:
      emitAddImmediate(os, dst, frameBaseRegister(function),
                       frameBaseOffset(function, -stackObject(function, operand.index).offset));
      break;
    case MachineOperandKind::GlobalAddress:
      os << "  adrp " << dst << ", " << operand.text << '\n';
      os << "  add " << dst << ", " << dst << ", :lo12:" << operand.text << '\n';
      break;
    case MachineOperandKind::StackSlot:
      emitLoad(os, dst, operand.type, frameBaseRegister(function),
               frameBaseOffset(function, -stackObject(function, operand.index).offset));
      break;
    default:
      emitImmediate(os, dst, 0);
      break;
  }
}

void emitIntegerOperand(std::ostringstream& os, const MachineFunction& function,
                        const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::ImmediateInt:
      emitImmediate(os, dst, operand.intValue, operand.type);
      break;
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) {
        os << "  mov " << integerRegister(dst, operand.type) << ", "
           << integerRegister(operand.text, operand.type) << '\n';
      }
      break;
    case MachineOperandKind::FrameIndex:
    case MachineOperandKind::StackObject:
    case MachineOperandKind::GlobalAddress:
      emitAddress(os, function, operand, dst);
      break;
    case MachineOperandKind::StackSlot:
      emitLoad(os, dst, operand.type, frameBaseRegister(function),
               frameBaseOffset(function, -stackObject(function, operand.index).offset));
      break;
    default:
      emitImmediate(os, dst, 0, operand.type);
      break;
  }
}

void emitFloatOperand(std::ostringstream& os, const MachineFunction& function,
                      const MachineOperand& operand, const std::string& dst) {
  switch (operand.kind) {
    case MachineOperandKind::ImmediateFloat:
      emitImmediate(os, kImmScratch, floatBits(operand.floatValue), ValueType::int32Type());
      os << "  fmov " << dst << ", w17\n";
      break;
    case MachineOperandKind::PhysicalRegister:
      if (dst != operand.text) os << "  fmov " << dst << ", " << operand.text << '\n';
      break;
    case MachineOperandKind::FrameIndex:
    case MachineOperandKind::StackSlot:
      emitLoad(os, dst, operand.type, frameBaseRegister(function),
               frameBaseOffset(function, -stackObject(function, operand.index).offset));
      break;
    default:
      os << "  fmov " << dst << ", wzr\n";
      break;
  }
}

std::string integerOperand(std::ostringstream& os, const MachineFunction& function,
                           const MachineOperand& operand, const std::string& scratch) {
  if (operand.kind == MachineOperandKind::PhysicalRegister) return operand.text;
  if (operand.kind == MachineOperandKind::ImmediateInt && operand.intValue == 0) return "xzr";
  emitIntegerOperand(os, function, operand, scratch);
  return scratch;
}

std::string floatOperand(std::ostringstream& os, const MachineFunction& function,
                         const MachineOperand& operand, const std::string& scratch) {
  if (operand.kind == MachineOperandKind::PhysicalRegister) return operand.text;
  emitFloatOperand(os, function, operand, scratch);
  return scratch;
}

std::string resultRegister(const MachineOperand& result, const std::string& scratch) {
  return result.kind == MachineOperandKind::PhysicalRegister ? result.text : scratch;
}

void storeResult(std::ostringstream& os, const MachineFunction& function,
                 const MachineOperand& result, const std::string& source) {
  if (result.kind == MachineOperandKind::PhysicalRegister) {
    if (result.text == source) return;
    if (result.type.isFloat()) os << "  fmov " << result.text << ", " << source << '\n';
    else os << "  mov " << integerRegister(result.text, result.type) << ", "
            << integerRegister(source, result.type) << '\n';
  } else if (result.kind == MachineOperandKind::StackSlot || result.kind == MachineOperandKind::FrameIndex) {
    emitStore(os, source, result.type, frameBaseRegister(function),
              frameBaseOffset(function, -stackObject(function, result.index).offset));
  }
}

struct ArgumentMove { std::string destination; MachineOperand source; };

void emitParallelMoves(std::ostringstream& os, const MachineFunction& function,
                       std::vector<ArgumentMove> moves, bool floating) {
  moves.erase(std::remove_if(moves.begin(), moves.end(), [](const auto& move) {
    return move.source.kind == MachineOperandKind::PhysicalRegister && move.source.text == move.destination;
  }), moves.end());
  while (!moves.empty()) {
    auto ready = moves.end();
    for (auto it = moves.begin(); it != moves.end(); ++it) {
      const bool needed = std::any_of(moves.begin(), moves.end(), [&](const auto& move) {
        return move.source.kind == MachineOperandKind::PhysicalRegister && move.source.text == it->destination;
      });
      if (!needed) { ready = it; break; }
    }
    if (ready != moves.end()) {
      if (floating) emitFloatOperand(os, function, ready->source, ready->destination);
      else emitIntegerOperand(os, function, ready->source, ready->destination);
      moves.erase(ready);
      continue;
    }
    const auto source = moves.front().source;
    const std::string scratch = floating ? kFloatScratch : kAddrScratch;
    if (floating) emitFloatOperand(os, function, source, scratch);
    else emitIntegerOperand(os, function, source, scratch);
    for (auto& move : moves) {
      if (move.source.kind == MachineOperandKind::PhysicalRegister && move.source.text == source.text)
        move.source = MachineOperand::physicalRegister(scratch, move.source.type);
    }
  }
}

bool allZero(const compiler::ir::core::Constant& constant) {
  if (constant.isScalar()) {
    std::int64_t integer = 0;
    if (parseInteger(constant.literal(), integer)) return integer == 0;
    double floating = 0;
    return parseFloat(constant.literal(), floating) && floating == 0.0;
  }
  return std::all_of(constant.elements().begin(), constant.elements().end(), allZero);
}

void emitConstant(std::ostringstream& os, const compiler::ir::core::Constant& constant,
                  const ObjectType& type, const compiler::backend::target::TargetInfo& target) {
  if (!type.isArray()) {
    if (type.isFloatScalar()) {
      double value = 0; parseFloat(constant.literal(), value);
      os << "  .word " << floatBits(value) << '\n';
    } else {
      std::int64_t value = 0; parseInteger(constant.literal(), value);
      os << (type.isInt64Scalar() ? "  .xword " : "  .word ") << value << '\n';
    }
    return;
  }
  const ObjectType element = type.elementType();
  const std::size_t count = std::max(type.extents.front().value_or(0), constant.elements().size());
  for (std::size_t index = 0; index < count; ++index) {
    if (index < constant.elements().size()) emitConstant(os, constant.elements()[index], element, target);
    else os << "  .zero " << element.size(target) << '\n';
  }
}

void emitGlobal(std::ostringstream& os, const MachineModule& module,
                const compiler::backend::mir::MachineGlobal& global) {
  const bool zero = allZero(global.initializer);
  os << (!global.isConst && zero ? ".bss\n" : global.isConst ? ".section .rodata\n" : ".data\n");
  os << ".globl " << global.name << '\n';
  os << ".type " << global.name << ", %object\n";
  os << ".balign " << std::max<std::size_t>(4, global.type.alignment(module.target)) << '\n';
  os << global.name << ":\n";
  if (!global.isConst && zero) os << "  .zero " << global.type.size(module.target) << '\n';
  else emitConstant(os, global.initializer, global.type, module.target);
  os << ".size " << global.name << ", .-" << global.name << '\n';
}

std::string conditionFor(const std::string& predicate) {
  if (predicate == "icmp.eq") return "eq";
  if (predicate == "icmp.ne") return "ne";
  if (predicate == "icmp.slt") return "lt";
  if (predicate == "icmp.sle") return "le";
  if (predicate == "icmp.sgt") return "gt";
  if (predicate == "icmp.sge") return "ge";
  return "al";
}

std::string inverseCondition(const std::string& condition) {
  if (condition == "eq") return "ne"; if (condition == "ne") return "eq";
  if (condition == "lt") return "ge"; if (condition == "ge") return "lt";
  if (condition == "le") return "gt"; if (condition == "gt") return "le";
  return "nv";
}

void emitInstruction(std::ostringstream& os, const MachineModule&,
                     const MachineFunction& function, const MachineInstruction& instruction,
                     const std::string& returnLabel,
                     const std::unordered_map<std::string, std::string>& labels,
                     const std::string* fallthrough = nullptr) {
  const auto& opcode = instruction.opcode;
  if (opcode == "stack.adjust") { emitStackAdjust(os, instruction.operands[0].intValue); return; }
  if (opcode == "stack.set_fp") { emitAddImmediate(os, "x29", "sp", instruction.operands[0].intValue); return; }
  if (opcode == "stack.save") {
    const auto& reg = instruction.operands[0];
    if (reg.type.isFloat()) {
      ValueType wide = ValueType::int64Type();
      emitStore(os, doubleRegister(reg.text), wide, "sp", instruction.operands[1].intValue);
    } else emitStore(os, reg.text, reg.type, "sp", instruction.operands[1].intValue);
    return;
  }
  if (opcode == "stack.restore") {
    if (instruction.result.type.isFloat()) {
      ValueType wide = ValueType::int64Type();
      emitLoad(os, doubleRegister(instruction.result.text), wide, frameBaseRegister(function),
               frameBaseOffset(function, instruction.operands[0].intValue));
    } else {
      emitLoad(os, instruction.result.text, instruction.result.type, frameBaseRegister(function),
               frameBaseOffset(function, instruction.operands[0].intValue));
    }
    return;
  }
  if (opcode == "stack.param.load") {
    const std::string dst = resultRegister(instruction.result, instruction.result.type.isFloat() ? kFloatScratch : kAddrScratch);
    emitLoad(os, dst, instruction.result.type, frameBaseRegister(function),
             frameBaseOffset(function, instruction.operands[0].intValue));
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "spill.reload" || opcode == "frame.load") {
    const auto& frame = instruction.operands[0];
    emitLoad(os, instruction.result.text, instruction.result.type, frameBaseRegister(function),
             frameBaseOffset(function, -stackObject(function, frame.index).offset)); return;
  }
  if (opcode == "spill.store" || opcode == "frame.store") {
    const auto& source = instruction.operands[0];
    const std::string reg = source.type.isFloat() ? floatOperand(os, function, source, kFloatScratch)
                                                  : integerOperand(os, function, source, kAddrScratch);
    emitStore(os, reg, source.type, frameBaseRegister(function),
              frameBaseOffset(function,
                              -stackObject(function, instruction.operands[1].index).offset)); return;
  }
  if (opcode == "copy") {
    if (!instruction.hasResult || instruction.operands.empty()) return;
    const std::string dst = resultRegister(instruction.result, instruction.result.type.isFloat() ? kFloatScratch : kAddrScratch);
    if (instruction.result.type.isFloat()) emitFloatOperand(os, function, instruction.operands[0], dst);
    else emitIntegerOperand(os, function, instruction.operands[0], dst);
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "load") {
    const auto& address = instruction.operands[0];
    const std::string dst = resultRegister(instruction.result, instruction.result.type.isFloat() ? kFloatScratch : kImmScratch);
    if (address.kind == MachineOperandKind::PhysicalRegister) emitLoad(os, dst, instruction.result.type, address.text, 0);
    else { emitAddress(os, function, address, kAddrScratch); emitLoad(os, dst, instruction.result.type, kAddrScratch, 0); }
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "load.indexed") {
    const auto& baseOp = instruction.operands[0];
    const auto& indexOp = instruction.operands[1];
    const auto scale = instruction.operands[2].intValue;
    const std::string base =
        baseOp.kind == MachineOperandKind::PhysicalRegister ? baseOp.text
                                                            : kAddrScratch;
    if (baseOp.kind != MachineOperandKind::PhysicalRegister) {
      emitAddress(os, function, baseOp, kAddrScratch);
    }
    const std::string dst = resultRegister(
        instruction.result,
        instruction.result.type.isFloat() ? kFloatScratch : kImmScratch);
    if (indexOp.kind == MachineOperandKind::ImmediateInt) {
      emitLoad(os, dst, instruction.result.type, base,
               indexOp.intValue * scale);
    } else {
      const std::string index =
          integerOperand(os, function, indexOp, kImmScratch);
      emitIndexedLoad(os, dst, instruction.result.type, base, index, scale);
    }
    storeResult(os, function, instruction.result, dst);
    return;
  }
  if (opcode == "store") {
    const auto& value = instruction.operands[0]; const auto& address = instruction.operands[1];
    const std::string source = value.type.isFloat() ? floatOperand(os, function, value, kFloatScratch)
                                                    : integerOperand(os, function, value, kImmScratch);
    const std::string base = address.kind == MachineOperandKind::PhysicalRegister ? address.text : kAddrScratch;
    if (address.kind != MachineOperandKind::PhysicalRegister) emitAddress(os, function, address, kAddrScratch);
    emitStore(os, source, value.type, base, 0); return;
  }
  if (opcode == "store.indexed") {
    const auto& value = instruction.operands[0];
    const auto& baseOp = instruction.operands[1];
    const auto& indexOp = instruction.operands[2];
    const auto scale = instruction.operands[3].intValue;
    const std::string base =
        baseOp.kind == MachineOperandKind::PhysicalRegister ? baseOp.text
                                                            : kAddrScratch;
    if (baseOp.kind != MachineOperandKind::PhysicalRegister) {
      emitAddress(os, function, baseOp, kAddrScratch);
    }
    const std::string source =
        value.type.isFloat()
            ? floatOperand(os, function, value, kFloatScratch)
            : integerOperand(os, function, value,
                             indexOp.kind == MachineOperandKind::ImmediateInt
                                 ? kImmScratch
                                 : kHiddenScratch);
    if (indexOp.kind == MachineOperandKind::ImmediateInt) {
      emitStore(os, source, value.type, base, indexOp.intValue * scale);
    } else {
      const std::string index =
          integerOperand(os, function, indexOp, kImmScratch);
      emitIndexedStore(os, source, value.type, base, index, scale);
    }
    return;
  }
  if (opcode == "br") {
    if (instruction.operands.size() == 1) {
      const auto& target = labels.at(instruction.operands[0].text);
      if (fallthrough == nullptr || *fallthrough != target) os << "  b " << target << '\n';
    } else {
      const auto condition = integerOperand(os, function, instruction.operands[0], kAddrScratch);
      const auto& yes = labels.at(instruction.operands[1].text); const auto& no = labels.at(instruction.operands[2].text);
      if (fallthrough != nullptr && *fallthrough == no) os << "  cbnz " << integerRegister(condition, instruction.operands[0].type) << ", " << yes << '\n';
      else if (fallthrough != nullptr && *fallthrough == yes) os << "  cbz " << integerRegister(condition, instruction.operands[0].type) << ", " << no << '\n';
      else os << "  cbnz " << integerRegister(condition, instruction.operands[0].type) << ", " << yes << "\n  b " << no << '\n';
    }
    return;
  }
  if (opcode.rfind("br.icmp.", 0) == 0) {
    const auto& lhsOp = instruction.operands[0]; const auto& rhsOp = instruction.operands[1];
    const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
    const auto lhsRegister = integerRegister(lhs, lhsOp.type);
    if (rhsOp.kind != MachineOperandKind::ImmediateInt ||
        !emitCompareImmediate(os, lhsRegister, rhsOp.intValue)) {
      const auto rhs = integerOperand(os, function, rhsOp, kImmScratch);
      os << "  cmp " << lhsRegister << ", " << integerRegister(rhs, lhsOp.type) << '\n';
    }
    const auto cond = conditionFor(opcode.substr(3));
    const auto& yes = labels.at(instruction.operands[2].text); const auto& no = labels.at(instruction.operands[3].text);
    if (fallthrough != nullptr && *fallthrough == no) os << "  b." << cond << ' ' << yes << '\n';
    else if (fallthrough != nullptr && *fallthrough == yes) os << "  b." << inverseCondition(cond) << ' ' << no << '\n';
    else os << "  b." << cond << ' ' << yes << "\n  b " << no << '\n';
    return;
  }
  if (opcode == "ret") {
    if (!instruction.operands.empty()) {
      if (instruction.operands[0].type.isFloat()) emitFloatOperand(os, function, instruction.operands[0], "s0");
      else emitIntegerOperand(os, function, instruction.operands[0], "x0");
    }
    os << "  b " << returnLabel << '\n'; return;
  }
  if (opcode == "call") {
    std::size_t ints = 0, floats = 0, stack = 0;
    std::vector<ArgumentMove> integerMoves, floatMoves;
    for (std::size_t index = 1; index < instruction.operands.size(); ++index) {
      const auto& argument = instruction.operands[index];
      if (argument.type.isFloat()) {
        if (floats < 8) floatMoves.push_back({"s" + std::to_string(floats++), argument});
        else { emitFloatOperand(os, function, argument, kFloatScratch); emitStore(os, kFloatScratch, argument.type, "sp", stack); stack += 8; }
      } else {
        if (ints < 8) integerMoves.push_back({"x" + std::to_string(ints++), argument});
        else { emitIntegerOperand(os, function, argument, kImmScratch); emitStore(os, kImmScratch, argument.type, "sp", stack); stack += 8; }
      }
    }
    emitParallelMoves(os, function, std::move(integerMoves), false);
    emitParallelMoves(os, function, std::move(floatMoves), true);
    os << "  bl " << instruction.operands[0].text << '\n';
    if (instruction.hasResult) storeResult(os, function, instruction.result, instruction.result.type.isFloat() ? "s0" : "x0");
    return;
  }
  if (opcode == "gep" || opcode == "gep.linear" || opcode == "getelementptr" || opcode == "getelementptr.linear") {
    const auto& baseOp = instruction.operands[0]; const auto& indexOp = instruction.operands[1];
    const auto scale = instruction.operands[2].intValue;
    const std::string base = baseOp.kind == MachineOperandKind::PhysicalRegister ? baseOp.text : kAddrScratch;
    if (baseOp.kind != MachineOperandKind::PhysicalRegister) emitAddress(os, function, baseOp, kAddrScratch);
    const std::string dst = resultRegister(instruction.result, kImmScratch);
    if (indexOp.kind == MachineOperandKind::ImmediateInt) emitAddImmediate(os, dst, base, indexOp.intValue * scale);
    else {
      const std::string index = integerOperand(os, function, indexOp, kImmScratch);
      if (scale == 1) os << "  add " << dst << ", " << base << ", " << index << '\n';
      else if (scale > 0 && (scale & (scale - 1)) == 0) {
        unsigned shift = 0; for (auto n = static_cast<std::uint64_t>(scale); n > 1; n >>= 1) ++shift;
        os << "  add " << dst << ", " << base << ", " << index << ", lsl #" << shift << '\n';
      } else {
        emitImmediate(os, kHiddenScratch, scale);
        os << "  mul x18, " << index << ", x18\n";
        os << "  add " << dst << ", " << base << ", x18\n";
      }
    }
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "neg") {
    const auto source = integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto dst = resultRegister(instruction.result, kImmScratch);
    os << "  neg " << integerRegister(dst, instruction.result.type) << ", " << integerRegister(source, instruction.result.type) << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "select") {
    const auto cond = integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  cmp " << integerRegister(cond, instruction.operands[0].type) << ", #0\n";
    const auto yes = integerOperand(os, function, instruction.operands[1], kImmScratch);
    const auto no = integerOperand(os, function, instruction.operands[2], "x17");
    const auto dst = resultRegister(instruction.result, kHiddenScratch);
    os << "  csel " << integerRegister(dst, instruction.result.type) << ", " << integerRegister(yes, instruction.result.type)
       << ", " << integerRegister(no, instruction.result.type) << ", ne\n";
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "fneg") {
    const auto source = floatOperand(os, function, instruction.operands[0], kFloatScratch);
    const auto dst = resultRegister(instruction.result, "s17"); os << "  fneg " << dst << ", " << source << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "madd" && instruction.operands.size() == 3) {
    const auto lhs = integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto rhs = integerOperand(os, function, instruction.operands[1], kImmScratch);
    const auto accumulator =
        integerOperand(os, function, instruction.operands[2], kHiddenScratch);
    const auto dst = resultRegister(instruction.result, kImmScratch);
    const auto type = instruction.result.type;
    os << "  madd " << integerRegister(dst, type) << ", "
       << integerRegister(lhs, type) << ", " << integerRegister(rhs, type)
       << ", " << integerRegister(accumulator, type) << '\n';
    storeResult(os, function, instruction.result, dst);
    return;
  }
  if (opcode == "add" || opcode == "sub" || opcode == "mul" || opcode == "div" || opcode == "mod" ||
      opcode == "sdiv" || opcode == "srem" || opcode == "and" || opcode == "or" || opcode == "xor" ||
      opcode == "shl" || opcode == "ashr" || opcode == "lshr") {
    const auto& lhsOp = instruction.operands[0]; const auto& rhsOp = instruction.operands[1];
    const auto dst = resultRegister(instruction.result, kImmScratch);
    const auto d = integerRegister(dst, instruction.result.type);
    const bool isDivision = opcode == "div" || opcode == "sdiv";
    const bool isRemainder = opcode == "mod" || opcode == "srem";
    if ((opcode == "add" || opcode == "sub") &&
        rhsOp.kind == MachineOperandKind::ImmediateInt) {
      const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
      if (emitIntegerAddSubImmediate(
              os, opcode, d, integerRegister(lhs, instruction.result.type),
              rhsOp.intValue)) {
        storeResult(os, function, instruction.result, dst);
        return;
      }
    }
    if (opcode == "add" && lhsOp.kind == MachineOperandKind::ImmediateInt) {
      const auto rhs = integerOperand(os, function, rhsOp, kAddrScratch);
      if (emitIntegerAddSubImmediate(
              os, opcode, d, integerRegister(rhs, instruction.result.type),
              lhsOp.intValue)) {
        storeResult(os, function, instruction.result, dst);
        return;
      }
    }
    const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
    const auto l = integerRegister(lhs, instruction.result.type);
    std::uint64_t divisorMagnitude = 0;
    if ((isDivision || isRemainder) &&
        rhsOp.kind == MachineOperandKind::ImmediateInt &&
        powerOfTwoMagnitude(rhsOp.intValue, divisorMagnitude)) {
      const unsigned width = isNarrow(instruction.result.type) ? 32U : 64U;
      const unsigned shift = log2PowerOfTwo(divisorMagnitude);
      if (shift < width) {
        if (shift == 0) {
          if (isRemainder) {
            emitImmediate(os, dst, 0, instruction.result.type);
          } else if (rhsOp.intValue < 0) {
            os << "  neg " << d << ", " << l << '\n';
          } else if (d != l) {
            os << "  mov " << d << ", " << l << '\n';
          }
        } else if (isRemainder) {
          const auto mask = divisorMagnitude - 1U;
          if (shift == 1) {
            os << "  and " << d << ", " << l << ", #1\n";
            os << "  cmp " << l << ", #0\n";
            os << "  cneg " << d << ", " << d << ", lt\n";
          } else {
            const auto temporary =
                integerRegister(kHiddenScratch, instruction.result.type);
            os << "  cmp " << l << ", #0\n";
            os << "  cneg " << temporary << ", " << l << ", lt\n";
            os << "  and " << d << ", " << temporary << ", #" << mask
               << '\n';
            os << "  cneg " << d << ", " << d << ", lt\n";
          }
        } else {
          const auto temporary =
              integerRegister(kHiddenScratch, instruction.result.type);
          if (shift == 1) {
            os << "  add " << d << ", " << l << ", " << l << ", lsr #"
               << width - 1U << '\n';
          } else {
            os << "  asr " << temporary << ", " << l << ", #"
               << width - 1U << '\n';
            os << "  add " << d << ", " << l << ", " << temporary
               << ", lsr #" << width - shift << '\n';
          }
          os << "  asr " << d << ", " << d << ", #" << shift << '\n';
          if (rhsOp.intValue < 0) os << "  neg " << d << ", " << d << '\n';
        }
        storeResult(os, function, instruction.result, dst);
        return;
      }
    }
    if ((opcode == "shl" || opcode == "ashr" || opcode == "lshr") &&
        rhsOp.kind == MachineOperandKind::ImmediateInt && rhsOp.intValue >= 0 &&
        rhsOp.intValue < (isNarrow(instruction.result.type) ? 32 : 64)) {
      const char* mnemonic = opcode == "shl" ? "lsl" : opcode == "ashr" ? "asr" : "lsr";
      os << "  " << mnemonic << ' ' << d << ", " << l << ", #"
         << rhsOp.intValue << '\n';
      storeResult(os, function, instruction.result, dst);
      return;
    }
    if (opcode == "and" && rhsOp.kind == MachineOperandKind::ImmediateInt &&
        rhsOp.intValue == 1) {
      os << "  and " << d << ", " << l << ", #1\n";
      storeResult(os, function, instruction.result, dst);
      return;
    }
    const auto rhs = integerOperand(os, function, rhsOp, kImmScratch);
    const auto r = integerRegister(rhs, instruction.result.type);
    if (opcode == "add") os << "  add " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "sub") os << "  sub " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "mul") os << "  mul " << d << ", " << l << ", " << r << '\n';
    else if (isDivision) os << "  sdiv " << d << ", " << l << ", " << r << '\n';
    else if (isRemainder) {
      const std::string quotient = integerRegister(kHiddenScratch, instruction.result.type);
      os << "  sdiv " << quotient << ", " << l << ", " << r << '\n';
      os << "  msub " << d << ", " << quotient << ", " << r << ", " << l << '\n';
    } else if (opcode == "and") os << "  and " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "or") os << "  orr " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "xor") os << "  eor " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "shl") os << "  lsl " << d << ", " << l << ", " << r << '\n';
    else if (opcode == "ashr") os << "  asr " << d << ", " << l << ", " << r << '\n';
    else os << "  lsr " << d << ", " << l << ", " << r << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "fadd" || opcode == "fsub" || opcode == "fmul" || opcode == "fdiv") {
    const auto lhs = floatOperand(os, function, instruction.operands[0], "s16");
    const auto rhs = floatOperand(os, function, instruction.operands[1], "s17");
    const auto dst = resultRegister(instruction.result, "s18");
    os << "  " << opcode << ' ' << dst << ", " << lhs << ", " << rhs << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode.rfind("icmp.", 0) == 0) {
    const auto& lhsOp = instruction.operands[0];
    const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
    const auto& rhsOp = instruction.operands[1];
    const auto dst = resultRegister(instruction.result, kImmScratch);
    const auto lhsRegister = integerRegister(lhs, lhsOp.type);
    if (rhsOp.kind != MachineOperandKind::ImmediateInt ||
        !emitCompareImmediate(os, lhsRegister, rhsOp.intValue)) {
      const auto rhs = integerOperand(os, function, rhsOp, kImmScratch);
      os << "  cmp " << lhsRegister << ", " << integerRegister(rhs, lhsOp.type) << '\n';
    }
    os << "  cset " << integerRegister(dst, ValueType::int32Type()) << ", " << conditionFor(opcode) << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode.rfind("fcmp.", 0) == 0) {
    const auto lhs = floatOperand(os, function, instruction.operands[0], "s16");
    const auto rhs = floatOperand(os, function, instruction.operands[1], "s17");
    const auto dst = resultRegister(instruction.result, kImmScratch);
    os << "  fcmp " << lhs << ", " << rhs << '\n';
    std::string cond = "eq";
    if (opcode == "fcmp.olt") cond = "mi"; else if (opcode == "fcmp.ole") cond = "ls";
    else if (opcode == "fcmp.ogt") cond = "gt"; else if (opcode == "fcmp.oge") cond = "ge";
    if (opcode == "fcmp.one") {
      os << "  cset w18, mi\n  cset " << integerRegister(dst, ValueType::int32Type()) << ", gt\n";
      os << "  orr " << integerRegister(dst, ValueType::int32Type()) << ", " << integerRegister(dst, ValueType::int32Type()) << ", w18\n";
    } else os << "  cset " << integerRegister(dst, ValueType::int32Type()) << ", " << cond << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "cast" || opcode == "zext" || opcode == "sext" || opcode == "trunc") {
    const auto& sourceOp = instruction.operands[0]; const auto source = integerOperand(os, function, sourceOp, kAddrScratch);
    const auto dst = resultRegister(instruction.result, kImmScratch);
    if (instruction.result.type.isBool()) os << "  and " << integerRegister(dst, ValueType::int32Type()) << ", " << integerRegister(source, ValueType::int32Type()) << ", #1\n";
    else if (opcode == "zext" && isNarrow(sourceOp.type) && !isNarrow(instruction.result.type)) os << "  mov " << integerRegister(dst, ValueType::int32Type()) << ", " << integerRegister(source, ValueType::int32Type()) << '\n';
    else if ((opcode == "sext" || opcode == "cast") && sourceOp.type.isInt32() && !isNarrow(instruction.result.type)) os << "  sxtw " << dst << ", " << integerRegister(source, ValueType::int32Type()) << '\n';
    else os << "  mov " << integerRegister(dst, instruction.result.type) << ", " << integerRegister(source, instruction.result.type) << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "sitofp" || opcode == "uitofp") {
    const auto& sourceOp = instruction.operands[0]; const auto source = integerOperand(os, function, sourceOp, kAddrScratch);
    const auto dst = resultRegister(instruction.result, kFloatScratch);
    os << "  " << (opcode == "sitofp" ? "scvtf " : "ucvtf ") << dst << ", " << integerRegister(source, sourceOp.type) << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if (opcode == "fptosi") {
    const auto source = floatOperand(os, function, instruction.operands[0], kFloatScratch);
    const auto dst = resultRegister(instruction.result, kAddrScratch);
    os << "  fcvtzs " << integerRegister(dst, instruction.result.type) << ", " << source << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  os << "  // unsupported opcode: " << opcode << '\n';
}

void emitFunction(std::ostringstream& os, const MachineModule& module, const MachineFunction& function) {
  std::unordered_map<std::string, std::string> labels;
  for (const auto& block : function.blocks) labels.emplace(block.name, blockLabel(function, block));
  const std::string exit = epilogueLabel(function);
  os << ".text\n.globl " << function.name << "\n.type " << function.name << ", %function\n";
  os << ".p2align 2\n" << function.name << ":\n";
  for (const auto& instruction : function.prologueInstructions) emitInstruction(os, module, function, instruction, exit, labels);
  for (std::size_t index = 0; index < function.blocks.size(); ++index) {
    const auto& block = function.blocks[index]; os << labels.at(block.name) << ":\n";
    const std::string* fallthrough = index + 1 < function.blocks.size() ? &labels.at(function.blocks[index + 1].name) : nullptr;
    for (const auto& instruction : block.instructions) emitInstruction(os, module, function, instruction, exit, labels, fallthrough);
  }
  os << exit << ":\n";
  for (const auto& instruction : function.epilogueInstructions) emitInstruction(os, module, function, instruction, exit, labels);
  os << "  ret\n.size " << function.name << ", .-" << function.name << '\n';
}

}  // namespace

std::string AArch64AsmPrinter::print(const MachineModule& module) const {
  std::ostringstream os;
  os << ".arch armv8-a\n";
  for (const auto& global : module.globals) emitGlobal(os, module, global);
  for (const auto& function : module.functions) emitFunction(os, module, function);
  os << ".section .note.GNU-stack,\"\",%progbits\n";
  return os.str();
}

}  // namespace compiler::backend::asmcode
