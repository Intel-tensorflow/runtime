/*
 * Copyright 2021 The TensorFlow Runtime Authors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//===- jitrt.cc - ---------------------------------------------------------===//
// Support library for implementing TFRT kernels that do JIT compilation using
// MLIR framework.
//===----------------------------------------------------------------------===//

#include "tfrt/jitrt/jitrt.h"

#include <sys/types.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <memory>
#include <numeric>
#include <string>
#include <type_traits>
#include <utility>

#include "llvm/ExecutionEngine/Orc/Core.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/IR/PassTimingInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/ExecutionEngine/CRunnerUtils.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Parser/Parser.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Support/LogicalResult.h"
#include "mlir/Support/Timing.h"
#include "mlir/Target/LLVMIR/ModuleTranslation.h"
#include "tfrt/dtype/dtype.h"
#include "tfrt/host_context/async_value_ref.h"
#include "tfrt/jitrt/results.h"
#include "tfrt/jitrt/xla.h"
#include "tfrt/support/error_util.h"
#include "tfrt/tensor/dense_host_tensor.h"
#include "third_party/tensorflow/compiler/xla/mlir/transforms/runtime/passes.h"
#include "third_party/tensorflow/compiler/xla/mlir/transforms/runtime/specialization.h"
#include "third_party/tensorflow/compiler/xla/mlir/utils/runtime/async_runtime_api.h"
#include "third_party/tensorflow/compiler/xla/mlir/utils/runtime/constraints.h"
#include "third_party/tensorflow/compiler/xla/runtime/arguments.h"
#include "third_party/tensorflow/compiler/xla/runtime/async_runtime.h"
#include "third_party/tensorflow/compiler/xla/runtime/constraints.h"
#include "third_party/tensorflow/compiler/xla/runtime/custom_call_registry.h"
#include "third_party/tensorflow/compiler/xla/runtime/diagnostics.h"
#include "third_party/tensorflow/compiler/xla/runtime/executable.h"
#include "third_party/tensorflow/compiler/xla/runtime/execution_engine.h"
#include "third_party/tensorflow/compiler/xla/runtime/runtime.h"
#include "third_party/tensorflow/compiler/xla/runtime/symbolic_shape.h"

//----------------------------------------------------------------------------//
// XLA Runtime API implementation.
//----------------------------------------------------------------------------//

namespace xla {
namespace runtime {

using tfrt::jitrt::CustomCall;
using tfrt::jitrt::DiagnosticEngine;
using tfrt::jitrt::Executable;

llvm::orc::SymbolMap RuntimeApiSymbolMap(llvm::orc::MangleAndInterner);

}  // namespace runtime
}  // namespace xla

namespace tfrt {
namespace jitrt {

using llvm::ErrorOr;
using mlir::FailureOr;
using mlir::succeeded;

using xla::runtime::ArgumentConstraint;
using xla::runtime::GetArgumentsConstraints;

// Enable IR printing during the kernel compilation pipeline execution.
static bool DebugJitrtCompile() {
#if defined(DEBUG_JITRT)
  return true;
#else
  return false;
#endif
}

static bool EnablePassTiming() {
  if (DebugJitrtCompile()) return true;

#if defined(ENABLE_JITRT_PASS_TIMING)
  return true;
#else
  return false;
#endif
}

// Escape slashes, substituting them with double underscores to get a memory
// region name for the JitRtMemoryMapper.
//
// The profiler's UI might interpret slashes as callchain separators,
// whereas we want the region name to be shown in full.
static std::string EscapeMemRegionName(llvm::StringRef memory_region_name) {
  return llvm::join(llvm::split(memory_region_name, '/'), "__");
}

// -------------------------------------------------------------------------- //
// Converting from runtime buffers (aka Tensors) to Memref descriptors.
// -------------------------------------------------------------------------- //

Expected<MemrefDesc> ConvertTensorToMemrefDesc(const Tensor& tensor) {
  if (auto* dht = dyn_cast<DenseHostTensor>(&tensor)) {
    return MemrefDesc(dht->shape().GetRank(), dht->dtype(),
                      const_cast<void*>(dht->data()), 0,
                      [&](auto sizes, auto strides) {
                        dht->shape().GetDimensions(sizes);
                        dht->shape().GetStrides(strides);
                      });
  }

  return MakeStringError("unsupported tensor type: ", tensor.tensor_type());
}

//----------------------------------------------------------------------------//
// Setup MLIR pass pipeline to lower to LLVM dialect, and use ORC JIT to codegen
// functions at runtime.
//----------------------------------------------------------------------------//

static void InitializeCompiler() {
  static const bool initialized = ([] {
    llvm::InitializeNativeTarget();
    // Initialize asm printer and parser so that we can handle the inline
    // assembly generated in MLIR for some operations.
    llvm::InitializeNativeTargetAsmPrinter();
    llvm::InitializeNativeTargetAsmParser();
    return true;
  })();
  (void)initialized;
}

static void SetupPassDebugging(mlir::MLIRContext* context,
                               mlir::PassManager& pm) {
  // Print IR after all passes.
  if (DebugJitrtCompile()) {
    context->disableMultithreading();
    pm.enableIRPrinting([](mlir::Pass*, mlir::Operation*) { return false; },
                        [](mlir::Pass*, mlir::Operation*) { return true; },
                        /*printModuleScope=*/true,
                        /*printAfterOnlyOnChange=*/false,
                        /*printAfterOnlyOnFailure=*/false, llvm::errs());
  }
}

