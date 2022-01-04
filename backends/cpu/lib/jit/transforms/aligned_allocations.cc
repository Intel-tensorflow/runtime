/* Copyright 2022 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include <cassert>
#include <memory>
#include <utility>

#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/OperationSupport.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "tfrt/cpu/jit/transforms/codegen_passes.h"

namespace tfrt {
namespace cpu {
namespace jit {
namespace {

#define GEN_PASS_CLASSES
#include "tfrt/cpu/jit/transforms/codegen_gen_passes.h.inc"

struct AlignedAllocationsPass
    : public AlignedAllocationsPassBase<AlignedAllocationsPass> {
  explicit AlignedAllocationsPass(int64_t alignment) { alignment_ = alignment; }
  void runOnFunction() override;
};

}  // namespace

void AlignedAllocationsPass::runOnFunction() {
  assert(alignment_ >= 0 && "alignment must be larger or equal to 0");
  if (alignment_ == 0) return;

  auto i64 = mlir::IntegerType::get(&getContext(), 64);
  auto alignment_attr = mlir::IntegerAttr::get(i64, alignment_);

  getFunction().walk([&](mlir::memref::AllocOp alloc) {
    // Add alignment attribute only if the alignment attribute is missing or the
    // current alignment is smaller.
    if (!alloc.alignment().hasValue() || *alloc.alignment() < alignment_)
      alloc.alignmentAttr(alignment_attr);
  });
}

std::unique_ptr<mlir::FunctionPass> CreateAlignedAllocationsPass(
    int64_t alignment) {
  return std::make_unique<AlignedAllocationsPass>(alignment);
}

}  // namespace jit
}  // namespace cpu
}  // namespace tfrt
