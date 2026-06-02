#include "../../../include/compiler/midend/pass/PipelineBuilder.h"

#include "../../../include/compiler/midend/pass/CommonSubexpressionEliminationPass.h"
#include "../../../include/compiler/midend/pass/ConstantFoldingPass.h"
#include "../../../include/compiler/midend/pass/DeadCodeEliminationPass.h"
#include "../../../include/compiler/midend/pass/FunctionInliningPass.h"
#include "../../../include/compiler/midend/pass/LateFunctionInliningPass.h"
#include "../../../include/compiler/midend/pass/LocalScalarCleanupPass.h"
#include "../../../include/compiler/midend/pass/LoopInvariantCodeMotionPass.h"
#include "../../../include/compiler/midend/pass/Mem2RegPass.h"
#include "../../../include/compiler/midend/pass/PhiSimplificationPass.h"
#include "../../../include/compiler/midend/pass/RepeatedAccumulationEliminationPass.h"
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

PassManager PipelineBuilder::buildLoopPipeline() {
  PassManager pipeline;
  pipeline.add(std::make_unique<LoopInvariantCodeMotionPass>());
  pipeline.add(std::make_unique<RepeatedAccumulationEliminationPass>());
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

PassManager PipelineBuilder::buildO1Pipeline() {
  PassManager pipeline;
  pipeline.add(fixedPoint("ssa-enabling", buildSsaEnablingPipeline(), 2));
  pipeline.add(fixedPoint("early-inline", buildInlinePipeline(), 3));
  pipeline.add(std::make_unique<LoopInvariantCodeMotionPass>());
  pipeline.add(std::make_unique<RepeatedAccumulationEliminationPass>());
  pipeline.add(fixedPoint("late-inline", buildLateInlinePipeline(), 2));
  pipeline.add(fixedPoint("basic-clean", buildBasicCleanPipeline(), 4));
  return pipeline;
}

} // namespace compiler::midend::pass