static mlir::LogicalResult RunPipeline(
    mlir::ModuleOp module,
    const std::function<void(mlir::PassManager&)>& create_pipeline) {
  if (!create_pipeline) return mlir::success();

  mlir::PassManager pm(module.getContext());
  SetupPassDebugging(module.getContext(), pm);

  // Instrument the pass manager to capture timing information.
  mlir::DefaultTimingManager tm;
  mlir::TimingScope timing;
  if (EnablePassTiming()) {
    tm.setEnabled(true);
    timing = tm.getRootScope();
    pm.enableTiming(timing);
  }

  create_pipeline(pm);

  return pm.run(module);
}

// Runs the user-provided compilation pipeline to compile the module to LLVM.
static mlir::LogicalResult RunCompilationPipeline(
    mlir::ModuleOp module, const CompilationOptions& opts) {
  return RunPipeline(module, opts.create_compilation_pipeline);
}

// Runs the user-provided specialization pipeline.
static mlir::LogicalResult RunSpecializationPipeline(
    mlir::ModuleOp module, const CompilationOptions& opts) {
  return RunPipeline(module, opts.create_specialization_pipeline);
}

//----------------------------------------------------------------------------//
// JitCompilationContext to manage specialization and compilation.
//----------------------------------------------------------------------------//

using SymbolicShape = SymbolicShapesResolver::SymbolicShape;

namespace internal {
// JitCompilationContext manages parsing, specialization and compilation of a
// single compiled module. It owns the MLIR context where the module is created,
// and handlers to capture all diagnostics messages.
class JitCompilationContext {
 public:
  // Instantiates JIT compilation context from the serialized mlir source.
  static Expected<std::unique_ptr<JitCompilationContext>> Instantiate(
      CompilationOptions opts, string_view mlir_module, string_view entrypoint);

  // Makes an executable from the JIT compilation context. This is the end of
  // life for the compilation context, it effectively converts the MLIR module
  // to the executable (function pointer) using LLVM JIT code generation.
  // Optional specialization identifier specifies if the compiled executable is
  // a default one, or a specialization.
  static Expected<Executable> Compile(
      std::unique_ptr<JitCompilationContext>, string_view memory_region_name,
      Optional<size_t> specialization = llvm::None);

  template <typename OriginalError>
  llvm::Error Error(OriginalError original_error) {
    return MakeStringError(original_error, ":\n", diagnostic_);
  }

  llvm::StringRef name() const {
    return module().getName().value_or("<unknown>");
  }

  mlir::ModuleOp module() const {
    assert(module_ && "failed to parse the mlir module");
    return *module_;
  }

  mlir::func::FuncOp entrypoint() const {
    assert(entrypoint_ && "failed to resolve entrypoint function");
    return entrypoint_;
  }

  // Specialize compiled module to the arguments:
  //
  // - update all unknown dimensions according to the resolved symbolic shapes
  // - attach symbolic shape attribute to the operands
  // - sink small constants into the function body
  //
  // After entrypoint signature is updated, and all constant arguments
  // materialized in the function body, runs the user-provided specialization
  // pipeline to optimize the module based on the new information in the IR.
  //
  // Returns error if arguments are not compatible with compiled module
  // entrypoint signature.
  llvm::Error Specialize(ArgumentsRef arguments,
                         ArrayRef<SymbolicShape> symbolic_shapes,
                         ArrayRef<ArgumentConstraint> constraints,
                         const SpecializationListener* listener);

