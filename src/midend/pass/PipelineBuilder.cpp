#include "../../../include/compiler/midend/pass/PipelineBuilder.h"

#include "../../../include/compiler/midend/pass/ArrayAccessSimplificationPass.h"
#include "../../../include/compiler/midend/pass/BasicAliasAnalysisPass.h"
#include "../../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"
#include "../../../include/compiler/midend/pass/CompareSimplificationPass.h"
#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"
#include "../../../include/compiler/midend/pass/DeadCodeEliminationPass.h"
#include "../../../include/compiler/midend/pass/DeadStoreEliminationPass.h"
#include "../../../include/compiler/midend/pass/FunctionSpecializationPass.h"
#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"
#include "../../../include/compiler/midend/pass/GlobalConstPropagationPass.h"
#include "../../../include/compiler/midend/pass/GlobalDCEPass.h"
#include "../../../include/compiler/midend/pass/GlobalValueNumberingPass.h"
#include "../../../include/compiler/midend/pass/IndVarSimplificationPass.h"
#include "../../../include/compiler/midend/pass/InternalizePass.h"
#include "../../../include/compiler/midend/pass/LCSSAPass.h"
#include "../../../include/compiler/midend/pass/LateFunctionInliningPass.h"
#include "../../../include/compiler/midend/pass/LoadEliminationPass.h"
#include "../../../include/compiler/midend/pass/LocalScalarCleanupPass.h"
#include "../../../include/compiler/midend/pass/LoopAliasAnalysisPass.h"
#include "../../../include/compiler/midend/pass/LoopAnalysisPass.h"
#include "../../../include/compiler/midend/pass/LoopEliminationPass.h"
#include "../../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"
#include "../../../include/compiler/midend/pass/LoopRotatePass.h"
#include "../../../include/compiler/midend/pass/LoopSimplificationPass.h"
#include "../../../include/compiler/midend/pass/LoopStrengthReductionPass.h"
#include "../../../include/compiler/midend/pass/LoopUnrollPass.h"
#include "../../../include/compiler/midend/pass/LoopUnswitchPass.h"
#include "../../../include/compiler/midend/pass/Mem2RegPass.h"
#include "../../../include/compiler/midend/pass/PhiSimplificationPass.h"
#include "../../../include/compiler/midend/pass/RepeatedAccumulationEliminationPass.h"
#include "../../../include/compiler/midend/pass/ReassociatePass.h"
#include "../../../include/compiler/midend/pass/ScalarReplacementPass.h"
#include "../../../include/compiler/midend/pass/SCEVAnalysisPass.h"
#include "../../../include/compiler/midend/pass/SparseConditionalConstantPropagationPass.h"
#include "../../../include/compiler/midend/pass/StoreToLoadForwardingPass.h"
#include "../../../include/compiler/midend/pass/TreeShakingPass.h"
#include "../../../include/compiler/midend/pass/TrivialBlockEliminationPass.h"

#include <memory>
#include <string>
#include <utility>

namespace compiler::midend::pass {

namespace {

class FixedPointPipelinePass final : public ModulePass {
public:
  FixedPointPipelinePass(std::string name, PassManager pipeline, int maxRounds)
      : name_(std::move(name)), pipeline_(std::move(pipeline)),
        maxRounds_(maxRounds) {}

  std::string name() const override { return name_; }

  bool run(compiler::ir::core::Module &module) override {
    return pipeline_.runFixedPoint(module, maxRounds_);
  }

private:
  std::string name_;
  PassManager pipeline_;
  int maxRounds_;
};

std::unique_ptr<ModulePass> fixedPoint(std::string name, PassManager pipeline,
                                       int maxRounds) {
  return std::make_unique<FixedPointPipelinePass>(
      std::move(name), std::move(pipeline), maxRounds);
}

} // namespace

PassManager PipelineBuilder::buildO0Pipeline() { return {}; }

PassManager PipelineBuilder::buildBasicCleanPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<ConstantFoldingPass>());
  pipeline.add(std::make_unique<LocalScalarCleanupPass>());
  pipeline.add(std::make_unique<CommonSubexpressionEliminationPass>());
  pipeline.add(std::make_unique<DeadCodeEliminationPass>());
  pipeline.add(std::make_unique<TrivialBlockEliminationPass>());
  pipeline.add(std::make_unique<PhiSimplificationPass>());
  return pipeline;
}

PassManager PipelineBuilder::buildSsaEnablingPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<TrivialBlockEliminationPass>());
  pipeline.add(std::make_unique<Mem2RegPass>());
  pipeline.add(std::make_unique<PhiSimplificationPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

PassManager PipelineBuilder::buildInlinePipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<FunctionInliningPass>());
  pipeline.add(fixedPoint("ssa-enabling", buildSsaEnablingPipeline(), 2));
  return pipeline;
}

