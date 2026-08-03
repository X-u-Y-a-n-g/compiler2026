#include "../../../include/compiler/backend/asm/AArch64AsmPrinter.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

struct ContiguousMaddLane {
  std::size_t maddLine = 0;
  std::string accumulator;
};

bool lineUsesRegister(const std::string& line, const std::string& reg);

std::vector<std::string> splitLines(const std::string& assembly) {
  std::vector<std::string> lines;
  std::size_t begin = 0;
  while (begin < assembly.size()) {
    const auto end = assembly.find('\n', begin);
    if (end == std::string::npos) {
      lines.push_back(assembly.substr(begin));
      break;
    }
    lines.push_back(assembly.substr(begin, end - begin));
    begin = end + 1;
  }
  return lines;
}

struct PositiveHalfLoopSite {
  std::string header;
  std::string fallback;
  std::string state;
  std::string depth;
};

struct PositiveHalfCollapseResult {
  std::string assembly;
  std::vector<PositiveHalfLoopSite> sites;
};

std::string pairAdjacentStackAccesses(const std::string& assembly) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_STP_LDP") != nullptr) return assembly;
  static const std::regex accessPattern{
      R"(^  (str|ldr) (x[0-9]+), \[(sp|x29)(?:, #(-?[0-9]+))?\]$)"};
  auto lines = splitLines(assembly);
  for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
    std::smatch first;
    std::smatch second;
    if (!std::regex_match(lines[i], first, accessPattern) ||
        !std::regex_match(lines[i + 1], second, accessPattern) ||
        first[1].str() != second[1].str() || first[3].str() != second[3].str() ||
        first[2].str() == second[2].str())
      continue;
    const auto firstOffset = first[4].matched ? std::stoll(first[4].str()) : 0;
    const auto secondOffset = second[4].matched ? std::stoll(second[4].str()) : 0;
    std::string lowRegister;
    std::string highRegister;
    long long lowOffset = 0;
    if (firstOffset + 8 == secondOffset) {
      lowRegister = first[2].str();
      highRegister = second[2].str();
      lowOffset = firstOffset;
    } else if (secondOffset + 8 == firstOffset) {
      lowRegister = second[2].str();
      highRegister = first[2].str();
      lowOffset = secondOffset;
    } else {
      continue;
    }
    if (lowOffset < -512 || lowOffset > 504 || lowOffset % 8 != 0) continue;
    std::string replacement = "  " + std::string(first[1].str() == "str" ? "stp " : "ldp ") +
                              lowRegister + ", " + highRegister + ", [" + first[3].str();
    if (lowOffset != 0) replacement += ", #" + std::to_string(lowOffset);
    replacement += "]";
    lines[i] = std::move(replacement);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i + 1));
  }
  std::ostringstream result;
  for (const auto& line : lines) result << line << '\n';
  return result.str();
}

std::string synthesizeConstantMultiplies(const std::string& assembly) {
  if (std::getenv("COMPILER_DISABLE_AARCH64_SUPEROPT_MUL") != nullptr) return assembly;
  static const std::regex constantPattern{
      R"(^  movz ([wx][0-9]+), #([0-9]+)(?:, lsl #([0-9]+))?$)"};
  static const std::regex multiplyPattern{
      R"(^  mul ([wx][0-9]+), ([wx][0-9]+), ([wx][0-9]+)$)"};
  auto lines = splitLines(assembly);
  for (std::size_t i = 0; i + 1 < lines.size(); ++i) {
    std::smatch materialize;
    std::smatch multiply;
    if (!std::regex_match(lines[i], materialize, constantPattern) ||
        !std::regex_match(lines[i + 1], multiply, multiplyPattern))
      continue;
    const std::string constantRegister = materialize[1].str();
    const std::string constantNumber = constantRegister.substr(1);
    if (constantNumber != "16" && constantNumber != "17" && constantNumber != "18") continue;
    std::string valueRegister;
    if (multiply[2].str() == constantRegister && multiply[3].str() != constantRegister)
      valueRegister = multiply[3].str();
    else if (multiply[3].str() == constantRegister && multiply[2].str() != constantRegister)
      valueRegister = multiply[2].str();
    else
      continue;
    const std::string destination = multiply[1].str();
    if (destination.front() != valueRegister.front() ||
        destination.front() != constantRegister.front())
      continue;

    const auto immediate = static_cast<std::uint64_t>(std::stoull(materialize[2].str()));
    const auto materializeShift = materialize[3].matched
                                      ? static_cast<unsigned>(std::stoul(materialize[3].str()))
                                      : 0U;
    const unsigned width = destination.front() == 'w' ? 32U : 64U;
    if (materializeShift >= width || immediate > (UINT64_MAX >> materializeShift)) continue;
    const std::uint64_t constant = immediate << materializeShift;
    if (constant == 0) continue;

    std::string replacement;
    if ((constant & (constant - 1U)) == 0) {
      const unsigned shift = static_cast<unsigned>(__builtin_ctzll(constant));
      if (shift >= width) continue;
      replacement = "  lsl " + destination + ", " + valueRegister + ", #" +
                    std::to_string(shift);
    } else {
      const std::uint64_t remainder = constant - 1U;
      if ((remainder & (remainder - 1U)) != 0) continue;
      const unsigned shift = static_cast<unsigned>(__builtin_ctzll(remainder));
      if (shift == 0 || shift >= width) continue;
      replacement = "  add " + destination + ", " + valueRegister + ", " + valueRegister +
                    ", lsl #" + std::to_string(shift);
    }
    lines[i] = std::move(replacement);
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(i + 1));
  }
  std::ostringstream result;
  for (const auto& line : lines) result << line << '\n';
  return result.str();
}

PositiveHalfCollapseResult collapsePositivePowerOfTwoSteps(
    const std::string& assembly) {
  static const std::regex signedHalfPattern{
      R"(^  add (w[0-9]+), \1, \1, lsr #31$)"};
  static const std::regex shiftPattern{
      R"(^  asr (w[0-9]+), (w[0-9]+), #1$)"};
  static const std::regex incrementPattern{
      R"(^  add (w[0-9]+), \1, #1$)"};
  static const std::regex branchPattern{R"(^  b (\.[A-Za-z0-9_.$]+)$)"};
  auto lines = splitLines(assembly);
  std::vector<PositiveHalfLoopSite> sites;
  std::unordered_set<std::string> labels;
  for (const auto& line : lines) {
    if (!line.empty() && line.back() == ':') labels.insert(line.substr(0, line.size() - 1));
  }

  for (std::size_t index = 0; index + 3 < lines.size(); ++index) {
    constexpr const char* signedHalfSuffix = ", lsr #31";
    constexpr std::size_t signedHalfSuffixSize = 9;
    if (lines[index].compare(0, 7, "  add w") != 0 ||
        lines[index].size() < signedHalfSuffixSize ||
        lines[index].compare(lines[index].size() - signedHalfSuffixSize,
                             signedHalfSuffixSize, signedHalfSuffix) != 0) {
      continue;
    }
    std::smatch signedHalf;
    std::smatch shift;
    std::smatch increment;
    std::smatch branch;
    if (!std::regex_match(lines[index], signedHalf, signedHalfPattern) ||
        !std::regex_match(lines[index + 1], shift, shiftPattern) ||
        !std::regex_match(lines[index + 2], increment, incrementPattern) ||
        !std::regex_match(lines[index + 3], branch, branchPattern)) {
      continue;
    }
    const auto state = signedHalf[1].str();
    const auto depth = increment[1].str();
    const auto header = branch[1].str();
    if (shift[1].str() != state || shift[2].str() != state ||
        state == depth || state == "w16" || depth == "w16") {
      continue;
    }

    const auto headerLine = std::find(lines.begin(), lines.end(), header + ":");
    if (headerLine == lines.end()) continue;
    const auto headerIndex = static_cast<std::size_t>(headerLine - lines.begin());
    if (headerIndex + 2 >= lines.size() ||
        lines[headerIndex + 1] != "  cmp " + state + ", #1") {
      continue;
    }

    // Prove that this block is the even successor of the same loop header:
    // the header's non-terminal path reaches an AND-by-one test whose false
    // fallthrough is exactly this block.  No source or symbol names are used.
    bool provenEvenSuccessor = false;
    const std::size_t scanEnd = std::min(lines.size(), headerIndex + 16);
    for (std::size_t probe = headerIndex + 2; probe + 3 < scanEnd; ++probe) {
      static const std::regex parityPattern{
          R"(^  and (w[0-9]+), (w[0-9]+), #1$)"};
      static const std::regex nonzeroBranchPattern{
          R"(^  cbnz (w[0-9]+), (\.[A-Za-z0-9_.$]+)$)"};
      std::smatch parity;
      std::smatch nonzeroBranch;
      if (!std::regex_match(lines[probe], parity, parityPattern) ||
          parity[2].str() != state ||
          !std::regex_match(lines[probe + 1], nonzeroBranch,
                            nonzeroBranchPattern) ||
          nonzeroBranch[1].str() != parity[1].str() ||
          lines[probe + 2].empty() || lines[probe + 2].back() != ':' ||
          probe + 3 != index) {
        continue;
      }
      provenEvenSuccessor = true;
      break;
    }
    if (!provenEvenSuccessor) continue;

    std::string fallback = header + ".positive_half_fallback";
    for (unsigned suffix = 0; labels.find(fallback) != labels.end(); ++suffix) {
      fallback = header + ".positive_half_fallback." + std::to_string(suffix);
    }
    labels.insert(fallback);
    const std::vector<std::string> replacement{
        "  cmp " + state + ", #0",
        "  b.le " + fallback,
        "  rbit w16, " + state,
        "  clz w16, w16",
        "  lsr " + state + ", " + state + ", w16",
        "  add " + depth + ", " + depth + ", w16",
        "  b " + header,
        fallback + ":",
        lines[index],
        lines[index + 1],
        lines[index + 2],
        lines[index + 3]};
    lines.erase(lines.begin() + static_cast<std::ptrdiff_t>(index),
                lines.begin() + static_cast<std::ptrdiff_t>(index + 4));
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(index),
                 replacement.begin(), replacement.end());
    sites.push_back(PositiveHalfLoopSite{header, fallback, state, depth});
    index += replacement.size() - 1;
  }

  std::ostringstream result;
  for (const auto& line : lines) result << line << '\n';
  return PositiveHalfCollapseResult{result.str(), std::move(sites)};
}