  const CompilationOptions& options() const { return opts_; }

 private:
  JitCompilationContext(CompilationOptions opts, string_view mlir_module,
                        string_view entrypoint);

  CompilationOptions opts_;
  std::unique_ptr<mlir::MLIRContext> context_;
  std::string diagnostic_;
  llvm::raw_string_ostream diagnostic_os_;
  llvm::SourceMgr source_mgr_;
  mlir::SourceMgrDiagnosticHandler handler_;
  mlir::OwningOpRef<mlir::ModuleOp>
      module_;                     // can be null if failed to parse the module
  mlir::func::FuncOp entrypoint_;  // can be null if failed to parse the module
  bool specialized_;
};
}  // namespace internal

using JitCompilationContext = internal::JitCompilationContext;

// Creates a new MLIR Context and registers all the dialects that are expected
// in the compiled module.
static std::unique_ptr<mlir::MLIRContext> CreateMlirContext(
    const CompilationOptions& opts) {
  mlir::DialectRegistry registry;

  // Call user-provided callback to register all required dialects.
  if (opts.register_dialects) opts.register_dialects(registry);

  // TODO(ezhulenev): Wrap host context work queue into the llvm ThreadPool API
  // and pass it to all MLIR contexts.
  auto ctx = std::make_unique<mlir::MLIRContext>(
      registry, mlir::MLIRContext::Threading::DISABLED);
  ctx->loadAllAvailableDialects();
  return ctx;
}

JitCompilationContext::JitCompilationContext(CompilationOptions opts,
                                             string_view mlir_module,
                                             string_view entrypoint)
    : opts_(std::move(opts)),
      context_(CreateMlirContext(opts_)),
      diagnostic_os_(diagnostic_),
      handler_(source_mgr_, context_.get(), diagnostic_os_),
      specialized_(false) {
  source_mgr_.AddNewSourceBuffer(
      llvm::MemoryBuffer::getMemBuffer(mlir_module, "jitrt.kernel"),
      llvm::SMLoc());
  module_ = mlir::parseSourceFile<mlir::ModuleOp>(source_mgr_, context_.get());
  if (module_)
    entrypoint_ = module_->lookupSymbol<mlir::func::FuncOp>(entrypoint);
}

/*static*/ Expected<std::unique_ptr<JitCompilationContext>>
JitCompilationContext::Instantiate(CompilationOptions opts,
                                   string_view mlir_module,
                                   string_view entrypoint) {
  std::unique_ptr<JitCompilationContext> context(
      new JitCompilationContext(std::move(opts), mlir_module, entrypoint));
  if (!context->module_)
    return context->Error("failed to parse the mlir source");
  if (!context->entrypoint_)
    return context->Error("failed to resolve entrypoint function");
  return {std::move(context)};
}

