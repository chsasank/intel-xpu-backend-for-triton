#include "mlir/Analysis/DataFlowFramework.h"
#include "mlir/Conversion/ArithToLLVM/ArithToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/GPUToGENX/GPUToGENXPass.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/LLVMCommon/VectorPattern.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Dialect/Index/IR/IndexDialect.h"
#include "mlir/Dialect/Index/IR/IndexOps.h"
#include "mlir/Dialect/LLVMIR/GENXDialect.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "triton/Analysis/Allocation.h"
#include "triton/Analysis/AxisInfo.h"
#include "triton/Analysis/Membar.h"
#include "triton/Conversion/TritonGPUToLLVM/Passes.h"
#include "triton/Dialect/NVGPU/IR/Dialect.h"
#include "triton/Dialect/Triton/IR/Dialect.h"
#include "triton/Dialect/TritonGPU/IR/Dialect.h"
#include "triton/Dialect/TritonNvidiaGPU/IR/Dialect.h"
#include "triton/Tools/Sys/GetPlatform.hpp"

#include "PatternTritonGPUOpToLLVM.h"

#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"

namespace mlir {
namespace triton {
#define GEN_PASS_DEF_CONVERTTRITONGPUTOLLVM
#include "triton/Conversion/TritonGPUToLLVM/Passes.h.inc"
} // namespace triton
} // namespace mlir

using namespace mlir;
using namespace mlir::triton;
namespace ttng = mlir::triton::nvidia_gpu;

namespace {

// pass ws related named attrs.
static void addWSNamedAttrs(Operation *op,
                            ArrayRef<mlir::NamedAttribute> attrs) {
  for (const NamedAttribute attr : attrs)
    if (attr.getName() == "async_agent" || attr.getName() == "agent.mutex_role")
      op->setAttr(attr.getName(), attr.getValue());
}

class TritonLLVMFunctionConversionTarget : public ConversionTarget {
public:
  explicit TritonLLVMFunctionConversionTarget(MLIRContext &ctx, Target target)
      : ConversionTarget(ctx) {
    addLegalDialect<index::IndexDialect>();
    addLegalDialect<LLVM::LLVMDialect>();
    switch (target) {
    case Target::NVVM:
      addLegalDialect<NVVM::NVVMDialect>();
      break;
    case Target::GENX:
      addLegalDialect<GENX::GENXDialect>();
      break;
    }
    addLegalOp<mlir::UnrealizedConversionCastOp>();
  }
};

class FoldSplatMaskInInsertAsync : public mlir::RewritePattern {

public:
  FoldSplatMaskInInsertAsync(mlir::MLIRContext *context)
      : mlir::RewritePattern(
            triton::nvidia_gpu::InsertSliceTMAOp::getOperationName(), 1,
            context) {}

  LogicalResult
  matchAndRewrite(mlir::Operation *op,
                  mlir::PatternRewriter &rewriter) const override {
    auto insertOp = cast<triton::nvidia_gpu::InsertSliceTMAOp>(op);
    if (!insertOp.getMask())
      return failure();
    auto splatOp = insertOp.getMask().getDefiningOp<triton::SplatOp>();
    if (!splatOp)
      return failure();
    rewriter.modifyOpInPlace(insertOp, [&]() {
      insertOp.getMaskMutable().assign(splatOp->getOperand(0));
    });
    return mlir::success();
  }
};

struct ReturnOpConversion : public ConvertOpToLLVMPattern<triton::ReturnOp> {
  using ConvertOpToLLVMPattern<triton::ReturnOp>::ConvertOpToLLVMPattern;