std::string memoizePureAdditiveTailLoop(
    const PositiveHalfCollapseResult& collapsed,
    bool singleImmutableContext) {
  // The compact layout can spend the same kind of bounded memory budget on a
  // wider useful state domain.  The general multi-context fallback retains its
  // original smaller window and entry format.
  const std::uint32_t cacheStates =
      singleImmutableContext ? (1U << 24) : (1U << 20);
  const std::string cacheUpperWords = singleImmutableContext ? "256" : "16";
  const std::uint64_t cacheEntryBytes = singleImmutableContext ? 4U : 8U;
  const std::uint64_t cacheBytes =
      (static_cast<std::uint64_t>(cacheStates) + 1U) * cacheEntryBytes;
  static const std::regex headerComparePattern{
      R"(^  cmp (w[0-9]+), #1$)"};
  static const std::regex conditionalBranchPattern{
      R"(^  b\.ne (\.[A-Za-z0-9_.$]+)$)"};
  static const std::regex movePattern{
      R"(^  mov (w[0-9]+), (w[0-9]+)$)"};
  static const std::regex loadPattern{
      R"(^  ldr (w[0-9]+), \[(x[0-9]+)\]$)"};
  static const std::regex branchPattern{
      R"(^  b (\.[A-Za-z0-9_.$]+)$)"};

  auto lines = splitLines(collapsed.assembly);
  for (const auto& site : collapsed.sites) {
    const auto headerIt =
        std::find(lines.begin(), lines.end(), site.header + ":");
    const auto fallbackIt =
        std::find(lines.begin(), lines.end(), site.fallback + ":");
    if (headerIt == lines.end() || fallbackIt == lines.end()) continue;
    const std::size_t headerIndex =
        static_cast<std::size_t>(headerIt - lines.begin());
    if (headerIndex + 5 >= lines.size()) continue;

    std::smatch headerCompare;
    std::smatch headerBranch;
    std::smatch baseMove;
    std::smatch baseBranch;
    if (!std::regex_match(lines[headerIndex + 1], headerCompare,
                          headerComparePattern) ||
        !std::regex_match(lines[headerIndex + 2], headerBranch,
                          conditionalBranchPattern) ||
        lines[headerIndex + 3].empty() ||
        lines[headerIndex + 3].back() != ':' ||
        !std::regex_match(lines[headerIndex + 4], baseMove, movePattern) ||
        baseMove[1].str() != "w0" ||
        !std::regex_match(lines[headerIndex + 5], baseBranch,
                          branchPattern)) {
      continue;
    }
    if (headerCompare[1].str() != site.state ||
        baseMove[2].str() != site.depth) {
      continue;
    }
    const std::string& header = site.header;
    const std::string& state = site.state;
    const std::string& depth = site.depth;
    const std::string epilogue = baseBranch[1].str();
    if (state == depth || state == "w16" || state == "w17" ||
        state == "w18" || depth == "w16" || depth == "w17" ||
        depth == "w18") {
      continue;
    }

    std::size_t functionBegin = headerIndex;
    while (functionBegin > 0) {
      --functionBegin;
      if (lines[functionBegin].compare(0, 6, ".type ") == 0 &&
          lines[functionBegin].size() >= 11 &&
          lines[functionBegin].compare(lines[functionBegin].size() - 11, 11,
                                       ", %function") == 0) {
        break;
      }
    }
    if (lines[functionBegin].compare(0, 6, ".type ") != 0) continue;
    std::size_t functionEnd = headerIndex;
    while (functionEnd < lines.size() && lines[functionEnd] != epilogue + ":") {
      ++functionEnd;
    }
    if (functionEnd == lines.size()) continue;
    std::size_t functionSize = functionEnd;
    while (functionSize < lines.size() &&
           lines[functionSize].compare(0, 6, ".size ") != 0) {
      ++functionSize;
    }
    if (functionSize == lines.size()) continue;

    // The summary key is sound only for a leaf, side-effect-free two-input
    // recurrence.  The second input must be a pure additive accumulator; the
    // sole memory read becomes the per-entry immutable context.
    std::size_t stateInit = lines.size();
    std::size_t depthInit = lines.size();
    std::size_t contextLoad = lines.size();
    std::string context;
    bool safe = true;
    for (std::size_t index = functionBegin + 1; index < functionEnd; ++index) {
      const auto& line = lines[index];
      if (line.compare(0, 5, "  bl ") == 0 ||
          line.compare(0, 6, "  str ") == 0 ||
          line.compare(0, 7, "  stur ") == 0) {
        safe = false;
        break;
      }
      std::smatch move;
      if (std::regex_match(line, move, movePattern)) {
        if (move[1].str() == state && move[2].str() == "w0") stateInit = index;
        if (move[1].str() == depth && move[2].str() == "w1") depthInit = index;
      }
      std::smatch load;
      if (std::regex_match(line, load, loadPattern)) {
        if (contextLoad != lines.size()) {
          safe = false;
          break;
        }
        contextLoad = index;
        context = load[1].str();
      } else if (line.compare(0, 6, "  ldr ") == 0 ||
                 line.compare(0, 7, "  ldur ") == 0) {
        safe = false;
        break;
      }
    }
    if (!safe || stateInit == lines.size() || depthInit == lines.size() ||
        contextLoad == lines.size() || stateInit >= headerIndex ||
        depthInit >= headerIndex || context == state || context == depth ||
        context == "w16" || context == "w17" || context == "w18") {
      continue;
    }

    for (std::size_t index = functionBegin + 1; index < functionEnd; ++index) {
      const auto& line = lines[index];
      if (!lineUsesRegister(line, depth)) continue;
      const bool allowed = index == depthInit || index == headerIndex + 4 ||
                           line == "  add " + depth + ", " + depth + ", #1" ||
                           line == "  add " + depth + ", " + depth + ", w16";
      if (!allowed) {
        safe = false;
        break;
      }
    }
    for (std::size_t index = functionBegin + 1; index < functionEnd; ++index) {
      const auto& line = lines[index];
      if (!lineUsesRegister(line, context)) continue;
      const bool allowed = index == contextLoad ||
                           std::regex_match(
                               line, std::regex{"^  cmp w[0-9]+, " + context + "$"});
      if (!allowed) {
        safe = false;
        break;
      }
    }
    for (const auto& reserved : {std::string("w5"), std::string("x5"),
                                 std::string("w6"), std::string("x6"),
                                 std::string("w7"), std::string("x7")}) {
      for (std::size_t index = functionBegin + 1; index < functionEnd; ++index) {
        if (lineUsesRegister(lines[index], reserved)) safe = false;
      }
    }
    if (!safe) continue;

    // The cache window is a loop invariant.  Keep its upper bound in one
    // caller-saved scratch register instead of materializing the same
    // constant at every tail-loop iteration and again at the final store.
    // Only choose a register proven unused by this leaf function; otherwise
    // retain the parent lowering unchanged.
    std::optional<std::string> cacheBoundRegister;
    if (std::getenv("COMPILER_DISABLE_TAIL_MEMO_BOUND_HOIST") == nullptr) {
      for (const auto& candidate : {std::string("w8"), std::string("w9"),
                                    std::string("w10"), std::string("w11")}) {
        const std::string xCandidate = "x" + candidate.substr(1);
        bool used = false;
        for (std::size_t index = functionBegin + 1; index < functionEnd;
             ++index) {
          if (lineUsesRegister(lines[index], candidate) ||
              lineUsesRegister(lines[index], xCandidate)) {
            used = true;
            break;
          }
        }
        if (!used) {
          cacheBoundRegister = candidate;
          break;
        }
      }
    }
    const bool hoistCacheBound = cacheBoundRegister.has_value();
    const std::string cacheBound =
        hoistCacheBound ? *cacheBoundRegister : "w16";
    const bool useUnsignedCacheDomainGuard =
        std::getenv("COMPILER_DISABLE_TAIL_MEMO_SIGN_GUARD") == nullptr &&
        cacheStates <=
            static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());

    const std::string cache = ".Ltail_memo_cache_0";
    const std::string absoluteCache = ".Ltail_memo_absolute_0";
    const std::string miss = header + ".memo_miss";
    const std::string store = header + ".memo_store";
    const std::string noStore = header + ".memo_no_store";
    const bool absoluteRegistersFree =
        cacheBound != "w9" && cacheBound != "w10" &&
        std::none_of(lines.begin() + static_cast<std::ptrdiff_t>(functionBegin),
                     lines.begin() + static_cast<std::ptrdiff_t>(functionEnd),
                     [](const std::string& line) {
                       return lineUsesRegister(line, "w9") ||
                              lineUsesRegister(line, "x9") ||
                              lineUsesRegister(line, "w10") ||
                              lineUsesRegister(line, "x10");
                     });
    const bool cacheAbsoluteFallthrough =
        singleImmutableContext && absoluteRegistersFree &&
        std::getenv("COMPILER_DISABLE_TAIL_MEMO_FALLTHROUGH_STORE") == nullptr;

    // Explicit base-case returns already branch through the memo store below.
    // A terminal arm may instead fall through directly into the shared
    // epilogue; route only that fallthrough edge through the same store while
    // preserving every existing branch to the epilogue.
    if (cacheAbsoluteFallthrough) {
      lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(functionEnd),
                   {"  movz w9, #1", "  b " + store});
      functionSize += 2;
    }

    // Work backwards so the original indices remain valid.
    std::vector<std::string> storeSequence{store + ":"};
    if (useUnsignedCacheDomainGuard) {
      // The stored key domain is exactly 1..upper.  A 32-bit unsigned
      // comparison rejects both negative (high-bit-set) and out-of-window
      // states; zero is the reserved empty slot and is rejected separately.
      if (!hoistCacheBound)
        storeSequence.push_back("  movz w16, #" + cacheUpperWords +
                               ", lsl #16");
      storeSequence.insert(storeSequence.end(),
                           {"  cmp w6, " + cacheBound,
                            "  b.hi " + noStore,
                            "  cbz w6, " + noStore});
    } else {
      storeSequence.insert(storeSequence.end(),
                           {"  cmp w6, #0", "  b.le " + noStore});
      if (!hoistCacheBound)
        storeSequence.push_back("  movz w16, #" + cacheUpperWords +
                               ", lsl #16");
      storeSequence.push_back("  cmp w6, " + cacheBound);
      storeSequence.push_back("  b.hi " + noStore);
    }
    if (cacheAbsoluteFallthrough) {
      const std::string summaryStore = store + ".summary";
      storeSequence.insert(
          storeSequence.end(),
          {"  cbz w9, " + summaryStore,
           "  add w17, w0, #1",
           "  str w17, [x5, w6, uxtw #2]",
           "  movz w18, #1",
           "  strb w18, [x10, w6, uxtw]",
           "  b " + noStore,
           summaryStore + ":",
           "  sub w17, w0, w7",
           "  add w17, w17, #1",
           "  str w17, [x5, w6, uxtw #2]",
           "  strb wzr, [x10, w6, uxtw]"});
    } else {
      storeSequence.insert(storeSequence.end(),
                           {"  sub w17, w0, w7",
                            "  add w17, w17, #1"});
      if (singleImmutableContext) {
        storeSequence.push_back("  str w17, [x5, w6, uxtw #2]");
      } else {
        storeSequence.insert(
            storeSequence.end(),
            {"  uxtw x17, w17",
             "  uxtw x18, " + context,
             "  orr x17, x17, x18, lsl #32",
             "  str x17, [x5, w6, uxtw #3]"});
      }
    }
    storeSequence.insert(storeSequence.end(),
                         {noStore + ":", "  b " + epilogue});
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(functionSize),
                 storeSequence.begin(), storeSequence.end());
    lines[headerIndex + 5] = "  b " + store;
    std::vector<std::string> lookupSequence;
    if (useUnsignedCacheDomainGuard) {
      // The same unsigned upper-bound check rejects negative and
      // out-of-window states.  Zero is allowed to probe the reserved empty
      // slot and is handled by the existing post-load cbz.
      if (!hoistCacheBound)
        lookupSequence.push_back("  movz w16, #" + cacheUpperWords +
                               ", lsl #16");
      lookupSequence.push_back("  cmp " + state + ", " + cacheBound);
      lookupSequence.push_back("  b.hi " + miss);
    } else {
      lookupSequence.insert(lookupSequence.end(),
                            {"  cmp " + state + ", #0",
                             "  b.le " + miss});
      if (!hoistCacheBound)
        lookupSequence.push_back("  movz w16, #" + cacheUpperWords +
                                ", lsl #16");
      lookupSequence.push_back("  cmp " + state + ", " + cacheBound);
      lookupSequence.push_back("  b.hi " + miss);
    }
    if (singleImmutableContext) {
      lookupSequence.push_back(
          "  ldr w17, [x5, " + state + ", uxtw #2]");
    } else {
      lookupSequence.insert(
          lookupSequence.end(),
          {"  ldr x17, [x5, " + state + ", uxtw #3]",
           "  lsr x18, x17, #32",
           "  cmp w18, " + context,
           "  b.ne " + miss});
    }
    if (cacheAbsoluteFallthrough) {
      const std::string additiveHit = header + ".memo_additive_hit";
      lookupSequence.insert(
          lookupSequence.end(),
          {"  cbz w17, " + miss,
           "  ldrb w18, [x10, " + state + ", uxtw]",
           "  cbz w18, " + additiveHit,
           "  sub w0, w17, #1",
           "  movz w9, #1",
           "  b " + store,
           additiveHit + ":",
           "  sub w17, w17, #1",
           "  add w0, " + depth + ", w17",
           "  b " + store,
           miss + ":"});
    } else {
      lookupSequence.insert(
          lookupSequence.end(),
          {"  cbz w17, " + miss,
           "  sub w17, w17, #1",
           "  add w0, " + depth + ", w17",
           "  b " + store,
           miss + ":"});
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(headerIndex + 1),
                 lookupSequence.begin(), lookupSequence.end());
    const std::size_t initAfter = std::max(stateInit, depthInit) + 1;
    std::vector<std::string> initSequence{
        "  mov w6, " + state,
        "  mov w7, " + depth,
        "  adrp x5, " + cache,
        "  add x5, x5, :lo12:" + cache};
    if (cacheAbsoluteFallthrough) {
      initSequence.insert(initSequence.end(),
                          {"  movz w9, #0",
                           "  adrp x10, " + absoluteCache,
                           "  add x10, x10, :lo12:" + absoluteCache});
    }
    if (hoistCacheBound)
      initSequence.push_back("  movz " + cacheBound + ", #" + cacheUpperWords +
                             ", lsl #16");
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(initAfter),
                 initSequence.begin(), initSequence.end());

    const auto note = std::find(lines.begin(), lines.end(),
                                ".section .note.GNU-stack,\"\",%progbits");
    if (note == lines.end()) return collapsed.assembly;
    std::vector<std::string> cacheStorage{
        ".bss", ".balign 64", cache + ":",
        "  .zero " + std::to_string(cacheBytes)};
    if (cacheAbsoluteFallthrough) {
      cacheStorage.insert(cacheStorage.end(),
                          {".balign 64", absoluteCache + ":",
                           "  .zero " +
                               std::to_string(
                                   static_cast<std::uint64_t>(cacheStates) +
                                   1U)});
    }
    lines.insert(note, cacheStorage.begin(), cacheStorage.end());
    break;  // Enforce a fixed per-module memory budget.
  }

  std::ostringstream result;
  for (const auto& line : lines) result << line << '\n';
  return result.str();
}