/*static*/ Expected<Executable> JitCompilationContext::Compile(
    std::unique_ptr<JitCompilationContext> ctx, string_view memory_region_name,
    Optional<size_t> specialization) {
  const CompilationOptions& opts = ctx->options();
  mlir::func::FuncOp entry_func = ctx->entrypoint();
  std::string entrypoint = entry_func.getName().str();

  // We track end-to-end time to compile the final executable.
  auto compilation_start = std::chrono::steady_clock::now();

  // Get the signature of the entrypoint function.
  auto signature = opts.type_converter.Convert(entry_func.getFunctionType());
  if (auto err = signature.takeError()) return std::move(err);

  // Get the calling convention for the entrypoint function.
  if (!opts.calling_convention)
    return ctx->Error("calling convention is not defined");

  // Calling convention conversion can fail if some types are not supported.
  auto runtime_type = opts.calling_convention(entry_func.getFunctionType());
  if (!runtime_type)
    return ctx->Error("calling convention failed to convert entrypoint type");

  // Get the runtime signature of the entrypoint function.
  auto runtime_signature = opts.type_converter.Convert(runtime_type);
  if (auto err = runtime_signature.takeError()) return std::move(err);

  // Get the memory layout for passing function arguments.
  auto arguments_memory_layout =
      Executable::GetArgumentsMemoryLayout(*runtime_signature);
  if (auto err = arguments_memory_layout.takeError()) return std::move(err);

  // Get the memory layout for returning function results.
  auto results_memory_layout =
      Executable::GetResultsMemoryLayout(*runtime_signature);
  if (auto err = results_memory_layout.takeError()) return std::move(err);

  // Mark entry function with an attribute, so it can be converted to an Xla
  // entrypoint (see `rt-convert-to-entrypoint` pass).
  auto unit_attr = mlir::UnitAttr::get(entry_func.getContext());
  entry_func->setAttr(xla::runtime::kEntrypointAttrName, unit_attr);

  // Run the compilation pipeline to lower the module to LLVM dialect.
  if (failed(RunCompilationPipeline(ctx->module(), opts)))
    return ctx->Error("failed to run compilation pipeline");

  if (EnablePassTiming()) llvm::TimePassesIsEnabled = true;

  // Prepare JIT target machine for code generation.
  auto builder = llvm::orc::JITTargetMachineBuilder::detectHost();
  if (!builder) return builder.takeError();

  auto target_machine = builder->createTargetMachine();
  if (!target_machine) return target_machine.takeError();

  // Name of the compiled module if available.
  auto module_name = ctx->module().getSymName().value_or("<unknown>");

  // Memory region name to mmap executable code.
  std::string mapper_name = llvm::formatv(
      "/jitrt{0}{1}:@{2}::@{3}:{4}", memory_region_name.empty() ? "" : ":",
      EscapeMemRegionName(memory_region_name), module_name, entrypoint,
      specialization.has_value() ? "specialized" : "default");

  // Custom memory mapper to tag memory allocated for JitRt executables.
  std::unique_ptr<XlaRuntimeMemoryMapper> memory_mapper =
      XlaRuntimeMemoryMapper::Create(std::move(mapper_name));

  // Register symbols required for running JitRt Executable.
  ExecutionEngine::SymbolsBinding symbols =
      xla::runtime::RuntimeSymbolsBinding(ctx->options().runtime_symbol_map);

  // Construct options for the JitRt execution engine.
  ExecutionEngine::JitOptions engine_options;
  engine_options.opt_level = ctx->options().jit_code_opt_level;
  engine_options.target_machine = target_machine->get();
  engine_options.make_optimizing_transformer = mlir::makeOptimizingTransformer;
  engine_options.section_memory_mapper = memory_mapper.get();
  engine_options.symbols_binding = std::move(symbols);

  // Translate MLIR module to the LLVM module.
  auto llvm_ctx = std::make_unique<llvm::LLVMContext>();
  auto llvm_module = mlir::translateModuleToLLVMIR(ctx->module(), *llvm_ctx);
  if (!llvm_module)
    return MakeStringError("failed to translate module to LLVM IR");

  // Compile input module to the native function.
  auto engine = ExecutionEngine::CreateFromModule(
      std::move(llvm_ctx), std::move(llvm_module), entrypoint, engine_options);
  if (auto err = engine.takeError()) return std::move(err);

  // At this point compilation is completed, and all symbols in the LLVM module
  // materialized as addresses (entrypoint is an executable function pointer).
  auto time_to_compile = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - compilation_start);

  if (EnablePassTiming()) llvm::reportAndResetTimings();

  return Executable(
      ctx->name().str(), std::move(memory_mapper), std::move(*engine),
      std::move(*signature), std::move(*runtime_signature),
      std::move(*arguments_memory_layout), std::move(*results_memory_layout),
      specialization, time_to_compile);
}

llvm::Error JitCompilationContext::Specialize(
    ArgumentsRef arguments, ArrayRef<SymbolicShape> symbolic_shapes,
    ArrayRef<ArgumentConstraint> constraints,
    const SpecializationListener* listener) {
  assert(!specialized_ && "can specialize executable only once");
  specialized_ = true;

  mlir::func::FuncOp func = entrypoint();

  // Update function signature and sink constant arguments into the body.
  if (auto err = SpecializeFunction(func, arguments, symbolic_shapes,
                                    constraints, listener)) {
    // No need to call this->Error() because we don't have diagnostic to report
    // in case of a failed specialization.
    return MakeStringError("failed to specialize: ", err);
  }

  // Run the user-provided specialization pipeline to take advantage of the
  // specialized operands and sunk constants.
  if (failed(RunSpecializationPipeline(*module_, opts_)))
    return Error("failed to run specialization pipeline");

  return Error::success();
}