  LogicalResult
  matchAndRewrite(triton::ReturnOp op, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto funcOp = op->getParentOfType<LLVM::LLVMFuncOp>();
    if (funcOp->hasAttr("nvvm.kernel")) {
      // A GPU kernel
      if (op.getNumOperands() > 0) {
        return rewriter.notifyMatchFailure(
            op, "Kernel functions do not support return with operands");
      }
      rewriter.replaceOpWithNewOp<LLVM::ReturnOp>(op, TypeRange(), ValueRange(),
                                                  op->getAttrs());
    } else {
      // A device function
      LLVM::ReturnOp newOp;
      if (adaptor.getOperands().size() < 2) {
        // Single or no return value.
        newOp =
            rewriter.create<LLVM::ReturnOp>(op.getLoc(), adaptor.getOperands());
      } else {
        // Pack the results into a struct.
        auto packedResultsTy = this->getTypeConverter()->packFunctionResults(
            funcOp.getResultTypes());
        Value packedResults =
            rewriter.create<LLVM::UndefOp>(op.getLoc(), packedResultsTy);
        auto loc = op.getLoc();
        for (auto it : llvm::enumerate(adaptor.getOperands())) {
          packedResults = insert_val(packedResultsTy, packedResults, it.value(),
                                     it.index());
        }
        newOp = rewriter.create<LLVM::ReturnOp>(op.getLoc(), packedResults);
      }
      newOp->setAttrs(op->getAttrs());
      rewriter.replaceOp(op, newOp->getResults());
    }
    return success();
  }
};

/// FuncOp legalization pattern that converts MemRef arguments to pointers to
/// MemRef descriptors (LLVM struct data types) containing all the MemRef type
/// information.
struct FuncOpConversion : public FuncOpConversionBase {
  FuncOpConversion(LLVMTypeConverter &converter, int numWarps,
                   triton::Target target, PatternBenefit benefit)
      : FuncOpConversionBase(converter, benefit), numWarps(numWarps),
        target(target) {}

  triton::FuncOp amendFuncOp(triton::FuncOp funcOp,
                             ConversionPatternRewriter &rewriter) const {
    // Push back a variable that indicates the current stack pointer of shared
    // memory to the function arguments.
    auto loc = funcOp.getLoc();
    auto ctx = funcOp->getContext();
    auto ptrTy = LLVM::LLVMPointerType::get(rewriter.getContext(), 3);
    // 1. Modify the function type to add the new argument.
    auto funcTy = funcOp.getFunctionType();
    auto amendedInputTy = llvm::to_vector<4>(funcTy.getInputs());
    amendedInputTy.push_back(ptrTy);
    auto amendedFuncTy = FunctionType::get(funcTy.getContext(), amendedInputTy,
                                           funcTy.getResults());
    // 2. Modify the argument attributes to add the new argument.
    SmallVector<NamedAttribute> amendedAttrs;
    filterFuncAttributes(funcOp, /*filterArgAttrs=*/true, amendedAttrs);
    auto amendedArgAttrs = llvm::to_vector<4>(funcOp.getAllArgAttrs());
    amendedArgAttrs.emplace_back(DictionaryAttr::get(ctx));
    amendedAttrs.push_back(rewriter.getNamedAttr(
        funcOp.getArgAttrsAttrName(), rewriter.getArrayAttr(amendedArgAttrs)));
    // 3. Add a new argument to the region
    auto amendedFuncOp = rewriter.create<triton::FuncOp>(
        funcOp.getLoc(), funcOp.getName(), amendedFuncTy, amendedAttrs);
    auto &region = funcOp.getBody();
    region.addArgument(ptrTy, loc);
    rewriter.inlineRegionBefore(region, amendedFuncOp.getBody(),
                                amendedFuncOp.end());
    return amendedFuncOp;
  }