PassManager PipelineBuilder::buildLateInlinePipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<LateFunctionInliningPass>());
  pipeline.add(fixedPoint("ssa-enabling", buildSsaEnablingPipeline(), 2));
  return pipeline;
}

PassManager PipelineBuilder::buildSccpCleanPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<SparseConditionalConstantPropagationPass>());
  pipeline.add(std::make_unique<DeadCodeEliminationPass>());
  pipeline.add(std::make_unique<TrivialBlockEliminationPass>());
  pipeline.add(std::make_unique<PhiSimplificationPass>());
  return pipeline;
}

PassManager PipelineBuilder::buildScalarPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<ConstantFoldingPass>());
  pipeline.add(std::make_unique<SparseConditionalConstantPropagationPass>());
  pipeline.add(std::make_unique<ReassociatePass>());
  pipeline.add(std::make_unique<ConstantFoldingPass>());
  pipeline.add(std::make_unique<LocalScalarCleanupPass>());
  pipeline.add(std::make_unique<CommonSubexpressionEliminationPass>());
  pipeline.add(std::make_unique<GlobalValueNumberingPass>());
  pipeline.add(std::make_unique<CompareSimplificationPass>());
  pipeline.add(std::make_unique<DeadCodeEliminationPass>());
  pipeline.add(std::make_unique<TrivialBlockEliminationPass>());
  pipeline.add(std::make_unique<PhiSimplificationPass>());
  return pipeline;
}

PassManager PipelineBuilder::buildMemoryPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<BasicAliasAnalysisPass>());
  pipeline.add(std::make_unique<LoadEliminationPass>());
  pipeline.add(std::make_unique<StoreToLoadForwardingPass>());
  pipeline.add(std::make_unique<DeadStoreEliminationPass>());
  pipeline.add(std::make_unique<ScalarReplacementPass>());
  pipeline.add(std::make_unique<Mem2RegPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

PassManager PipelineBuilder::buildLoopPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<LoopAnalysisPass>());
  pipeline.add(std::make_unique<LoopSimplificationPass>());
  pipeline.add(std::make_unique<LCSSAPass>());
  pipeline.add(std::make_unique<LoopAliasAnalysisPass>());
  pipeline.add(std::make_unique<LoopInvariantCodeMotionPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));

  pipeline.add(std::make_unique<LoopRotatePass>());
  pipeline.add(std::make_unique<LoopSimplificationPass>());
  pipeline.add(std::make_unique<LCSSAPass>());
  pipeline.add(std::make_unique<LoopAliasAnalysisPass>());
  pipeline.add(std::make_unique<LoopInvariantCodeMotionPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));

  pipeline.add(std::make_unique<SCEVAnalysisPass>());
  pipeline.add(std::make_unique<IndVarSimplificationPass>());
  pipeline.add(std::make_unique<ArrayAccessSimplificationPass>());
  pipeline.add(std::make_unique<LoopStrengthReductionPass>());
  pipeline.add(std::make_unique<RepeatedAccumulationEliminationPass>());
  pipeline.add(std::make_unique<LoopEliminationPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));

  pipeline.add(std::make_unique<LoopUnrollPass>());
  pipeline.add(std::make_unique<LoopUnswitchPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  pipeline.add(fixedPoint("memory-main", buildMemoryPipeline(), 3));
  return pipeline;
}

PassManager PipelineBuilder::buildModulePipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<InternalizePass>());
  pipeline.add(std::make_unique<TreeShakingPass>());
  pipeline.add(std::make_unique<GlobalConstPropagationPass>());
  pipeline.add(std::make_unique<GlobalDCEPass>());
  pipeline.add(std::make_unique<FunctionSpecializationPass>());
  pipeline.add(fixedPoint("late-inline", buildLateInlinePipeline(), 2));
  pipeline.add(std::make_unique<TreeShakingPass>());
  pipeline.add(std::make_unique<GlobalDCEPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

PassManager PipelineBuilder::buildO1Pipeline() {
  PassManager pipeline;
  pipeline.add(fixedPoint("ssa-enabling", buildSsaEnablingPipeline(), 2));
  pipeline.add(fixedPoint("early-inline", buildInlinePipeline(), 3));
  pipeline.add(fixedPoint("scalar-main", buildScalarPipeline(), 4));
  pipeline.add(fixedPoint("memory-main", buildMemoryPipeline(), 3));
  pipeline.add(fixedPoint("loop-main", buildLoopPipeline(), 2));
  pipeline.add(fixedPoint("memory-main", buildMemoryPipeline(), 3));
  pipeline.add(fixedPoint("module-opt", buildModulePipeline(), 2));
  pipeline.add(fixedPoint("scalar-main", buildScalarPipeline(), 4));
  pipeline.add(fixedPoint("memory-main", buildMemoryPipeline(), 3));
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

} // namespace compiler::midend::pass