//----------------------------------------------------------------------------//
// JitExecutable implementation.
//----------------------------------------------------------------------------//

using Specialization = CompilationOptions::Specialization;

static bool IsSpecializationOnly(ArrayRef<ArgumentConstraint> constraints) {
  return llvm::any_of(constraints, [](ArgumentConstraint constraint) {
    return constraint != ArgumentConstraint::kResolved;
  });
}

static bool HasValueConstraints(ArrayRef<ArgumentConstraint> constraints) {
  return llvm::any_of(constraints, [](ArgumentConstraint constraint) {
    return constraint == ArgumentConstraint::kValue;
  });
}

// Returns true if all function operands have statically known shape.
static bool HasStaticShapeOperands(const FunctionType& signature) {
  auto is_dynamic = [](ArrayRef<Index> sizes) -> bool {
    return llvm::any_of(sizes, mlir::ShapedType::isDynamic);
  };

  for (unsigned i = 0; i < signature.num_operands(); ++i) {
    const Type* type = signature.operand(i);

    // Get the underlying value type from the async value.
    while (auto* value = dyn_cast<AsyncValueType>(type))
      type = &value->value_type();

    // Unranked types do not have statically known shape.
    if (isa<UnrankedTensorType, UnrankedMemrefType>(type)) return false;

    // For ranked memrefs and tensors check known sizes.
    if (auto* memref = dyn_cast<MemrefType>(type))
      if (is_dynamic(memref->sizes())) return false;
    if (auto* tensor = dyn_cast<RankedTensorType>(type))
      if (is_dynamic(tensor->sizes())) return false;

    // All other types are non-shaped and thus have "statically known shape".

    // TODO(ezhulenev): Run time types might need to support type interfaces or
    // a hierarchy with a base `ShapedType` so that users can define their own
    // types that can participate in shape specialization. This becomes
    // complicated for container-like types (e.g. async value) that might
    // contain a nested type that is shaped (e.g. memref). For now only the
    // canonical types can participate in shape specialization.
  }

  return true;
}

/*static*/ void JitExecutable::InlineCompilationTaskRunner(
    size_t num_specializations, ArrayRef<ArgumentConstraint> constraints,
    ArgumentsRef arguments, TaskFunction task, UserData user_data) {
  task();
}

/*static*/ Expected<JitExecutable> JitExecutable::Instantiate(
    string_view mlir_module, string_view entrypoint,
    CompilationOptions compilation_opts, string_view memory_region_name,
    CompilationTaskRunner runner) {
  // Set up LLVM target for code generation.
  InitializeCompiler();

  // Try to instantiate compilation context from the mlir source.
  Expected<std::unique_ptr<JitCompilationContext>> ctx =
      JitCompilationContext::Instantiate(compilation_opts, mlir_module,
                                         entrypoint);
  if (auto err = ctx.takeError()) return std::move(err);

  // Get resolved operands constraints for the entrypoint function.
  auto constraints = GetArgumentsConstraints((*ctx)->entrypoint());
  if (auto err = constraints.takeError()) return std::move(err);

  // Get the entrypoint function signature, it will be later required to
  // compute the specialized function signature from the operands at runtime.
  auto signature = compilation_opts.type_converter.Convert(
      (*ctx)->entrypoint().getFunctionType());
  if (auto err = signature.takeError()) return std::move(err);

  // If all of the operands have static shape, then we can always use default
  // binary for execution (unless specialization is explicitly required by the
  // operands constraints).
  if (HasStaticShapeOperands(*signature) && !IsSpecializationOnly(*constraints))
    compilation_opts.specialization = Specialization::kDisabled;

  // Return an error if specialization is explicitly disabled, yet some of
  // the operands have unresolved constraints.
  if (compilation_opts.specialization == Specialization::kDisabled &&
      IsSpecializationOnly(*constraints))
    return MakeStringError(
        "compilation options disabled specialization, yet operands have "
        "unresolved constraints: ",
        *constraints);

  // If the module must be specialized, return JitExecutable without a default
  // compiled executable.
  if (compilation_opts.specialization == Specialization::kAlways ||
      IsSpecializationOnly(*constraints))
    return JitExecutable(mlir_module, entrypoint, memory_region_name,
                         std::move(compilation_opts), std::move(*constraints),
                         std::move(*signature),
                         /*default_executable=*/llvm::None, std::move(runner));

  // Otherwise try to compile the default executable.
  Expected<Executable> executable =
      JitCompilationContext::Compile(std::move(*ctx), memory_region_name);
  if (auto err = executable.takeError()) return std::move(err);

  return JitExecutable(mlir_module, entrypoint, memory_region_name,
                       std::move(compilation_opts), std::move(*constraints),
                       std::move(*signature), std::move(*executable),
                       std::move(runner));
}