  LogicalResult
  matchAndRewrite(triton::FuncOp funcOp, OpAdaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    // Prevent LLVM's inliner to inline this function
    auto amendedFuncOp = funcOp;
    if (!LLVM::isKernel(funcOp))
      amendedFuncOp = amendFuncOp(funcOp, rewriter);

    auto newFuncOp = convertFuncOpToLLVMFuncOp(amendedFuncOp, rewriter);
    if (!newFuncOp) {
      return failure();
    }

    auto ctx = funcOp->getContext();
    switch (target) {
    case Target::NVVM:
    case Target::ROCDL:
      if (LLVM::isKernel(funcOp))
        // Set an attribute to indicate this function is a kernel entry.
        newFuncOp->setAttr("nvvm.kernel",
                           rewriter.getIntegerAttr(type::u1Ty(ctx), 1));

      // Set an attribute for maxntidx, it could be used in latter LLVM codegen
      // for `nvvm.annotation` metadata.
      newFuncOp->setAttr("nvvm.maxntid",
                         rewriter.getDenseI32ArrayAttr(32 * numWarps));
      break;
    case Target::GENX:
      NamedAttrList attrs;
      auto mod = funcOp->getParentOfType<ModuleOp>();
      int threadsPerWarp =
          triton::gpu::TritonGPUDialect::getThreadsPerWarp(mod);
      if (LLVM::isKernel(funcOp))
        attrs.append(GENX::GENXDialect::getKernelFuncAttrName(),
                     rewriter.getI32IntegerAttr(1));
      attrs.append(GENX::GENXDialect::getMaxWorkGroupSizeAttrName(),
                   rewriter.getI32ArrayAttr({threadsPerWarp * numWarps, 1, 1}));
      attrs.append(GENX::GENXDialect::getReqdSubGroupSizeAttrName(),
                   rewriter.getI32ArrayAttr(threadsPerWarp));
      newFuncOp->setDialectAttrs(attrs);
      break;
    }
    if (!LLVM::isKernel(funcOp)) {
      // The noinline attribute will be used by the LLVM codegen to prevent
      // inlining.
      // https://github.com/llvm/llvm-project/blob/main/mlir/lib/Dialect/LLVMIR/IR/LLVMInlining.cpp#L267
      newFuncOp.setPassthroughAttr(
          ArrayAttr::get(ctx, rewriter.getStringAttr("noinline")));
      rewriter.eraseOp(amendedFuncOp);
    }

    // required by AxisInfoAnalysis
    rewriter.eraseOp(funcOp);
    return success();
  }

private:
  int numWarps{0};
  triton::Target target;
};

// CallOpInterfaceLowering is adapted from
// https://github.com/llvm/llvm-project/blob/fae656b2dd80246c3c6f01e9c77c49560368752c/mlir/lib/Conversion/FuncToLLVM/FuncToLLVM.cpp#L485
struct CallOpConversion : public ConvertOpToLLVMPattern<triton::CallOp> {
  CallOpConversion(LLVMTypeConverter &converter, int numWarps,
                   PatternBenefit benefit, Target target)
      : ConvertOpToLLVMPattern<triton::CallOp>(converter, benefit),
        numWarps(numWarps), target(target) {}