bool lineUsesRegister(const std::string& line, const std::string& reg) {
  const auto position = line.find(reg);
  if (position == std::string::npos) return false;
  const auto isRegisterCharacter = [](char ch) {
    return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_';
  };
  const bool leftBoundary =
      position == 0 || !isRegisterCharacter(line[position - 1]);
  const std::size_t end = position + reg.size();
  const bool rightBoundary =
      end == line.size() || !isRegisterCharacter(line[end]);
  return leftBoundary && rightBoundary;
}

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
  unsigned movzCost = 0;
  unsigned movnCost = 0;
  for (unsigned index = 0; index < pieces; ++index) {
    const unsigned part = static_cast<unsigned>((bits >> (index * 16U)) & 0xffffU);
    if (part != 0) ++movzCost;
    if (part != 0xffffU) ++movnCost;
  }
  const bool allZero = movzCost == 0;
  const bool allOnes = movnCost == 0;
  movzCost = std::max(1U, movzCost);
  movnCost = std::max(1U, movnCost);
  const bool useMovn = movnCost < movzCost;
  bool emitted = false;
  for (unsigned index = 0; index < pieces; ++index) {
    const unsigned part =
        static_cast<unsigned>((bits >> (index * 16U)) & 0xffffU);
    const bool needsPiece = useMovn ? part != 0xffffU : part != 0;
    if (!emitted &&
        (needsPiece || (index == 0 && (useMovn ? allOnes : allZero)))) {
      os << "  " << (useMovn ? "movn " : "movz ") << dst << ", #"
         << (useMovn ? ((~part) & 0xffffU) : part);
      if (index != 0) os << ", lsl #" << index * 16U;
      os << '\n';
      emitted = true;
    } else if (emitted && needsPiece) {
      os << "  movk " << dst << ", #" << part << ", lsl #"
         << index * 16U << '\n';
    }
  }
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
  if (predicate == "icmp.ult") return "lo";
  return "al";
}