JitExecutable::JitExecutable(string_view mlir_module, string_view entrypoint,
                             string_view memory_region_name,
                             CompilationOptions compilation_opts,
                             ArrayRef<ArgumentConstraint> constraints,
                             FunctionType signature,
                             Optional<Executable> default_executable,
                             CompilationTaskRunner runner)
    : mlir_module_(mlir_module.str()),
      entrypoint_(entrypoint.str()),
      memory_region_name_(memory_region_name.str()),
      compilation_opts_(std::move(compilation_opts)),
      constraints_(constraints.begin(), constraints.end()),
      has_value_constraints_(HasValueConstraints(constraints_)),
      signature_(std::move(signature)),
      symbolic_shapes_resolver_(signature_, constraints_),
      has_default_executable_(default_executable.has_value()),
      runner_(std::move(runner)),
      specializations_(std::make_unique<Specializations>()) {
  // Initialize default executable if it is available.
  if (has_default_executable_) {
    default_executable_ =
        MakeAvailableAsyncValueRef<Executable>(std::move(*default_executable));
  } else {
    default_executable_ =
        MakeErrorAsyncValueRef("default executable is not available");
  }
}

AsyncValuePtr<Executable> JitExecutable::DefaultExecutable() const {
  return default_executable_.AsPtr();
}

ArrayRef<ArgumentConstraint> JitExecutable::constraints() const {
  return constraints_;
}

// Combines `hash` with a hash value computed from a value constrained operands.
static llvm::hash_code CombineWithValueConstraineOperands(
    llvm::hash_code hash, ArgumentsRef arguments,
    ArrayRef<ArgumentConstraint> constraints) {
  for (int i = 0; i < constraints.size(); ++i) {
    if (LLVM_LIKELY(constraints[i] != ArgumentConstraint::kValue)) continue;

    // TODO(ezhulenev): Currently we only support value specialization of Tensor
    // operands (wiht MemrefDesc run time argument), it should be extended to
    // support open type and argument hierarchies.
    const MemrefDesc& memref = cast<MemrefDesc>(arguments[i]);
    const auto* data = static_cast<uint8_t*>(memref.data());
    size_t rank = memref.rank();
    assert(rank == 0 || rank == 1);
    size_t num_values = rank == 0 ? 1 : memref.size(0);
    Index len = num_values * GetHostSize(memref.dtype());
    hash = llvm::hash_combine(hash, llvm::hash_combine_range(data, data + len));
  }
  return hash;
}