  LogicalResult
  matchAndRewrite(triton::CallOp callOp,
                  typename triton::CallOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const override {
    auto promotedOperands = promoteOperands(callOp, adaptor, rewriter);
    auto newCallOp =
        convertCallOpToLLVMCallOp(callOp, promotedOperands, rewriter);
    if (!newCallOp)
      return failure();
    auto results = getCallOpResults(callOp, newCallOp, rewriter);
    rewriter.replaceOp(callOp, results);
    return success();
  }

private:
  SmallVector<Value, 4>
  promoteOperands(triton::CallOp callOp,
                  typename triton::CallOp::Adaptor adaptor,
                  ConversionPatternRewriter &rewriter) const {
    // Get the last argument of the caller, which is the current stack pointer
    // of shared memory and append it to the operands of the callOp.
    auto loc = callOp.getLoc();
    auto caller = callOp->getParentOfType<FunctionOpInterface>();
    auto promotedOperands = this->getTypeConverter()->promoteOperands(
        callOp.getLoc(), /*opOperands=*/callOp->getOperands(),
        adaptor.getOperands(), rewriter);
    if (!caller->hasAttr("allocation.offset")) {
      auto base = LLVM::getStackPointer(rewriter, caller, target);
      promotedOperands.push_back(base);
      return promotedOperands;
    }
    promotedOperands.push_back(
        LLVM::getSharedMemoryBase(callOp->getLoc(), rewriter, callOp, target));
    return promotedOperands;
  }

  LLVM::CallOp
  convertCallOpToLLVMCallOp(triton::CallOp callOp,
                            ArrayRef<Value> promotedOperands,
                            ConversionPatternRewriter &rewriter) const {
    // Pack the result types into a struct.
    Type packedResult = nullptr;
    unsigned numResults = callOp.getNumResults();
    auto resultTypes = llvm::to_vector<4>(callOp.getResultTypes());

    if (numResults != 0) {
      if (!(packedResult =
                this->getTypeConverter()->packFunctionResults(resultTypes)))
        return nullptr;
    }
    auto newCallOp = rewriter.create<LLVM::CallOp>(
        callOp.getLoc(), packedResult ? TypeRange(packedResult) : TypeRange(),
        promotedOperands, callOp->getAttrs());
    return newCallOp;
  }

  SmallVector<Value>
  getCallOpResults(triton::CallOp callOp, LLVM::CallOp newCallOp,
                   ConversionPatternRewriter &rewriter) const {
    auto numResults = callOp.getNumResults();
    SmallVector<Value> results;
    if (numResults < 2) {
      // If < 2 results, packing did not do anything and we can just return.
      results.append(newCallOp.result_begin(), newCallOp.result_end());
    } else {
      // Otherwise, it had been converted to an operation producing a structure.
      // Extract individual results from the structure and return them as list.
      results.reserve(numResults);
      for (unsigned i = 0; i < numResults; ++i) {
        results.push_back(rewriter.create<LLVM::ExtractValueOp>(
            callOp.getLoc(), newCallOp->getResult(0), i));
      }
    }
    return results;
  }

  int numWarps{0};
  Target target;
};

class TritonLLVMConversionTarget : public ConversionTarget {
public:
  explicit TritonLLVMConversionTarget(MLIRContext &ctx, Target target)
      : ConversionTarget(ctx) {
    addLegalDialect<LLVM::LLVMDialect>();
    switch (target) {
    case Target::NVVM:
      addLegalDialect<NVVM::NVVMDialect>();
      addLegalDialect<mlir::triton::nvgpu::NVGPUDialect>();
      break;
    case Target::GENX:
      addLegalDialect<GENX::GENXDialect>();
      break;
    }
    addIllegalDialect<triton::TritonDialect>();
    addIllegalDialect<triton::gpu::TritonGPUDialect>();
    addIllegalDialect<triton::nvidia_gpu::TritonNvidiaGPUDialect>();
    addIllegalDialect<mlir::gpu::GPUDialect>();
    addLegalOp<mlir::UnrealizedConversionCastOp>();
  }
};

struct ConvertTritonGPUToLLVM
    : public triton::impl::ConvertTritonGPUToLLVMBase<ConvertTritonGPUToLLVM> {
  using ConvertTritonGPUToLLVMBase::ConvertTritonGPUToLLVMBase;

  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<triton::nvgpu::NVGPUDialect, LLVM::LLVMDialect,
                    NVVM::NVVMDialect, GENX::GENXDialect>();
  }

  ConvertTritonGPUToLLVM(int32_t computeCapability, Target target,
                         mlir::triton::gpu::TMAMetadataTy *tmaMetadata)
      : ConvertTritonGPUToLLVMBase({computeCapability, target}),
        tmaMetadata(tmaMetadata) {}

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    ModuleOp mod = getOperation();

    mlir::LowerToLLVMOptions option(context);
    option.overrideIndexBitwidth(32);
    TritonGPUToLLVMTypeConverter typeConverter(context, option);
    TritonLLVMConversionTarget convTarget(*context, target);
    int numWarps = triton::gpu::TritonGPUDialect::getNumWarps(mod);
    int numCTAs = triton::gpu::TritonGPUDialect::getNumCTAs(mod);
    int threadsPerWarp = triton::gpu::TritonGPUDialect::getThreadsPerWarp(mod);