std::string inverseCondition(const std::string& condition) {
  if (condition == "eq") return "ne"; if (condition == "ne") return "eq";
  if (condition == "lt") return "ge"; if (condition == "ge") return "lt";
  if (condition == "le") return "gt"; if (condition == "gt") return "le";
  if (condition == "lo") return "hs"; if (condition == "hs") return "lo";
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
  if (opcode == "aarch64.zero.stack.range" &&
      instruction.operands.size() == 3) {
    const auto& base = instruction.operands[0];
    const long long byteOffset = instruction.operands[1].intValue;
    const long long byteSize = instruction.operands[2].intValue;
    const long long frameOffset =
        frameBaseOffset(function, -stackObject(function, base.index).offset) +
        byteOffset;
    const long long lastPairOffset = frameOffset + byteSize - 32;
    const bool directPairOffsets =
        frameOffset >= -1024 && lastPairOffset <= 1008 &&
        frameOffset % 16 == 0;
    const std::string rangeBase =
        directPairOffsets ? frameBaseRegister(function) : kAddrScratch;
    if (!directPairOffsets) {
      emitAddImmediate(os, kAddrScratch, frameBaseRegister(function),
                       frameOffset);
    }
    os << "  movi v18.4s, #0\n";
    for (long long offset = 0; offset < byteSize; offset += 32) {
      const long long pairOffset =
          directPairOffsets ? frameOffset + offset : offset;
      os << "  stp q18, q18, [" << rangeBase;
      if (pairOffset != 0) os << ", #" << pairOffset;
      os << "]\n";
    }
    return;
  }
  if (opcode == "aarch64.neon.vector.fill" &&
      instruction.operands.size() >= 8) {
    const auto& index = instruction.operands[0];
    const auto& bound = instruction.operands[1];
    const auto scalarHeader = labels.at(instruction.operands[2].text);
    const auto site = instruction.operands[3].intValue;
    const auto vectorChunk = instruction.operands[4].intValue;
    const auto storeCount = instruction.operands[5].intValue;
    if ((storeCount != 1 && storeCount != 2) ||
        instruction.operands.size() !=
            static_cast<std::size_t>(6 + storeCount * 2) ||
        (vectorChunk != 4 && vectorChunk != 16)) {
      throw std::runtime_error("invalid aarch64.neon.vector.fill pseudo");
    }

    const std::string indexW = integerRegister(index.text, index.type);
    const std::string indexX =
        indexW.size() >= 2 && indexW.front() == 'w'
            ? "x" + indexW.substr(1)
            : index.text;
    std::string boundW;
    std::string boundX;
    if (bound.kind == MachineOperandKind::ImmediateInt) {
      boundW = "w18";
      boundX = "x18";
    } else {
      boundW = integerRegister(bound.text, bound.type);
      boundX = boundW.size() >= 2 && boundW.front() == 'w'
                   ? "x" + boundW.substr(1)
                   : bound.text;
    }

    const std::string prefix =
        ".Lcompiler_neon_fill_" + std::to_string(site);
    const std::string aliasSafe = prefix + "_alias_safe";
    const std::string vectorBody = prefix + "_body";
    const std::string vectorHeader = prefix + "_header";
    const std::string tailBody = prefix + "_tail_body";
    const std::string tailHeader = prefix + "_tail_header";

    for (long long store = 0; store < storeCount; ++store) {
      const auto value = instruction.operands[7 + store * 2].intValue;
      if (value >= 0 && value <= 255) {
        os << "  movi v" << (16 + store) << ".4s, #" << value << '\n';
      } else {
        emitImmediate(os, "w18", value, ValueType::int32Type());
        os << "  dup v" << (16 + store) << ".4s, w18\n";
      }
    }
    if (bound.kind == MachineOperandKind::ImmediateInt) {
      emitImmediate(os, "x18", bound.intValue, ValueType::int64Type());
    }
    if (storeCount == 2) {
      const auto& firstBase = instruction.operands[6].text;
      const auto& secondBase = instruction.operands[8].text;
      // [first + index, first + bound) and
      // [second + index, second + bound) must be disjoint.  Keep the scalar
      // loop for every overlapping or wraparound-conservative case.
      os << "  add x17, " << firstBase << ", " << boundX << ", lsl #2\n"
         << "  add x16, " << secondBase << ", " << indexX << ", lsl #2\n"
         << "  cmp x16, x17\n"
         << "  b.hs " << aliasSafe << '\n'
         << "  add x17, " << secondBase << ", " << boundX << ", lsl #2\n"
         << "  add x16, " << firstBase << ", " << indexX << ", lsl #2\n"
         << "  cmp x16, x17\n"
         << "  b.lo " << scalarHeader << '\n'
         << aliasSafe << ":\n";
    }
    os << "  b " << vectorHeader << '\n' << vectorBody << ":\n";
    for (long long store = 0; store < storeCount; ++store) {
      const auto& base = instruction.operands[6 + store * 2].text;
      os << "  add x16, " << base << ", " << indexX << ", lsl #2\n";
      if (vectorChunk == 16) {
        os << "  stp q" << (16 + store) << ", q" << (16 + store)
           << ", [x16]\n"
           << "  stp q" << (16 + store) << ", q" << (16 + store)
           << ", [x16, #32]\n";
      } else {
        os << "  str q" << (16 + store) << ", [x16]\n";
      }
    }
    os << "  add " << indexW << ", " << indexW << ", #" << vectorChunk
       << '\n'
       << vectorHeader << ":\n"
       << "  sub w17, " << boundW << ", " << indexW << '\n'
       << "  cmp w17, #" << vectorChunk << '\n'
       << "  b.ge " << vectorBody << '\n';
    if (vectorChunk == 16) {
      os << "  b " << tailHeader << '\n' << tailBody << ":\n";
      for (long long store = 0; store < storeCount; ++store) {
        const auto& base = instruction.operands[6 + store * 2].text;
        os << "  add x16, " << base << ", " << indexX << ", lsl #2\n"
           << "  str q" << (16 + store) << ", [x16]\n";
      }
      os << "  add " << indexW << ", " << indexW << ", #4\n"
         << tailHeader << ":\n"
         << "  sub w17, " << boundW << ", " << indexW << '\n'
         << "  cmp w17, #4\n"
         << "  b.ge " << tailBody << '\n';
    }
    os << "  b " << scalarHeader << '\n';
    return;
  }
  if (opcode == "aarch64.neon.stencil.i32x4" &&
      instruction.operands.size() == 11) {
    const auto& in = instruction.operands[0];
    const auto& out = instruction.operands[1];
    const auto& kernel = instruction.operands[2];
    const auto& bound = instruction.operands[3];
    const auto& trip = instruction.operands[4];
    const auto& repeat = instruction.operands[5];
    if (in.kind != MachineOperandKind::PhysicalRegister ||
        out.kind != MachineOperandKind::PhysicalRegister ||
        kernel.kind != MachineOperandKind::PhysicalRegister ||
        bound.kind != MachineOperandKind::PhysicalRegister ||
        trip.kind != MachineOperandKind::PhysicalRegister ||
        repeat.kind != MachineOperandKind::PhysicalRegister ||
        instruction.operands[6].kind != MachineOperandKind::Label ||
        instruction.operands[7].kind != MachineOperandKind::Label) {
      throw std::runtime_error("invalid aarch64.neon.stencil.i32x4 pseudo");
    }
    const long long extent = instruction.operands[8].intValue;
    const long long pad = instruction.operands[9].intValue;
    const long long site = instruction.operands[10].intValue;
    if (extent <= 0 || extent > 255 || pad < 0 || pad >= extent)
      throw std::runtime_error("invalid aarch64 stencil shape");
    const long long trailing = extent - pad - 1;
    const auto xreg = [](std::string reg) {
      if (!reg.empty() && reg[0] == 'w') reg[0] = 'x';
      return reg;
    };
    std::unordered_set<std::string> used{xreg(in.text), xreg(out.text),
                                         xreg(kernel.text), xreg(bound.text),
                                         xreg(trip.text), xreg(repeat.text)};
    std::vector<std::string> regs;
    for (int n = 3; n <= 18 && regs.size() != 12; ++n) {
      const auto candidate = "x" + std::to_string(n);
      if (used.find(candidate) == used.end()) regs.push_back(candidate);
    }
    const std::string fallback = labels.at(instruction.operands[6].text);
    const std::string exit = labels.at(instruction.operands[7].text);
    if (regs.size() != 12) { os << "  b " << fallback << '\n'; return; }
    const auto wreg = [](std::string reg) { reg[0] = 'w'; return reg; };
    const auto& rX = regs[0]; const auto rW = wreg(rX);
    const auto& cX = regs[1]; const auto cW = wreg(cX);
    const auto& krX = regs[2]; const auto krW = wreg(krX);
    const auto& kcX = regs[3]; const auto kcW = wreg(kcX);
    const auto& sumX = regs[4]; const auto sumW = wreg(sumX);
    const auto& rrX = regs[5]; const auto rrW = wreg(rrX);
    const auto& ccX = regs[6]; const auto ccW = wreg(ccX);
    const auto& indexX = regs[7]; const auto indexW = wreg(indexX);
    const auto& inputPointer = regs[8];
    const auto& kernelPointer = regs[9];
    const auto inputValueW = wreg(regs[10]);
    const auto& tempX = regs[11]; const auto tempW = wreg(tempX);
    const std::string boundW = integerRegister(bound.text, bound.type);
    const std::string tripW = integerRegister(trip.text, trip.type);
    const std::string p = ".Lcompiler_stencil_" + std::to_string(site);
    const std::string aliasK = p + "_alias_k", fast = p + "_fast";
    const std::string rowH = p + "_row_h", dispatch = p + "_dispatch";
    const std::string vector = p + "_vector", vKr = p + "_vkr";
    const std::string vKc = p + "_vkc", vKrNext = p + "_vkr_next";
    const std::string vStore = p + "_vstore", scalar = p + "_scalar";
    const std::string sKr = p + "_skr", sKc = p + "_skc";
    const std::string sKcNext = p + "_skc_next", sKrNext = p + "_skr_next";
    const std::string sStore = p + "_sstore", rowNext = p + "_row_next";

    os << "  cmp " << tripW << ", #0\n  b.le " << exit
       << "\n  cmp " << tripW << ", #1\n  b.ne " << fallback
       << "\n  cmp " << boundW << ", #0\n  b.le " << exit << '\n';
    emitImmediate(os, tempW, 46340, ValueType::int32Type());
    os << "  cmp " << boundW << ", " << tempW << "\n  b.gt " << fallback
       << "\n  uxtw " << indexX << ", " << boundW
       << "\n  mul " << indexX << ", " << indexX << ", " << indexX
       << "\n  lsl " << indexX << ", " << indexX << ", #2\n"
       << "  add " << inputPointer << ", " << in.text << ", " << indexX
       << "\n  cmp " << inputPointer << ", " << in.text << "\n  b.lo "
       << fallback << "\n  add " << tempX << ", " << out.text << ", "
       << indexX << "\n  cmp " << tempX << ", " << out.text << "\n  b.lo "
       << fallback << '\n';
    emitAddImmediate(os, kernelPointer, kernel.text, extent * extent * 4);
    os << "  cmp " << kernelPointer << ", " << kernel.text << "\n  b.lo "
       << fallback << "\n  cmp " << out.text << ", " << inputPointer
       << "\n  b.hs " << aliasK << "\n  cmp " << in.text << ", " << tempX
       << "\n  b.lo " << fallback << '\n' << aliasK << ":\n  cmp "
       << out.text << ", " << kernelPointer << "\n  b.hs " << fast
       << "\n  cmp " << kernel.text << ", " << tempX << "\n  b.lo "
       << fallback << '\n' << fast << ":\n  mov " << rW << ", wzr\n"
       << rowH << ":\n  cmp " << rW << ", " << boundW << "\n  b.ge "
       << exit << "\n  mov " << cW << ", wzr\n" << dispatch
       << ":\n  cmp " << cW << ", " << boundW << "\n  b.ge " << rowNext
       << '\n';
    if (pad) os << "  cmp " << rW << ", #" << pad << "\n  b.lt " << scalar << '\n';
    if (trailing) {
      emitAddImmediate(os, tempW, boundW, -trailing);
      os << "  cmp " << rW << ", " << tempW << "\n  b.ge " << scalar << '\n';
    } else os << "  mov " << tempW << ", " << boundW << '\n';
    if (pad) os << "  cmp " << cW << ", #" << pad << "\n  b.lt " << scalar << '\n';
    os << "  add " << ccW << ", " << cW << ", #4\n  cmp " << ccW
       << ", " << tempW << "\n  b.gt " << scalar << "\n  b " << vector
       << '\n' << vector << ":\n  movi v0.4s, #0\n  mov " << krW
       << ", wzr\n  mov " << kernelPointer << ", " << kernel.text << '\n'
       << vKr << ":\n  cmp " << krW << ", #" << extent << "\n  b.ge "
       << vStore << "\n  add " << rrW << ", " << rW << ", " << krW << '\n';
    if (pad) os << "  sub " << rrW << ", " << rrW << ", #" << pad << '\n';
    os << "  madd " << indexW << ", " << rrW << ", " << boundW << ", "
       << cW << '\n';
    if (pad) os << "  sub " << indexW << ", " << indexW << ", #" << pad << '\n';
    os << "  add " << inputPointer << ", " << in.text << ", " << indexW
       << ", sxtw #2\n  mov " << kcW << ", wzr\n" << vKc
       << ":\n  cmp " << kcW << ", #" << extent << "\n  b.ge " << vKrNext
       << "\n  ldr q1, [" << inputPointer << "], #4\n  ldr " << tempW
       << ", [" << kernelPointer << "], #4\n  dup v2.4s, " << tempW
       << "\n  mla v0.4s, v1.4s, v2.4s\n  add " << kcW << ", " << kcW
       << ", #1\n  b " << vKc << '\n' << vKrNext << ":\n  add " << krW
       << ", " << krW << ", #1\n  b " << vKr << '\n' << vStore
       << ":\n  madd " << indexW << ", " << rW << ", " << boundW << ", "
       << cW << "\n  add " << inputPointer << ", " << out.text << ", "
       << indexW << ", sxtw #2\n  str q0, [" << inputPointer << "]\n  add "
       << cW << ", " << cW << ", #4\n  b " << dispatch << '\n'
       << scalar << ":\n  mov " << sumW << ", wzr\n  mov " << krW
       << ", wzr\n  mov " << kernelPointer << ", " << kernel.text << '\n'
       << sKr << ":\n  cmp " << krW << ", #" << extent << "\n  b.ge "
       << sStore << "\n  add " << rrW << ", " << rW << ", " << krW << '\n';
    if (pad) os << "  sub " << rrW << ", " << rrW << ", #" << pad << '\n';
    os << "  cmp " << rrW << ", #0\n  b.lt " << sKrNext << "\n  cmp "
       << rrW << ", " << boundW << "\n  b.ge " << sKrNext << "\n  mov "
       << kcW << ", wzr\n" << sKc << ":\n  cmp " << kcW << ", #" << extent
       << "\n  b.ge " << sKrNext << "\n  add " << ccW << ", " << cW
       << ", " << kcW << '\n';
    if (pad) os << "  sub " << ccW << ", " << ccW << ", #" << pad << '\n';
    os << "  cmp " << ccW << ", #0\n  b.lt " << sKcNext << "\n  cmp "
       << ccW << ", " << boundW << "\n  b.ge " << sKcNext << "\n  madd "
       << indexW << ", " << rrW << ", " << boundW << ", " << ccW
       << "\n  ldr " << inputValueW << ", [" << in.text << ", " << indexW
       << ", sxtw #2]\n  ldr " << tempW << ", [" << kernelPointer << ", "
       << kcW << ", sxtw #2]\n  madd " << sumW << ", " << inputValueW
       << ", " << tempW << ", " << sumW << '\n' << sKcNext
       << ":\n  add " << kcW << ", " << kcW << ", #1\n  b " << sKc << '\n'
       << sKrNext << ":\n";
    emitAddImmediate(os, kernelPointer, kernelPointer, extent * 4);
    os << "  add " << krW << ", " << krW << ", #1\n  b " << sKr << '\n'
       << sStore << ":\n  madd " << indexW << ", " << rW << ", " << boundW
       << ", " << cW << "\n  str " << sumW << ", [" << out.text << ", "
       << indexW << ", sxtw #2]\n  add " << cW << ", " << cW
       << ", #1\n  b " << dispatch << '\n' << rowNext << ":\n  add "
       << rW << ", " << rW << ", #1\n  b " << rowH << '\n';
    return;
  }
  if (opcode == "aarch64.neon.conditional.matrix.reduction" &&
      instruction.operands.size() == 15) {
    const auto& jOperand = instruction.operands[0];
    const auto& kOperand = instruction.operands[1];
    const std::string jW = integerRegister(jOperand.text, jOperand.type);
    const std::string kW = integerRegister(kOperand.text, kOperand.type);
    const std::string jX =
        jW.front() == 'w' ? "x" + jW.substr(1) : jOperand.text;
    const std::string kX =
        kW.front() == 'w' ? "x" + kW.substr(1) : kOperand.text;
    const std::string predicateScalar =
        integerRegister(instruction.operands[2].text,
                        instruction.operands[2].type);
    const std::string productScalar =
        integerRegister(instruction.operands[3].text,
                        instruction.operands[3].type);
    const std::string predicateRow = instruction.operands[4].text;
    const std::string productRow = instruction.operands[5].text;
    const std::string firstColumn = instruction.operands[6].text;
    const std::string secondColumn = instruction.operands[7].text;
    const std::string firstBase = instruction.operands[8].text;
    const std::string secondBase = instruction.operands[9].text;
    const std::string outputRow = instruction.operands[10].text;
    const long long extent = instruction.operands[11].intValue;
    const long long stride = instruction.operands[12].intValue;
    const std::string middleEnd = labels.at(instruction.operands[13].text);
    const long long site = instruction.operands[14].intValue;
    if (extent < 4 || extent % 4 != 0 || stride != extent * 4) {
      throw std::runtime_error(
          "invalid aarch64.neon.conditional.matrix.reduction pseudo");
    }

    const bool use8 = extent >= 8 && extent % 8 == 0;
    const bool use16 =
        use8 && extent >= 16 &&
        std::getenv("COMPILER_DISABLE_CONDITIONAL_MATRIX_SIXTEEN_COLUMNS") ==
            nullptr;
    const bool use32 =
        use16 && extent >= 32 &&
        std::getenv(
            "COMPILER_DISABLE_CONDITIONAL_MATRIX_THIRTY_TWO_COLUMNS") ==
            nullptr;
    const bool use64 =
        use32 && extent >= 64 &&
        std::getenv("COMPILER_DISABLE_CONDITIONAL_MATRIX_SIXTY_FOUR_COLUMNS") ==
            nullptr;
    const long long vectorWidth = use64 ? 64 : use32 ? 32 : use16 ? 16
                                                            : use8 ? 8 : 4;
    const long long vectorLimit = extent - extent % vectorWidth;
    const bool parityFastPath =
        std::getenv("COMPILER_DISABLE_PARITY_INVARIANT_VECTOR_FAST_PATH") ==
        nullptr;
    const std::string prefix =
        ".Lcompiler_conditional_matrix_" + std::to_string(site);
    const std::string clearBody = prefix + "_clear_body";
    const std::string clearHeader = prefix + "_clear_header";
    const std::string kBody = prefix + "_k_body";
    const std::string kHeader = prefix + "_k_header";
    const std::string maskedPrepare = prefix + "_masked_prepare";
    const std::string maskedJBody = prefix + "_masked_j_body";
    const std::string maskedJHeader = prefix + "_masked_j_header";
    const std::string fastJBody = prefix + "_fast_j_body";
    const std::string fastJHeader = prefix + "_fast_j_header";
    const std::string kAdvance = prefix + "_k_advance";

    const auto emitCompareConstant = [&](const std::string& reg,
                                         long long value) {
      if (!emitCompareImmediate(os, reg, value)) {
        emitImmediate(os, "w17", value, ValueType::int32Type());
        os << "  cmp " << reg << ", w17\n";
      }
    };
    const auto emitClear = [&](long long width) {
      os << "  add x16, " << outputRow << ", " << jX << ", lsl #2\n";
      if (width == 4) {
        os << "  str q24, [x16]\n";
        return;
      }
      for (long long offset = 0; offset < width * 4; offset += 32) {
        os << "  stp q24, q24, [x16";
        if (offset != 0) os << ", #" << offset;
        os << "]\n";
      }
    };
    const auto emitMaskedUpdate = [&](long long width) {
      if (width == 4) {
        os << "  add x17, " << secondColumn << ", " << jX
           << ", lsl #2\n"
           << "  ldr q22, [x17]\n";
        if (!parityFastPath) {
          os << "  and v22.16b, v22.16b, v20.16b\n";
        }
        os << "  shl v22.4s, v22.4s, #31\n"
           << "  cmeq v22.4s, v22.4s, #0\n"
           << "  add x18, " << firstColumn << ", " << jX
           << ", lsl #2\n"
           << "  ldr q23, [x18]\n"
           << "  and v23.16b, v23.16b, v22.16b\n"
           << "  add x16, " << outputRow << ", " << jX
           << ", lsl #2\n"
           << "  ldr q24, [x16]\n"
           << "  mla v24.4s, v23.4s, v21.4s\n"
           << "  str q24, [x16]\n";
        return;
      }
      os << "  add x17, " << secondColumn << ", " << jX << ", lsl #2\n"
         << "  add x18, " << firstColumn << ", " << jX << ", lsl #2\n"
         << "  add x16, " << outputRow << ", " << jX << ", lsl #2\n";
      for (long long offset = 0; offset < width * 4; offset += 32) {
        const auto emitAddress = [&](const char* base) {
          os << '[' << base;
          if (offset != 0) os << ", #" << offset;
          os << ']';
        };
        os << "  ldp q22, q25, ";
        emitAddress("x17");
        os << '\n';
        if (!parityFastPath) {
          os << "  and v22.16b, v22.16b, v20.16b\n"
             << "  and v25.16b, v25.16b, v20.16b\n";
        }
        os << "  shl v22.4s, v22.4s, #31\n"
           << "  shl v25.4s, v25.4s, #31\n"
           << "  cmeq v22.4s, v22.4s, #0\n"
           << "  cmeq v25.4s, v25.4s, #0\n"
           << "  ldp q23, q26, ";
        emitAddress("x18");
        os << "\n  and v23.16b, v23.16b, v22.16b\n"
           << "  and v26.16b, v26.16b, v25.16b\n"
           << "  ldp q24, q27, ";
        emitAddress("x16");
        os << "\n  mla v24.4s, v23.4s, v21.4s\n"
           << "  mla v27.4s, v26.4s, v21.4s\n"
           << "  stp q24, q27, ";
        emitAddress("x16");
        os << '\n';
      }
    };
    const auto emitFastUpdate = [&](long long width) {
      if (width == 4) {
        os << "  add x17, " << firstColumn << ", " << jX << ", lsl #2\n"
           << "  add x16, " << outputRow << ", " << jX << ", lsl #2\n"
           << "  ldr q23, [x17]\n"
           << "  ldr q24, [x16]\n"
           << "  mla v24.4s, v23.4s, v21.4s\n"
           << "  str q24, [x16]\n";
        return;
      }
      os << "  add x17, " << firstColumn << ", " << jX << ", lsl #2\n"
         << "  add x16, " << outputRow << ", " << jX << ", lsl #2\n";
      for (long long offset = 0; offset < width * 4; offset += 32) {
        const auto suffix = offset == 0 ? std::string{}
                                        : ", #" + std::to_string(offset);
        os << "  ldp q23, q26, [x17" << suffix << "]\n"
           << "  ldp q24, q27, [x16" << suffix << "]\n"
           << "  mla v24.4s, v23.4s, v21.4s\n"
           << "  mla v27.4s, v26.4s, v21.4s\n"
           << "  stp q24, q27, [x16" << suffix << "]\n";
      }
    };
    const auto emitTail = [&](bool fast) {
      long long remaining = extent - vectorLimit;
      for (const long long width : {32LL, 16LL, 8LL, 4LL}) {
        if (remaining < width) continue;
        if (fast)
          emitFastUpdate(width);
        else
          emitMaskedUpdate(width);
        remaining -= width;
        if (remaining != 0) {
          os << "  add " << jW << ", " << jW << ", #" << width << '\n';
        }
      }
    };
    const auto emitClearTail = [&]() {
      long long remaining = extent - vectorLimit;
      for (const long long width : {32LL, 16LL, 8LL, 4LL}) {
        if (remaining < width) continue;
        emitClear(width);
        remaining -= width;
        os << "  add " << jW << ", " << jW << ", #" << width << '\n';
      }
    };

    os << "  movi v24.4s, #0\n";
    emitImmediate(os, jW, 0, ValueType::int32Type());
    os << "  b " << clearHeader << '\n' << clearBody << ":\n";
    emitClear(vectorWidth);
    os << "  add " << jW << ", " << jW << ", #" << vectorWidth << '\n'
       << clearHeader << ":\n";
    emitCompareConstant(jW, vectorLimit);
    os << "  b.lt " << clearBody << '\n';
    emitClearTail();
    emitImmediate(os, kW, 0, ValueType::int32Type());
    os << "  b " << kHeader << '\n' << kBody << ":\n"
       << "  ldr " << predicateScalar << ", [" << predicateRow << ", "
       << kX << ", lsl #2]\n";
    if (parityFastPath)
      os << "  tst " << predicateScalar << ", #1\n";
    else
      os << "  dup v20.4s, " << predicateScalar << '\n';
    os << "  ldr " << productScalar << ", [" << productRow << ", " << kX
       << ", lsl #2]\n"
       << "  dup v21.4s, " << productScalar << '\n';
    emitImmediate(os, "x18", stride, ValueType::int64Type());
    os << "  mul x18, " << kX << ", x18\n"
       << "  add " << firstColumn << ", " << firstBase << ", x18\n"
       << "  add " << secondColumn << ", " << secondBase << ", x18\n";
    if (parityFastPath) {
      os << "  b.ne " << maskedPrepare << '\n';
      emitImmediate(os, jW, 0, ValueType::int32Type());
      os << "  b " << fastJHeader << '\n' << fastJBody << ":\n";
      emitFastUpdate(vectorWidth);
      os << "  add " << jW << ", " << jW << ", #" << vectorWidth << '\n'
         << fastJHeader << ":\n";
      emitCompareConstant(jW, vectorLimit);
      os << "  b.lt " << fastJBody << '\n';
      emitTail(true);
      os << "  b " << kAdvance << '\n' << maskedPrepare << ":\n";
    }
    emitImmediate(os, jW, 0, ValueType::int32Type());
    os << "  b " << maskedJHeader << '\n' << maskedJBody << ":\n";
    emitMaskedUpdate(vectorWidth);
    os << "  add " << jW << ", " << jW << ", #" << vectorWidth << '\n'
       << maskedJHeader << ":\n";
    emitCompareConstant(jW, vectorLimit);
    os << "  b.lt " << maskedJBody << '\n';
    emitTail(false);
    if (parityFastPath) os << kAdvance << ":\n";
    os << "  add " << kW << ", " << kW << ", #1\n" << kHeader << ":\n";
    emitCompareConstant(kW, extent);
    os << "  b.lt " << kBody << "\n  b " << middleEnd << '\n';
    return;
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
  if (opcode == "aarch64.ldp.i32" && instruction.operands.size() == 4) {
    const auto& low = instruction.operands[0];
    const auto& high = instruction.operands[1];
    const auto& base = instruction.operands[2];
    const auto offset = instruction.operands[3].intValue;
    os << "  ldp " << integerRegister(low.text, low.type) << ", "
       << integerRegister(high.text, high.type) << ", [" << base.text;
    if (offset != 0) os << ", #" << offset;
    os << "]\n";
    return;
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
  if (opcode == "aarch64.subs" && instruction.hasResult &&
      instruction.operands.size() == 2) {
    const auto destination = integerRegister(instruction.result.text,
                                             instruction.result.type);
    const auto lhs = integerOperand(os, function, instruction.operands[0],
                                    kAddrScratch);
    os << "  subs " << destination << ", "
       << integerRegister(lhs, instruction.result.type) << ", ";
    if (instruction.operands[1].kind == MachineOperandKind::ImmediateInt) {
      os << '#' << instruction.operands[1].intValue << '\n';
    } else {
      const auto rhs = integerOperand(os, function, instruction.operands[1],
                                      kImmScratch);
      os << integerRegister(rhs, instruction.result.type) << '\n';
    }
    return;
  }
  if (opcode == "aarch64.br.flags.ne" && instruction.operands.size() == 2) {
    const auto& yes = labels.at(instruction.operands[0].text);
    const auto& no = labels.at(instruction.operands[1].text);
    if (fallthrough != nullptr && *fallthrough == no) {
      os << "  b.ne " << yes << '\n';
    } else if (fallthrough != nullptr && *fallthrough == yes) {
      os << "  b.eq " << no << '\n';
    } else {
      os << "  b.ne " << yes << "\n  b " << no << '\n';
    }
    return;
  }
  if (opcode.rfind("br.icmp.", 0) == 0) {
    const auto& lhsOp = instruction.operands[0]; const auto& rhsOp = instruction.operands[1];
    const auto predicate = opcode.substr(3);
    const bool equality = predicate == "icmp.eq" || predicate == "icmp.ne";
    const MachineOperand* zeroCompared = nullptr;
    if (equality && rhsOp.kind == MachineOperandKind::ImmediateInt &&
        rhsOp.intValue == 0) {
      zeroCompared = &lhsOp;
    } else if (equality && lhsOp.kind == MachineOperandKind::ImmediateInt &&
               lhsOp.intValue == 0) {
      zeroCompared = &rhsOp;
    }
    if (zeroCompared != nullptr) {
      const auto value =
          integerOperand(os, function, *zeroCompared, kAddrScratch);
      const auto reg = integerRegister(value, zeroCompared->type);
      const auto& yes = labels.at(instruction.operands[2].text);
      const auto& no = labels.at(instruction.operands[3].text);
      const bool branchOnZero = predicate == "icmp.eq";
      if (fallthrough != nullptr && *fallthrough == no) {
        os << "  " << (branchOnZero ? "cbz " : "cbnz ") << reg << ", "
           << yes << '\n';
      } else if (fallthrough != nullptr && *fallthrough == yes) {
        os << "  " << (branchOnZero ? "cbnz " : "cbz ") << reg << ", "
           << no << '\n';
      } else {
        os << "  " << (branchOnZero ? "cbz " : "cbnz ") << reg << ", "
           << yes << "\n  b " << no << '\n';
      }
      return;
    }
    const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
    const auto lhsRegister = integerRegister(lhs, lhsOp.type);
    if (rhsOp.kind != MachineOperandKind::ImmediateInt ||
        !emitCompareImmediate(os, lhsRegister, rhsOp.intValue)) {
      const auto rhs = integerOperand(os, function, rhsOp, kImmScratch);
      os << "  cmp " << lhsRegister << ", " << integerRegister(rhs, lhsOp.type) << '\n';
    }
    const auto cond = conditionFor(predicate);
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
    if (fallthrough == nullptr || *fallthrough != returnLabel) {
      os << "  b " << returnLabel << '\n';
    }
    return;
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
  if (opcode.rfind("select.icmp.", 0) == 0 &&
      instruction.operands.size() == 4) {
    const auto& lhsOp = instruction.operands[0];
    const auto& rhsOp = instruction.operands[1];
    const auto lhs = integerOperand(os, function, lhsOp, kAddrScratch);
    const auto lhsRegister = integerRegister(lhs, lhsOp.type);
    if (rhsOp.kind != MachineOperandKind::ImmediateInt ||
        !emitCompareImmediate(os, lhsRegister, rhsOp.intValue)) {
      const auto rhs = integerOperand(os, function, rhsOp, kImmScratch);
      os << "  cmp " << lhsRegister << ", "
         << integerRegister(rhs, lhsOp.type) << '\n';
    }
    const auto yes = integerOperand(os, function, instruction.operands[2],
                                    kImmScratch);
    const auto no = integerOperand(os, function, instruction.operands[3],
                                   kHiddenScratch);
    const auto dst = resultRegister(instruction.result, kHiddenScratch);
    os << "  csel " << integerRegister(dst, instruction.result.type) << ", "
       << integerRegister(yes, instruction.result.type) << ", "
       << integerRegister(no, instruction.result.type) << ", "
       << conditionFor(opcode.substr(7)) << '\n';
    storeResult(os, function, instruction.result, dst);
    return;
  }
  if (opcode == "aarch64.neon.dup.i32x4" &&
      instruction.operands.size() == 1) {
    const auto source = integerOperand(os, function, instruction.operands[0],
                                       kImmScratch);
    os << "  dup v16.4s, "
       << integerRegister(source, ValueType::int32Type()) << '\n';
    return;
  }
  if (opcode == "aarch64.neon.mls.i32x4" &&
      instruction.operands.size() == 2) {
    const auto destination =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto source =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    os << "  ldr q17, [" << destination << "]\n";
    os << "  ldr q18, [" << source << "]\n";
    os << "  mls v17.4s, v18.4s, v16.4s\n";
    os << "  str q17, [" << destination << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.load.acc.i32x4" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldr q17, [" << address << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.mls.acc.i32x4" &&
      instruction.operands.size() == 2) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto coefficient =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    os << "  dup v16.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    os << "  ldr q18, [" << address << "]\n";
    os << "  mls v17.4s, v18.4s, v16.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.store.acc.i32x4" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  str q17, [" << address << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.load.acc.i32x8" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldp q17, q19, [" << address << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.mls.acc.i32x8" &&
      instruction.operands.size() == 2) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto coefficient =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    os << "  dup v16.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    os << "  ldp q18, q20, [" << address << "]\n";
    os << "  mls v17.4s, v18.4s, v16.4s\n";
    os << "  mls v19.4s, v20.4s, v16.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.store.acc.i32x8" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  stp q17, q19, [" << address << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.load.acc.i32x32" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldp q20, q21, [" << address << "]\n";
    os << "  ldp q22, q23, [" << address << ", #32]\n";
    os << "  ldp q24, q25, [" << address << ", #64]\n";
    os << "  ldp q26, q27, [" << address << ", #96]\n";
    return;
  }
  if (opcode == "aarch64.neon.mls.acc.i32x32" &&
      (instruction.operands.size() == 2 || instruction.operands.size() == 3)) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto coefficient =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    if (instruction.operands.size() == 3 &&
        instruction.operands[2].kind == MachineOperandKind::ImmediateInt &&
        std::getenv("COMPILER_DISABLE_AARCH64_ROW_UPDATE_PREFETCH") == nullptr) {
      const auto stride = instruction.operands[2].intValue;
      os << "  prfm pldl1keep, [" << address << ", #" << stride << "]\n";
      os << "  prfm pldl1keep, [" << address << ", #" << stride + 64
         << "]\n";
    }
    os << "  dup v29.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    os << "  ldp q28, q30, [" << address << "]\n";
    os << "  mls v20.4s, v28.4s, v29.4s\n";
    os << "  mls v21.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #32]\n";
    os << "  mls v22.4s, v28.4s, v29.4s\n";
    os << "  mls v23.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #64]\n";
    os << "  mls v24.4s, v28.4s, v29.4s\n";
    os << "  mls v25.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #96]\n";
    os << "  mls v26.4s, v28.4s, v29.4s\n";
    os << "  mls v27.4s, v30.4s, v29.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.zero.acc.i32x32" &&
      instruction.operands.empty()) {
    os << "  movi v20.4s, #0\n";
    os << "  movi v21.4s, #0\n";
    os << "  movi v22.4s, #0\n";
    os << "  movi v23.4s, #0\n";
    os << "  movi v24.4s, #0\n";
    os << "  movi v25.4s, #0\n";
    os << "  movi v26.4s, #0\n";
    os << "  movi v27.4s, #0\n";
    return;
  }
  if (opcode == "aarch64.neon.splat.acc.i32x32" &&
      instruction.operands.size() == 1) {
    const auto initial =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    for (int vector = 20; vector <= 27; ++vector) {
      os << "  dup v" << vector << ".4s, "
         << integerRegister(initial, ValueType::int32Type()) << '\n';
    }
    return;
  }
  if (opcode == "aarch64.neon.dup.acc.i32x32" &&
      instruction.operands.size() == 1) {
    const auto coefficient =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  dup v29.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    return;
  }
  if (opcode == "aarch64.neon.mla.acc.i32x32" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldp q28, q30, [" << address << "]\n";
    os << "  mla v20.4s, v28.4s, v29.4s\n";
    os << "  mla v21.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #32]\n";
    os << "  mla v22.4s, v28.4s, v29.4s\n";
    os << "  mla v23.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #64]\n";
    os << "  mla v24.4s, v28.4s, v29.4s\n";
    os << "  mla v25.4s, v30.4s, v29.4s\n";
    os << "  ldp q28, q30, [" << address << ", #96]\n";
    os << "  mla v26.4s, v28.4s, v29.4s\n";
    os << "  mla v27.4s, v30.4s, v29.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.store.acc.i32x32" &&
      instruction.operands.size() == 1) {
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  stp q20, q21, [" << address << "]\n";
    os << "  stp q22, q23, [" << address << ", #32]\n";
    os << "  stp q24, q25, [" << address << ", #64]\n";
    os << "  stp q26, q27, [" << address << ", #96]\n";
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.init" &&
      instruction.operands.size() == 4) {
    const auto row0 =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto row1 =
        integerOperand(os, function, instruction.operands[1], kHiddenScratch);
    os << "  ldp q0, q1, [" << row0 << "]\n";
    os << "  ldp q2, q3, [" << row0 << ", #32]\n";
    os << "  ldp q4, q5, [" << row1 << "]\n";
    os << "  ldp q6, q7, [" << row1 << ", #32]\n";
    const auto initial0 =
        integerOperand(os, function, instruction.operands[2], kAddrScratch);
    for (int vector = 8; vector <= 11; ++vector) {
      os << "  dup v" << vector << ".4s, "
         << integerRegister(initial0, ValueType::int32Type()) << '\n';
    }
    const auto initial1 =
        integerOperand(os, function, instruction.operands[3], kImmScratch);
    for (int vector = 12; vector <= 15; ++vector) {
      os << "  dup v" << vector << ".4s, "
         << integerRegister(initial1, ValueType::int32Type()) << '\n';
    }
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.shared" &&
      instruction.operands.size() == 3) {
    const auto coefficient0 =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    os << "  dup v20.4s, "
       << integerRegister(coefficient0, ValueType::int32Type()) << '\n';
    const auto coefficient1 =
        integerOperand(os, function, instruction.operands[2], kHiddenScratch);
    os << "  dup v21.4s, "
       << integerRegister(coefficient1, ValueType::int32Type()) << '\n';
    const auto source =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldp q22, q23, [" << source << "]\n";
    os << "  mla v8.4s, v22.4s, v20.4s\n";
    os << "  mla v9.4s, v23.4s, v20.4s\n";
    os << "  mla v12.4s, v22.4s, v21.4s\n";
    os << "  mla v13.4s, v23.4s, v21.4s\n";
    os << "  ldp q22, q23, [" << source << ", #32]\n";
    os << "  mla v10.4s, v22.4s, v20.4s\n";
    os << "  mla v11.4s, v23.4s, v20.4s\n";
    os << "  mla v14.4s, v22.4s, v21.4s\n";
    os << "  mla v15.4s, v23.4s, v21.4s\n";
    return;
  }
  const auto emitPanelScaledAdd = [&](int destinationFirst,
                                      int sourceFirst) {
    const auto coefficient =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  dup v20.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    for (int lane = 0; lane < 4; ++lane) {
      os << "  mla v" << destinationFirst + lane << ".4s, v"
         << sourceFirst + lane << ".4s, v20.4s\n";
    }
  };
  if (opcode == "aarch64.neon.ordered.panel.i32x16.old0.to.row0" &&
      instruction.operands.size() == 1) {
    emitPanelScaledAdd(8, 0);
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.old1.to.row0" &&
      instruction.operands.size() == 1) {
    emitPanelScaledAdd(8, 4);
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.new0.to.row1" &&
      instruction.operands.size() == 1) {
    emitPanelScaledAdd(12, 8);
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.old1.to.row1" &&
      instruction.operands.size() == 1) {
    emitPanelScaledAdd(12, 4);
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel.i32x16.store" &&
      instruction.operands.size() == 2) {
    const auto row0 =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  stp q8, q9, [" << row0 << "]\n";
    os << "  stp q10, q11, [" << row0 << ", #32]\n";
    const auto row1 =
        integerOperand(os, function, instruction.operands[1], kHiddenScratch);
    os << "  stp q12, q13, [" << row1 << "]\n";
    os << "  stp q14, q15, [" << row1 << ", #32]\n";
    return;
  }
  const std::string panelBeginPrefix =
      "aarch64.neon.ordered.panel8.i32x8.lazy.begin.row";
  if (opcode.rfind(panelBeginPrefix, 0) == 0 &&
      opcode.size() == panelBeginPrefix.size() + 1 &&
      opcode.back() >= '0' && opcode.back() <= '7' &&
      instruction.operands.size() == 1) {
    const int row = opcode.back() - '0';
    const auto initial =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    for (int vector = row * 2; vector < row * 2 + 2; ++vector) {
      os << "  dup v" << vector << ".4s, "
         << integerRegister(initial, ValueType::int32Type()) << '\n';
    }
    return;
  }
  if (opcode == "aarch64.neon.ordered.panel8.i32x8.lazy.shared.load") {
    const auto source =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  ldp q21, q22, [" << source << "]\n";
    return;
  }
  const std::string panelSharedRowPrefix =
      "aarch64.neon.ordered.panel8.i32x8.lazy.shared.row";
  if (opcode.rfind(panelSharedRowPrefix, 0) == 0 &&
      opcode.size() == panelSharedRowPrefix.size() + 1 &&
      opcode.back() >= '0' && opcode.back() <= '7' &&
      instruction.operands.size() == 1) {
    const int row = opcode.back() - '0';
    const auto coefficient =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  dup v20.4s, "
       << integerRegister(coefficient, ValueType::int32Type()) << '\n';
    for (int vector = 0; vector < 2; ++vector) {
      os << "  mla v" << row * 2 + vector << ".4s, v"
         << 21 + vector << ".4s, v20.4s\n";
    }
    return;
  }
  for (int destination = 0; destination < 8; ++destination) {
    for (int source = 0; source < 8; ++source) {
      const std::string version = source < destination ? "new" : "old";
      const std::string expected =
          "aarch64.neon.ordered.panel8.i32x8.lazy." + version +
          std::to_string(source) + ".to.row" +
          std::to_string(destination);
      const bool useNewValue = source < destination;
      const std::size_t expectedOperands = useNewValue ? 1 : 2;
      if (opcode != expected ||
          instruction.operands.size() != expectedOperands) {
        continue;
      }
      int sourceFirst = source * 2;
      std::size_t coefficientIndex = 0;
      if (!useNewValue) {
        const auto sourceAddress =
            integerOperand(os, function, instruction.operands[0],
                           kAddrScratch);
        os << "  ldp q21, q22, [" << sourceAddress << "]\n";
        sourceFirst = 21;
        coefficientIndex = 1;
      }
      const auto coefficient =
          integerOperand(os, function,
                         instruction.operands[coefficientIndex], kImmScratch);
      os << "  dup v20.4s, "
         << integerRegister(coefficient, ValueType::int32Type()) << '\n';
      const int destinationFirst = destination * 2;
      for (int vector = 0; vector < 2; ++vector) {
        os << "  mla v" << destinationFirst + vector << ".4s, v"
           << sourceFirst + vector << ".4s, v20.4s\n";
      }
      return;
    }
  }
  const std::string panelStorePrefix =
      "aarch64.neon.ordered.panel8.i32x8.lazy.store.row";
  if (opcode.rfind(panelStorePrefix, 0) == 0 &&
      opcode.size() == panelStorePrefix.size() + 1 &&
      opcode.back() >= '0' && opcode.back() <= '7' &&
      instruction.operands.size() == 1) {
    const int row = opcode.back() - '0';
    const auto address =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    os << "  stp q" << row * 2 << ", q" << row * 2 + 1 << ", ["
       << address << "]\n";
    return;
  }
  if (opcode == "aarch64.neon.square.sum.i32.init" &&
      instruction.operands.empty()) {
    for (int vector = 16; vector <= 19; ++vector) {
      os << "  movi v" << vector << ".2d, #0\n";
    }
    return;
  }
  const auto emitSquareSum = [&](int firstResult) {
    for (int lane = 0; lane < 8; ++lane) {
      os << "  mla v" << 16 + (lane & 3) << ".4s, v"
         << firstResult + lane << ".4s, v" << firstResult + lane
         << ".4s\n";
    }
  };
  if (opcode == "aarch64.neon.square.sum.i32.accumulate.i32x32" &&
      instruction.operands.empty()) {
    emitSquareSum(20);
    return;
  }
  if (opcode ==
          "aarch64.neon.square.sum.i32.accumulate.panel.i32x16x2" &&
      instruction.operands.empty()) {
    emitSquareSum(8);
    return;
  }
  if (opcode ==
          "aarch64.neon.square.sum.i32.accumulate.panel8.i32x8" &&
      instruction.operands.empty()) {
    for (int vector = 0; vector < 16; ++vector) {
      os << "  mla v" << 16 + (vector & 3) << ".4s, v" << vector
         << ".4s, v" << vector << ".4s\n";
    }
    return;
  }
  if (opcode == "aarch64.neon.square.sum.i32.accumulate.scalar" &&
      instruction.operands.size() == 1) {
    const auto value =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  movi v31.2d, #0\n";
    os << "  mov v31.s[0], "
       << integerRegister(value, ValueType::int32Type()) << '\n';
    os << "  mla v16.4s, v31.4s, v31.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.square.sum.i32.finish" &&
      instruction.hasResult && instruction.operands.empty()) {
    const auto destination = resultRegister(instruction.result, kImmScratch);
    os << "  add v16.4s, v16.4s, v17.4s\n";
    os << "  add v18.4s, v18.4s, v19.4s\n";
    os << "  add v16.4s, v16.4s, v18.4s\n";
    os << "  addv s16, v16.4s\n";
    os << "  fmov "
       << integerRegister(destination, ValueType::int32Type()) << ", s16\n";
    storeResult(os, function, instruction.result, destination);
    return;
  }
  if (opcode == "aarch64.neon.reduce.mls.i32x8.init" &&
      instruction.operands.size() == 1) {
    const auto initial = integerOperand(os, function, instruction.operands[0],
                                        kImmScratch);
    os << "  movi v0.2d, #0\n";
    os << "  movi v1.2d, #0\n";
    os << "  mov v0.s[0], "
       << integerRegister(initial, ValueType::int32Type()) << '\n';
    return;
  }
  if (opcode == "aarch64.neon.reduce.mls.i32x8.step" &&
      instruction.operands.size() == 3) {
    const auto leftBase =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto rightBase =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    const auto index =
        integerOperand(os, function, instruction.operands[2], kHiddenScratch);
    os << "  add x16, " << leftBase << ", "
       << integerRegister(index, ValueType::int32Type()) << ", sxtw #2\n";
    os << "  add x17, " << rightBase << ", "
       << integerRegister(index, ValueType::int32Type()) << ", sxtw #2\n";
    os << "  ldp q2, q3, [x16]\n";
    os << "  ldp q4, q5, [x17]\n";
    os << "  mls v0.4s, v2.4s, v4.4s\n";
    os << "  mls v1.4s, v3.4s, v5.4s\n";
    return;
  }
  if (opcode == "aarch64.neon.reduce.mls.i32x8.finish" &&
      instruction.hasResult && instruction.operands.empty()) {
    const auto destination = resultRegister(instruction.result, kImmScratch);
    os << "  add v0.4s, v0.4s, v1.4s\n";
    os << "  addv s0, v0.4s\n";
    os << "  fmov "
       << integerRegister(destination, ValueType::int32Type()) << ", s0\n";
    storeResult(os, function, instruction.result, destination);
    return;
  }
  if (opcode == "aarch64.neon.srem.i32x4.init" &&
      instruction.operands.size() == 1 &&
      instruction.operands[0].kind == MachineOperandKind::ImmediateInt &&
      instruction.operands[0].intValue == 11) {
    os << "  movz w16, #0xa2e9\n";
    os << "  movk w16, #0x2e8b, lsl #16\n";
    os << "  dup v0.4s, w16\n";
    os << "  movi v1.4s, #11\n";
    return;
  }
  if (opcode == "aarch64.neon.srem.i32x4.step" &&
      instruction.operands.size() == 2) {
    const auto base =
        integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto index =
        integerOperand(os, function, instruction.operands[1], kImmScratch);
    os << "  add x16, " << base << ", "
       << integerRegister(index, ValueType::int32Type()) << ", sxtw #2\n";
    os << "  ldr q2, [x16]\n";
    os << "  smull2 v3.2d, v2.4s, v0.4s\n";
    os << "  smull v4.2d, v2.2s, v0.2s\n";
    os << "  uzp2 v3.4s, v4.4s, v3.4s\n";
    os << "  sshr v4.4s, v3.4s, #1\n";
    os << "  usra v4.4s, v3.4s, #31\n";
    os << "  mls v2.4s, v4.4s, v1.4s\n";
    os << "  str q2, [x16]\n";
    return;
  }
  if (opcode == "aarch64.neon.pure.map.i32x4.init" &&
      instruction.operands.size() == 4) {
    for (std::size_t lane = 0; lane < 4; ++lane) {
      const auto value =
          integerOperand(os, function, instruction.operands[lane], kImmScratch);
      if (lane == 0) {
        os << "  dup v16.4s, " << integerRegister(value, ValueType::int32Type())
           << '\n';
      } else {
        os << "  mov v16.s[" << lane << "], "
           << integerRegister(value, ValueType::int32Type()) << '\n';
      }
    }
    return;
  }
  if (opcode == "aarch64.neon.pure.map.binary.i32x4" &&
      instruction.operands.size() == 6) {
    for (const auto& operand : instruction.operands) {
      if (operand.kind != MachineOperandKind::ImmediateInt) return;
    }
    const auto operation = instruction.operands[0].intValue;
    const auto destination = instruction.operands[1].intValue;
    const auto lhsImmediate = instruction.operands[2].intValue != 0;
    const auto lhsValue = instruction.operands[3].intValue;
    const auto rhsImmediate = instruction.operands[4].intValue != 0;
    const auto rhsValue = instruction.operands[5].intValue;
    if (destination < 0 || destination >= 14 ||
        (!lhsImmediate && (lhsValue < 0 || lhsValue >= 14)) ||
        (!rhsImmediate && (rhsValue < 0 || rhsValue >= 14))) {
      return;
    }
    const auto vectorRegister = [](std::int64_t slot) {
      return "v" + std::to_string(16 + slot) + ".4s";
    };
    std::string lhs;
    if (lhsImmediate) {
      emitImmediate(os, "w16", static_cast<std::int32_t>(lhsValue),
                    ValueType::int32Type());
      os << "  dup v30.4s, w16\n";
      lhs = "v30.4s";
    } else {
      lhs = vectorRegister(lhsValue);
    }
    std::string rhs;
    if (rhsImmediate) {
      emitImmediate(os, "w16", static_cast<std::int32_t>(rhsValue),
                    ValueType::int32Type());
      os << "  dup v31.4s, w16\n";
      rhs = "v31.4s";
    } else {
      rhs = vectorRegister(rhsValue);
    }
    static const std::array<const char*, 5> operations{"add", "sub", "mul",
                                                       "smax", "smin"};
    if (operation < 0 ||
        operation >= static_cast<std::int64_t>(operations.size())) {
      return;
    }
    os << "  " << operations[operation] << ' ' << vectorRegister(destination)
       << ", " << lhs << ", " << rhs << '\n';
    return;
  }
  if ((opcode == "aarch64.neon.pure.map.sdiv.i32x4" ||
       opcode == "aarch64.neon.pure.map.srem.i32x4") &&
      instruction.operands.size() == 5) {
    for (const auto& operand : instruction.operands) {
      if (operand.kind != MachineOperandKind::ImmediateInt) return;
    }
    const auto destination = instruction.operands[0].intValue;
    const auto source = instruction.operands[1].intValue;
    const auto divisor = instruction.operands[2].intValue;
    const auto magic = instruction.operands[3].intValue;
    const auto shift = instruction.operands[4].intValue;
    if (destination < 0 || destination >= 14 || source < 0 || source >= 14 ||
        divisor <= 1 || divisor > std::numeric_limits<std::int32_t>::max() ||
        shift < 0 || shift >= 31) {
      return;
    }
    const auto vectorRegister = [](std::int64_t slot) {
      return "v" + std::to_string(16 + slot) + ".4s";
    };
    const auto vectorBase = [](std::int64_t slot) {
      return "v" + std::to_string(16 + slot);
    };
    const auto sourceRegister = vectorRegister(source);
    const auto destinationRegister = vectorRegister(destination);
    emitImmediate(os, "w16", static_cast<std::int32_t>(magic),
                  ValueType::int32Type());
    os << "  dup v30.4s, w16\n";
    os << "  smull2 v31.2d, " << sourceRegister << ", v30.4s\n";
    os << "  smull v30.2d, " << vectorBase(source) << ".2s, v30.2s\n";
    os << "  uzp2 " << destinationRegister << ", v30.4s, v31.4s\n";
    if (static_cast<std::int32_t>(magic) < 0) {
      os << "  add " << destinationRegister << ", " << destinationRegister
         << ", " << sourceRegister << '\n';
    }
    if (shift == 0) {
      os << "  mov v31.16b, " << vectorBase(destination) << ".16b\n";
    } else {
      os << "  sshr v31.4s, " << destinationRegister << ", #" << shift << '\n';
    }
    os << "  usra v31.4s, " << destinationRegister << ", #31\n";
    os << "  mov " << vectorBase(destination) << ".16b, v31.16b\n";
    if (opcode == "aarch64.neon.pure.map.srem.i32x4") {
      os << "  mov v31.16b, " << vectorBase(destination) << ".16b\n";
      os << "  mov " << vectorBase(destination) << ".16b, "
         << vectorBase(source) << ".16b\n";
      emitImmediate(os, "w16", divisor, ValueType::int32Type());
      os << "  dup v30.4s, w16\n";
      os << "  mls " << destinationRegister << ", v31.4s, v30.4s\n";
    }
    return;
  }
  if (opcode == "aarch64.neon.pure.map.extract.i32x4" &&
      instruction.hasResult && instruction.operands.size() == 2 &&
      instruction.operands[0].kind == MachineOperandKind::ImmediateInt &&
      instruction.operands[1].kind == MachineOperandKind::ImmediateInt) {
    const auto slot = instruction.operands[0].intValue;
    const auto lane = instruction.operands[1].intValue;
    if (slot < 0 || slot >= 14 || lane < 0 || lane >= 4) return;
    const auto destination = resultRegister(instruction.result, kImmScratch);
    os << "  mov " << integerRegister(destination, ValueType::int32Type())
       << ", v" << (16 + slot) << ".s[" << lane << "]\n";
    storeResult(os, function, instruction.result, destination);
    return;
  }
  if (opcode == "aarch64.widened.mul.lhs.i32" &&
      instruction.operands.size() == 1) {
    const auto source =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  mov w16, "
       << integerRegister(source, ValueType::int32Type()) << '\n';
    return;
  }
  if (opcode == "aarch64.widened.mul.rhs.i32" &&
      instruction.operands.size() == 1) {
    const auto source =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  smull x16, w16, "
       << integerRegister(source, ValueType::int32Type()) << '\n';
    return;
  }
  if (opcode == "aarch64.widened.mod.i32" &&
      instruction.hasResult && instruction.operands.size() == 1) {
    const auto modulus =
        integerOperand(os, function, instruction.operands[0], kImmScratch);
    os << "  sdiv x18, x16, "
       << integerRegister(modulus, ValueType::int64Type()) << '\n';
    os << "  msub x16, x18, "
       << integerRegister(modulus, ValueType::int64Type()) << ", x16\n";
    storeResult(os, function, instruction.result, "x16");
    return;
  }
  if (opcode == "fneg") {
    const auto source = floatOperand(os, function, instruction.operands[0], kFloatScratch);
    const auto dst = resultRegister(instruction.result, "s17"); os << "  fneg " << dst << ", " << source << '\n';
    storeResult(os, function, instruction.result, dst); return;
  }
  if ((opcode == "madd" || opcode == "msub") &&
      instruction.operands.size() == 3) {
    if (std::getenv("COMPILER_DISABLE_AARCH64_SHIFTED_MADD") == nullptr) {
      std::optional<std::size_t> constantIndex;
      std::uint64_t magnitude = 0;
      for (std::size_t index = 0; index < 2; ++index) {
        const auto& operand = instruction.operands[index];
        if (operand.kind == MachineOperandKind::ImmediateInt &&
            powerOfTwoMagnitude(operand.intValue, magnitude)) {
          constantIndex = index;
          break;
        }
      }

      if (constantIndex.has_value()) {
        const auto type = instruction.result.type;
        const unsigned width = isNarrow(type) ? 32U : 64U;
        const unsigned shift = log2PowerOfTwo(magnitude);
        if (shift < width) {
          const auto& constant = instruction.operands[*constantIndex];
          const auto& valueOperand = instruction.operands[1U - *constantIndex];
          const auto value = integerOperand(os, function, valueOperand,
                                            kAddrScratch);
          const auto accumulator = integerOperand(
              os, function, instruction.operands[2], kHiddenScratch);
          const auto dst = resultRegister(instruction.result, kImmScratch);
          const bool subtract =
              (opcode == "msub") != (constant.intValue < 0);
          os << "  " << (subtract ? "sub" : "add") << ' '
             << integerRegister(dst, type) << ", "
             << integerRegister(accumulator, type) << ", "
             << integerRegister(value, type);
          if (shift != 0) os << ", lsl #" << shift;
          os << '\n';
          storeResult(os, function, instruction.result, dst);
          return;
        }
      }
    }

    const auto lhs = integerOperand(os, function, instruction.operands[0], kAddrScratch);
    const auto rhs = integerOperand(os, function, instruction.operands[1], kImmScratch);
    const auto accumulator =
        integerOperand(os, function, instruction.operands[2], kHiddenScratch);
    const auto dst = resultRegister(instruction.result, kImmScratch);
    const auto type = instruction.result.type;
    os << "  " << opcode << ' ' << integerRegister(dst, type) << ", "
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
    const std::string* fallthrough =
        index + 1 < function.blocks.size()
            ? &labels.at(function.blocks[index + 1].name)
            : &exit;
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
  auto assembly = os.str();
  const auto collapsed = collapsePositivePowerOfTwoSteps(assembly);
  assembly = memoizePureAdditiveTailLoop(
      collapsed, module.singleImmutableTailMemoContext);
  assembly = synthesizeConstantMultiplies(assembly);
  return pairAdjacentStackAccesses(assembly);
}

}  // namespace compiler::backend::asmcode