// TODO(ezhulenev): The fast path should be free of mutex to find the
// pre-compiled specialization. Maybe use atomic pointers (multiple atomic
// pointers?) to keep the most commonly used specialization available without
// doing a lookup in the AsyncValuesCache.
//
// TODO(ezhulenev): The number of specializations should be bounded, ideally we
// should only keep N most common specializations, and for everything else
// fall back on the default executable. However what to do if default executable
// is not available, and the number of specializations is above N?
Expected<AsyncValuePtr<Executable>> JitExecutable::GetExecutable(
    ArgumentsRef arguments, UserData user_data,
    const SpecializationListener* listener) {
  // Do not try to compile specialized executable if it is explicitly disabled.
  if (compilation_opts_.specialization == Specialization::kDisabled)
    return DefaultExecutable();

  // The number of arguments must match the entrypoint signature.
  if (LLVM_UNLIKELY(arguments.size() != signature_.num_operands()))
    return MakeStringError("expected ", signature_.num_operands(),
                           " arguments, got: ", arguments.size());

  // Resolve symbolic shapes hash based on the static and runtime information.
  //
  // We rely on the hash code to find the specialized executable. In case of
  // a collision (practically impossible) incompatible arguments will be
  // rejected by the executable arguments verification.
  ErrorOr<llvm::hash_code> hash =
      symbolic_shapes_resolver_.ResolveHash(arguments);

  // If we failed to resolve the symbolic shapes hash, then we need to verify
  // all the operands to find the mismatch and report it to the user.
  if (LLVM_UNLIKELY(hash.getError())) {
    for (unsigned i = 0; i < arguments.size(); ++i) {
      auto* type = signature_.operand(i);

      // TODO(ezhulenev): Support open shaped type/argument hierarchy.
      auto* memref_arg = dyn_cast<MemrefDesc>(&arguments[i]);
      if (!memref_arg) continue;

      if (auto* memref = dyn_cast<MemrefType>(type)) {
        if (auto err = VerifyMemrefArgument(i, *memref, *memref_arg))
          return std::move(err);

      } else if (auto* tensor = dyn_cast<RankedTensorType>(type)) {
        if (auto err = VerifyMemrefArgument(i, *tensor, *memref_arg))
          return std::move(err);

      } else {
        return MakeStringError("expected shaped operand at #", i,
                               ", got: ", *signature_.operand(i));
      }
    }

    assert(false && "failed to detect incorrect operand");
    return MakeStringError("failed to resolve symbolic shapes");
  }

  // Combine with a hash value computed from the value constrained operands.
  if (LLVM_UNLIKELY(has_value_constraints_))
    *hash = CombineWithValueConstraineOperands(*hash, arguments, constraints_);

  // Maybe return Executable from the cache.
  if (auto cached = specializations_->Find(*hash)) {
    // Always use specialized kernel if required by the compilation options.
    if (compilation_opts_.specialization == Specialization::kAlways)
      return cached;

    // Fall back on default executable if the specialization is not yet
    // available.
    if (has_default_executable_ && !cached.IsAvailable())
      return DefaultExecutable();

    return cached;
  }

  // Instantiation from the source and specialization are cheap, so we do it in
  // the caller thread. We only use compilation runner for expensive part.

  // Try to instantiate compilation context from the mlir source.
  Expected<std::unique_ptr<JitCompilationContext>> ctx =
      JitCompilationContext::Instantiate(compilation_opts_, mlir_module_,
                                         entrypoint_);

  if (auto err = ctx.takeError()) {
    assert(false && "parsing mlir module must always succeed at this point");
    return std::move(err);
  }

  // Specialize executable to the concrete operands.
  ErrorOr<llvm::SmallVector<SymbolicShape>> symbolic_shapes =
      symbolic_shapes_resolver_.Resolve(arguments);
  if (auto err = (*ctx)->Specialize(arguments, *symbolic_shapes, constraints_,
                                    listener)) {
    return MakeStringError("failed to specialize executable: ", err);
  }

  // Allocate a placeholder for the compiled specialization only after we are
  // ready to dispatch the compilation task.
  Specializations::Entry entry = specializations_->Allocate(*hash);

  // We lost the race; some other invocation will do the compilation.
  if (!entry.allocated) return entry.ptr;

  // Get the specialization id from the size of the specializations cache.
  size_t specialization = entry.size - 1;

  // Construct the task that will do the specialized executable compilation.
  auto compile = TaskFunction([ctx = std::move(*ctx), ref = entry.ptr.CopyRef(),
                               memory_region_name = memory_region_name_,
                               specialization]() mutable {
    Expected<Executable> executable = JitCompilationContext::Compile(
        std::move(ctx), memory_region_name, specialization);

    // Set the allocated entry async value state to error or concrete.
    if (auto err = executable.takeError()) {
      ref.SetError(std::move(err));
    } else {
      ref.emplace(std::move(*executable));
    }
  });

  // Offload specialization compilation to the user provided runner.
  runner_(specialization, constraints_, arguments, std::move(compile),
          user_data);

  // Use the default executable while we are compiling a specialized version if
  // this is not explicitly disabled by the compilation options.
  if (compilation_opts_.specialization == Specialization::kAlways)
    return entry.ptr;
  else
    return has_default_executable_ ? DefaultExecutable() : entry.ptr;
}

AsyncValueRef<Chain> JitExecutable::AllExecutablesCompiled() const {
  return specializations_->AllAvailable();
}

}  // namespace jitrt
}  // namespace tfrt