    // Hack: WSMaterialization may have changed the effective number of warps,
    // in a way that isn't reflected in triton_gpu.num-warps.  If so, we have to
    // respect that here.
    if (Attribute attr = mod->getAttr("triton_gpu.num-warp-groups-per-cta")) {
      numWarps *= attr.cast<IntegerAttr>().getInt();
    }

    // Preprocess
    decomposeInsertSliceAsyncOp(mod);

    // Allocate shared memory and set barrier
    ModuleAllocation allocation(mod);
    ModuleMembarAnalysis membarPass(&allocation);
    membarPass.run();

    /* Get tensorPtrMap before conversion */
    TensorPtrMapT tensorPtrMap;
    mod.walk(
        [&tensorPtrMap](mlir::triton::nvidia_gpu::InsertSliceTMAOp insertOp) {
          auto src = insertOp.getSrc();
          auto ptrTy = src.getType().dyn_cast<triton::PointerType>();
          if (ptrTy && ptrTy.getPointeeType().isa<RankedTensorType>()) {
            auto makeTensorPtrOp = getMakeTensorPtrOp(insertOp.getSrc());
            tensorPtrMap[insertOp.getOperation()] = makeTensorPtrOp;
          }
        });

    mod.walk(
        [&tensorPtrMap](mlir::triton::nvidia_gpu::StoreAsyncTMAOp storeOp) {
          auto dst = storeOp.getDst();
          auto ptrTy = dst.getType().dyn_cast<triton::PointerType>();
          if (ptrTy && ptrTy.getPointeeType().isa<RankedTensorType>()) {
            auto makeTensorPtrOp = getMakeTensorPtrOp(storeOp.getDst());
            tensorPtrMap[storeOp.getOperation()] = makeTensorPtrOp;
          }
        });

    // Hack: cleanup
    {
      RewritePatternSet patterns(context);
      patterns.add<FoldSplatMaskInInsertAsync>(context);
      SmallVector<Operation *> insertSlices;
      mod.walk([&insertSlices](triton::nvidia_gpu::InsertSliceTMAOp op) {
        insertSlices.push_back(op);
      });
      if (applyOpPatternsAndFold(insertSlices, std::move(patterns)).failed())
        signalPassFailure();
    }

    // Lower functions
    {
      mlir::LowerToLLVMOptions option(context);
      TritonGPUToLLVMTypeConverter typeConverter(context, option);
      TritonLLVMFunctionConversionTarget funcTarget(*context, target);
      RewritePatternSet funcPatterns(context);
      funcPatterns.add<FuncOpConversion>(typeConverter, numWarps, target,
                                         /*benefit=*/1);
      mlir::cf::populateControlFlowToLLVMConversionPatterns(typeConverter,
                                                            funcPatterns);
      if (failed(
              applyPartialConversion(mod, funcTarget, std::move(funcPatterns))))
        return signalPassFailure();
    }

    // initSharedMemory is run before the conversion of call and ret ops,
    // because the call op has to know the shared memory base address of each
    // function
    initSharedMemory(typeConverter, target);

    // Convert call and ret ops
    {
      mlir::LowerToLLVMOptions option(context);
      TritonGPUToLLVMTypeConverter typeConverter(context, option);
      TritonLLVMFunctionConversionTarget funcTarget(*context, target);
      RewritePatternSet funcPatterns(context);
      funcPatterns.add<CallOpConversion>(typeConverter, numWarps, 1, target);
      funcPatterns.add<ReturnOpConversion>(typeConverter, 1);
      if (failed(
              applyPartialConversion(mod, funcTarget, std::move(funcPatterns))))
        return signalPassFailure();
    }

    ModuleAxisInfoAnalysis axisInfoAnalysis(mod);

    // Emit logics to get threadId/blockIds/linearized clusterCTAId etc. and
    // cache the values. The reason to do it here is that cluster_ctaid is
    // currently implemented via inline asm, and thus cannot be CSEed.
    // clusterCTAId will be emitted only when numCTAs is larger than 1, and
    // other values will be DCEed if not used hereafter.
    bool isWarpSpecialization =
        ttng::TritonNvidiaGPUDialect::getWSSupportedAttr(mod);
    OpBuilder::InsertPoint indexInsertPoint;

    // tmaMetadata is absent in a triton-opt unit test, in this case, create a
    // local one and dump it after this pass is done.
    mlir::triton::gpu::TMAMetadataTy tmaMetaDataDebug;
    if (tmaMetadata == nullptr)
      tmaMetadata = &tmaMetaDataDebug;

    RewritePatternSet patterns(context);

    auto populatePatterns1 = [&](auto populateFunc) {
      populateFunc(typeConverter, patterns, numWarps, axisInfoAnalysis, target,
                   /*benefit*/ 10);
    };

    auto populatePatterns2 = [&](auto populateFunc) {
      populateFunc(typeConverter, patterns, numWarps, axisInfoAnalysis, target,
                   /*benefit*/ 10);
    };

    auto populatePatterns3 = [&](auto populateFunc) {
      populateFunc(typeConverter, patterns, numWarps, axisInfoAnalysis,
                   tmaMetadata, &tensorPtrMap, target,
                   /*benefit*/ 10);
    };

    auto populatePatterns4 = [&](auto populateFunc) {
      populateFunc(typeConverter, patterns, numWarps, axisInfoAnalysis,
                   computeCapability, target,
                   /*benefit*/ 10);
    };

    populatePatterns1(populateTritonGPUToLLVMPatterns);
    populatePatterns1(populateConvertLayoutOpToLLVMPatterns);
    populatePatterns2(populateDotOpToLLVMPatterns);
    populatePatterns4(populateElementwiseOpToLLVMPatterns);
    populatePatterns3(populateLoadStoreOpToLLVMPatterns);
    populatePatterns4(populateReduceOpToLLVMPatterns);
    populatePatterns1(populateScanOpToLLVMPatterns);
    populatePatterns2(populateViewOpToLLVMPatterns);
    populatePatterns2(populateBarrierOpToLLVMPatterns);
    populatePatterns2(populateTensorPtrOpsToLLVMPatterns);
    populatePatterns2(populateClusterOpsToLLVMPatterns);
    populatePatterns2(populateRegReallocOpToLLVMPatterns);
    populatePatterns1(populateHistogramOpToLLVMPatterns);

    // TODO(thomas): this should probably be done in a separate step to not
    // interfere with our own lowering of arith ops. Add arith/math's patterns
    // to help convert scalar expression to LLVM.
    mlir::arith::populateArithToLLVMConversionPatterns(typeConverter, patterns);
    mlir::populateMathToLLVMConversionPatterns(typeConverter, patterns);

    // Native lowering patterns
    switch (target) {
    case Target::NVVM:
      mlir::populateGpuToNVVMConversionPatterns(typeConverter, patterns);
      break;
    case Target::GENX:
      mlir::populateGpuToGENXConversionPatterns(typeConverter, patterns);
      break;
    }

    mlir::cf::populateControlFlowToLLVMConversionPatterns(typeConverter,
                                                          patterns);
    if (failed(applyPartialConversion(mod, convTarget, std::move(patterns))))
      return signalPassFailure();

    // Fold CTAId when there is only 1 CTA.
    if (numCTAs == 1) {
      mod.walk([](triton::nvgpu::ClusterCTAIdOp id) {
        OpBuilder b(id);
        Value zero = LLVM::createConstantI32(id->getLoc(), b, 0);
        id.replaceAllUsesWith(zero);
      });
    }
  }

private:
  mlir::triton::gpu::TMAMetadataTy *tmaMetadata = nullptr;

  void initSharedMemory(TritonGPUToLLVMTypeConverter &typeConverter,
                        Target target) {
    ModuleOp mod = getOperation();
    OpBuilder b(mod.getBodyRegion());
    auto ctx = mod.getContext();
    auto loc = mod.getLoc();
    auto elemTy = typeConverter.convertType(b.getIntegerType(8));
    switch (target) {
    case Target::NVVM:
    case Target::ROCDL: {
      // Set array size 0 and external linkage indicates that we use dynamic
      // shared allocation to allow a larger shared memory size for each kernel.
      //
      // Ask for 16B alignment on global_smem because that's the largest we
      // should ever need (4xi32).
      auto arrayTy = LLVM::LLVMArrayType::get(elemTy, 0);
      auto global = b.create<LLVM::GlobalOp>(
          loc, arrayTy, /*isConstant=*/false, LLVM::Linkage::External,
          "global_smem", /*value=*/Attribute(), /*alignment=*/16,
          // Add ROCm support.
          static_cast<unsigned>(NVVM::NVVMMemorySpace::kSharedMemorySpace));
    } break;
    case Target::GENX: {
    } break;
    }
  }

  void decomposeInsertSliceAsyncOp(ModuleOp mod) const {

    // The function has been deprecated upstream but is required to work on
    // genx. The current rewrite pattern for InsertSliceAsync generates PTX and
    // there is no matching instruciton on genx at the moment.
    // FIXME: remove this function once a suitable replacement is available.
    if (target != triton::Target::GENX)
      return;

    ModuleAxisInfoAnalysis axisInfoAnalysis(mod);
    // TODO(Keren): This is a hacky knob that may cause performance regression
    // when decomposition has been performed. We should remove this knob once we
    // have thorough analysis on async wait. Currently, we decompose
    // `insert_slice_async` into `load` and `insert_slice` without knowing which
    // `async_wait` is responsible for the `insert_slice_async`. To guarantee
    // correctness, we blindly set the `async_wait` to wait for all async ops.
    //
    // There are two options to improve this:
    // 1. We can perform a dataflow analysis to find the `async_wait` that is
    // responsible for the `insert_slice_async` in the backend.
    // 2. We can modify the pipeline to perform the decomposition before the
    // `async_wait` is inserted. However, it is also risky because we don't know
    // the correct vectorized shape yet in the pipeline pass. Making the
    // pipeline pass aware of the vectorization could introduce additional
    // dependencies on the AxisInfoAnalysis and the Coalesce analysis.
    bool decomposed = false;
    // insert_slice_async %src, %dst, %idx, %mask, %other
    // =>
    // %tmp = load %src, %mask, %other
    // %res = insert_slice %tmp into %dst[%idx]
    mod.walk([&](triton::gpu::InsertSliceAsyncOp insertSliceAsyncOp) -> void {
      OpBuilder builder(insertSliceAsyncOp);

      // Get the vectorized load size
      auto src = insertSliceAsyncOp.getSrc();
      auto dst = insertSliceAsyncOp.getDst();
      auto mask = insertSliceAsyncOp.getMask();
      auto srcTy = src.getType().cast<RankedTensorType>();
      auto dstTy = dst.getType().cast<RankedTensorType>();
      auto srcBlocked =
          srcTy.getEncoding().dyn_cast<triton::gpu::BlockedEncodingAttr>();
      auto resSharedLayout =
          dstTy.getEncoding().dyn_cast<triton::gpu::SharedEncodingAttr>();
      auto resElemTy = dstTy.getElementType();
      unsigned inVec = axisInfoAnalysis.getPtrContiguity(src);
      if (mask)
        inVec =
            std::min<unsigned>(axisInfoAnalysis.getMaskAlignment(mask), inVec);
      unsigned outVec = resSharedLayout.getVec();
      unsigned minVec = inVec;
      if (outVec > 1)
        minVec = std::min(outVec, inVec);
      auto maxBitWidth =
          std::max<unsigned>(128, resElemTy.getIntOrFloatBitWidth());
      auto vecBitWidth = resElemTy.getIntOrFloatBitWidth() * minVec;
      auto bitWidth = std::min<unsigned>(maxBitWidth, vecBitWidth);
      auto byteWidth = bitWidth / 8;

      // If the load byte width is not eligible or the current compute
      // capability does not support async copy, then we do decompose
      if (triton::gpu::InsertSliceAsyncOp::getEligibleLoadByteWidth(
              computeCapability)
              .contains(byteWidth)) {
        return;
      }

      // load
      auto tmpTy =
          RankedTensorType::get(srcTy.getShape(), resElemTy, srcBlocked);
      auto loadOp = builder.create<triton::LoadOp>(
          insertSliceAsyncOp.getLoc(), tmpTy, insertSliceAsyncOp.getSrc(),
          insertSliceAsyncOp.getMask(), insertSliceAsyncOp.getOther(),
          // TODO(Chenggang): confirm `boundaryCheck` and `padding`
          /*boundaryCheck=*/nullptr, /*padding=*/nullptr,
          insertSliceAsyncOp.getCache(), insertSliceAsyncOp.getEvict(),
          insertSliceAsyncOp.getIsVolatile());
      addWSNamedAttrs(loadOp, insertSliceAsyncOp->getAttrs());

      // insert_slice
      auto axis = insertSliceAsyncOp.getAxis();
      auto intAttr = [&](int64_t v) { return builder.getI64IntegerAttr(v); };
      auto offsets = SmallVector<OpFoldResult>(dstTy.getRank(), intAttr(0));
      auto sizes = SmallVector<OpFoldResult>(dstTy.getRank(), intAttr(1));
      auto strides = SmallVector<OpFoldResult>(dstTy.getRank(), intAttr(1));
      offsets[axis] = insertSliceAsyncOp.getIndex();
      for (size_t i = 0; i < dstTy.getRank(); i++) {
        if (i != axis)
          sizes[i] = intAttr(dstTy.getShape()[i]);
      }
      auto insertSliceOp = builder.create<tensor::InsertSliceOp>(
          insertSliceAsyncOp.getLoc(), loadOp, insertSliceAsyncOp.getDst(),
          offsets, sizes, strides);
      addWSNamedAttrs(insertSliceOp, insertSliceAsyncOp->getAttrs());

      // Replace
      insertSliceAsyncOp.replaceAllUsesWith(insertSliceOp.getResult());
      insertSliceAsyncOp.erase();
      decomposed = true;
    });

    mod.walk([&](triton::gpu::AsyncCommitGroupOp asyncCommitGroupOp) -> void {
      if (!triton::gpu::AsyncCommitGroupOp::isSupported(computeCapability))
        asyncCommitGroupOp.erase();
    });

    mod.walk([&](triton::gpu::AsyncWaitOp asyncWaitOp) -> void {
      if (!triton::gpu::AsyncWaitOp::isSupported(computeCapability)) {
        // async wait is supported in Ampere and later
        asyncWaitOp.erase();
      } else if (decomposed) {
        // Wait for all previous async ops
        OpBuilder builder(asyncWaitOp);
        auto newWaitOp =
            builder.create<triton::gpu::AsyncWaitOp>(asyncWaitOp.getLoc(), 0);
        addWSNamedAttrs(newWaitOp, asyncWaitOp->getAttrs());
        asyncWaitOp.erase();
      }
    });
  }
};

} // anonymous namespace

namespace mlir {
namespace triton {

std::unique_ptr<OperationPass<ModuleOp>> createConvertTritonGPUToLLVMPass() {
  return std::make_unique<ConvertTritonGPUToLLVM>();
}
std::unique_ptr<OperationPass<ModuleOp>> createConvertTritonGPUToLLVMPass(
    int32_t computeCapability, Target target,
    mlir::triton::gpu::TMAMetadataTy *tmaMetadata) {
  return std::make_unique<ConvertTritonGPUToLLVM>(computeCapability, target,
                                                  tmaMetadata);
}

} // namespace triton
} // namespace mlir
