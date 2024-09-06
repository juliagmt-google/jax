/* Copyright 2024 The JAX Authors.

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

#include "jaxlib/gpu/solver_kernels_ffi.h"

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "jaxlib/ffi_helpers.h"
#include "jaxlib/gpu/blas_handle_pool.h"
#include "jaxlib/gpu/gpu_kernel_helpers.h"
#include "jaxlib/gpu/make_batch_pointers.h"
#include "jaxlib/gpu/solver_handle_pool.h"
#include "jaxlib/gpu/vendor.h"
#include "xla/ffi/api/ffi.h"

namespace jax {
namespace JAX_GPU_NAMESPACE {

namespace ffi = ::xla::ffi;

namespace {
template <typename T>
inline absl::StatusOr<T*> AllocateWorkspace(ffi::ScratchAllocator& scratch,
                                            int64_t size,
                                            std::string_view name) {
  auto maybe_workspace = scratch.Allocate(sizeof(T) * size);
  if (!maybe_workspace.has_value()) {
    return absl::Status(
        absl::StatusCode::kResourceExhausted,
        absl::StrFormat("Unable to allocate workspace for %s", name));
  }
  return static_cast<T*>(maybe_workspace.value());
}
}  // namespace

#define SOLVER_DISPATCH_IMPL(impl, ...)         \
  if (dataType == ffi::F32) {                   \
    return impl<float>(__VA_ARGS__);            \
  } else if (dataType == ffi::F64) {            \
    return impl<double>(__VA_ARGS__);           \
  } else if (dataType == ffi::C64) {            \
    return impl<gpuComplex>(__VA_ARGS__);       \
  } else if (dataType == ffi::C128) {           \
    return impl<gpuDoubleComplex>(__VA_ARGS__); \
  }

#define SOLVER_BLAS_DISPATCH_IMPL(impl, ...)        \
  if (dataType == ffi::F32) {                       \
    return impl<float>(__VA_ARGS__);                \
  } else if (dataType == ffi::F64) {                \
    return impl<double>(__VA_ARGS__);               \
  } else if (dataType == ffi::C64) {                \
    return impl<gpublasComplex>(__VA_ARGS__);       \
  } else if (dataType == ffi::C128) {               \
    return impl<gpublasDoubleComplex>(__VA_ARGS__); \
  }

// LU decomposition: getrf

namespace {
#define GETRF_KERNEL_IMPL(type, name)                                          \
  template <>                                                                  \
  struct GetrfKernel<type> {                                                   \
    static absl::StatusOr<int> BufferSize(gpusolverDnHandle_t handle, int m,   \
                                          int n) {                             \
      int lwork;                                                               \
      JAX_RETURN_IF_ERROR(JAX_AS_STATUS(                                       \
          name##_bufferSize(handle, m, n, /*A=*/nullptr, /*lda=*/m, &lwork))); \
      return lwork;                                                            \
    }                                                                          \
    static absl::Status Run(gpusolverDnHandle_t handle, int m, int n, type* a, \
                            type* workspace, int lwork, int* ipiv,             \
                            int* info) {                                       \
      return JAX_AS_STATUS(                                                    \
          name(handle, m, n, a, m, workspace, lwork, ipiv, info));             \
    }                                                                          \
  }

template <typename T>
struct GetrfKernel;
GETRF_KERNEL_IMPL(float, gpusolverDnSgetrf);
GETRF_KERNEL_IMPL(double, gpusolverDnDgetrf);
GETRF_KERNEL_IMPL(gpuComplex, gpusolverDnCgetrf);
GETRF_KERNEL_IMPL(gpuDoubleComplex, gpusolverDnZgetrf);
#undef GETRF_KERNEL_IMPL

template <typename T>
ffi::Error GetrfImpl(int64_t batch, int64_t rows, int64_t cols,
                     gpuStream_t stream, ffi::ScratchAllocator& scratch,
                     ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                     ffi::Result<ffi::Buffer<ffi::S32>> ipiv,
                     ffi::Result<ffi::Buffer<ffi::S32>> info) {
  FFI_ASSIGN_OR_RETURN(auto m, MaybeCastNoOverflow<int>(rows));
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));

  FFI_ASSIGN_OR_RETURN(auto handle, SolverHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(int lwork,
                       GetrfKernel<T>::BufferSize(handle.get(), m, n));
  FFI_ASSIGN_OR_RETURN(auto workspace,
                       AllocateWorkspace<T>(scratch, lwork, "getrf"));

  auto a_data = static_cast<T*>(a.untyped_data());
  auto out_data = static_cast<T*>(out->untyped_data());
  auto ipiv_data = ipiv->typed_data();
  auto info_data = info->typed_data();
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        out_data, a_data, a.size_bytes(), gpuMemcpyDeviceToDevice, stream)));
  }

  int ipiv_step = std::min(m, n);
  for (auto i = 0; i < batch; ++i) {
    FFI_RETURN_IF_ERROR_STATUS(GetrfKernel<T>::Run(
        handle.get(), m, n, out_data, workspace, lwork, ipiv_data, info_data));
    out_data += m * n;
    ipiv_data += ipiv_step;
    ++info_data;
  }
  return ffi::Error::Success();
}

#define GETRF_BATCHED_KERNEL_IMPL(type, name)                                 \
  template <>                                                                 \
  struct GetrfBatchedKernel<type> {                                           \
    static absl::Status Run(gpublasHandle_t handle, int n, type** a, int lda, \
                            int* ipiv, int* info, int batch) {                \
      return JAX_AS_STATUS(name(handle, n, a, lda, ipiv, info, batch));       \
    }                                                                         \
  }

template <typename T>
struct GetrfBatchedKernel;
GETRF_BATCHED_KERNEL_IMPL(float, gpublasSgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(double, gpublasDgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(gpublasComplex, gpublasCgetrfBatched);
GETRF_BATCHED_KERNEL_IMPL(gpublasDoubleComplex, gpublasZgetrfBatched);
#undef GETRF_BATCHED_KERNEL_IMPL

template <typename T>
ffi::Error GetrfBatchedImpl(int64_t batch, int64_t cols, gpuStream_t stream,
                            ffi::ScratchAllocator& scratch, ffi::AnyBuffer a,
                            ffi::Result<ffi::AnyBuffer> out,
                            ffi::Result<ffi::Buffer<ffi::S32>> ipiv,
                            ffi::Result<ffi::Buffer<ffi::S32>> info) {
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));
  FFI_ASSIGN_OR_RETURN(auto handle, BlasHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(auto batch_ptrs,
                       AllocateWorkspace<T*>(scratch, batch, "batched getrf"));

  auto a_data = a.untyped_data();
  auto out_data = out->untyped_data();
  auto ipiv_data = ipiv->typed_data();
  auto info_data = info->typed_data();
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        out_data, a_data, a.size_bytes(), gpuMemcpyDeviceToDevice, stream)));
  }

  MakeBatchPointersAsync(stream, out_data, batch_ptrs, batch,
                         sizeof(T) * n * n);
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuGetLastError()));

  FFI_RETURN_IF_ERROR_STATUS(GetrfBatchedKernel<T>::Run(
      handle.get(), n, batch_ptrs, n, ipiv_data, info_data, batch));

  return ffi::Error::Success();
}

ffi::Error GetrfDispatch(gpuStream_t stream, ffi::ScratchAllocator scratch,
                         ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                         ffi::Result<ffi::Buffer<ffi::S32>> ipiv,
                         ffi::Result<ffi::Buffer<ffi::S32>> info) {
  auto dataType = a.element_type();
  if (dataType != out->element_type()) {
    return ffi::Error::InvalidArgument(
        "The input and output to getrf must have the same element type");
  }
  FFI_ASSIGN_OR_RETURN((auto [batch, rows, cols]),
                       SplitBatch2D(a.dimensions()));
  FFI_RETURN_IF_ERROR(
      CheckShape(out->dimensions(), {batch, rows, cols}, "out", "getrf"));
  FFI_RETURN_IF_ERROR(CheckShape(
      ipiv->dimensions(), {batch, std::min(rows, cols)}, "ipiv", "getrf"));
  FFI_RETURN_IF_ERROR(CheckShape(info->dimensions(), batch, "info", "getrf"));
  if (batch > 1 && rows == cols && rows / batch <= 128) {
    SOLVER_BLAS_DISPATCH_IMPL(GetrfBatchedImpl, batch, cols, stream, scratch, a,
                              out, ipiv, info);
  } else {
    SOLVER_DISPATCH_IMPL(GetrfImpl, batch, rows, cols, stream, scratch, a, out,
                         ipiv, info);
  }
  return ffi::Error::InvalidArgument("Unsupported element type for getrf");
}
}  // namespace

XLA_FFI_DEFINE_HANDLER_SYMBOL(GetrfFfi, GetrfDispatch,
                              ffi::Ffi::Bind()
                                  .Ctx<ffi::PlatformStream<gpuStream_t>>()
                                  .Ctx<ffi::ScratchAllocator>()
                                  .Arg<ffi::AnyBuffer>()         // a
                                  .Ret<ffi::AnyBuffer>()         // out
                                  .Ret<ffi::Buffer<ffi::S32>>()  // ipiv
                                  .Ret<ffi::Buffer<ffi::S32>>()  // info
);

// QR decomposition: geqrf

namespace {
#define GEQRF_KERNEL_IMPL(type, name)                                          \
  template <>                                                                  \
  struct GeqrfKernel<type> {                                                   \
    static absl::StatusOr<int> BufferSize(gpusolverDnHandle_t handle, int m,   \
                                          int n) {                             \
      int lwork;                                                               \
      JAX_RETURN_IF_ERROR(JAX_AS_STATUS(                                       \
          name##_bufferSize(handle, m, n, /*A=*/nullptr, /*lda=*/m, &lwork))); \
      return lwork;                                                            \
    }                                                                          \
    static absl::Status Run(gpusolverDnHandle_t handle, int m, int n, type* a, \
                            type* tau, type* workspace, int lwork,             \
                            int* info) {                                       \
      return JAX_AS_STATUS(                                                    \
          name(handle, m, n, a, m, tau, workspace, lwork, info));              \
    }                                                                          \
  }

template <typename T>
struct GeqrfKernel;
GEQRF_KERNEL_IMPL(float, gpusolverDnSgeqrf);
GEQRF_KERNEL_IMPL(double, gpusolverDnDgeqrf);
GEQRF_KERNEL_IMPL(gpuComplex, gpusolverDnCgeqrf);
GEQRF_KERNEL_IMPL(gpuDoubleComplex, gpusolverDnZgeqrf);
#undef GEQRF_KERNEL_IMPL

template <typename T>
ffi::Error GeqrfImpl(int64_t batch, int64_t rows, int64_t cols,
                     gpuStream_t stream, ffi::ScratchAllocator& scratch,
                     ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                     ffi::Result<ffi::AnyBuffer> tau) {
  FFI_ASSIGN_OR_RETURN(auto m, MaybeCastNoOverflow<int>(rows));
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));

  FFI_ASSIGN_OR_RETURN(auto handle, SolverHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(int lwork,
                       GeqrfKernel<T>::BufferSize(handle.get(), m, n));

  FFI_ASSIGN_OR_RETURN(auto workspace,
                       AllocateWorkspace<T>(scratch, lwork, "geqrf"));
  // Note: We ignore the returned value of info because it is only used for
  // shape checking (which we already do ourselves), but it is expected to be
  // in device memory, so we need to allocate it.
  FFI_ASSIGN_OR_RETURN(auto info, AllocateWorkspace<int>(scratch, 1, "geqrf"));

  auto a_data = static_cast<T*>(a.untyped_data());
  auto out_data = static_cast<T*>(out->untyped_data());
  auto tau_data = static_cast<T*>(tau->untyped_data());
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        out_data, a_data, a.size_bytes(), gpuMemcpyDeviceToDevice, stream)));
  }

  int out_step = m * n;
  int tau_step = std::min(m, n);
  for (auto i = 0; i < batch; ++i) {
    FFI_RETURN_IF_ERROR_STATUS(GeqrfKernel<T>::Run(
        handle.get(), m, n, out_data, tau_data, workspace, lwork, info));
    out_data += out_step;
    tau_data += tau_step;
  }
  return ffi::Error::Success();
}

#define GEQRF_BATCHED_KERNEL_IMPL(type, name)                               \
  template <>                                                               \
  struct GeqrfBatchedKernel<type> {                                         \
    static absl::Status Run(gpublasHandle_t handle, int m, int n, type** a, \
                            type** tau, int* info, int batch) {             \
      return JAX_AS_STATUS(name(handle, m, n, a, m, tau, info, batch));     \
    }                                                                       \
  }

template <typename T>
struct GeqrfBatchedKernel;
GEQRF_BATCHED_KERNEL_IMPL(float, gpublasSgeqrfBatched);
GEQRF_BATCHED_KERNEL_IMPL(double, gpublasDgeqrfBatched);
GEQRF_BATCHED_KERNEL_IMPL(gpublasComplex, gpublasCgeqrfBatched);
GEQRF_BATCHED_KERNEL_IMPL(gpublasDoubleComplex, gpublasZgeqrfBatched);
#undef GEQRF_BATCHED_KERNEL_IMPL

template <typename T>
ffi::Error GeqrfBatchedImpl(int64_t batch, int64_t rows, int64_t cols,
                            gpuStream_t stream, ffi::ScratchAllocator& scratch,
                            ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                            ffi::Result<ffi::AnyBuffer> tau) {
  FFI_ASSIGN_OR_RETURN(auto m, MaybeCastNoOverflow<int>(rows));
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));
  FFI_ASSIGN_OR_RETURN(auto handle, BlasHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(auto out_batch_ptrs,
                       AllocateWorkspace<T*>(scratch, batch, "batched geqrf"));
  FFI_ASSIGN_OR_RETURN(auto tau_batch_ptrs,
                       AllocateWorkspace<T*>(scratch, batch, "batched geqrf"));

  auto a_data = a.untyped_data();
  auto out_data = out->untyped_data();
  auto tau_data = tau->untyped_data();
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        out_data, a_data, a.size_bytes(), gpuMemcpyDeviceToDevice, stream)));
  }

  MakeBatchPointersAsync(stream, out_data, out_batch_ptrs, batch,
                         sizeof(T) * m * n);
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuGetLastError()));
  MakeBatchPointersAsync(stream, tau_data, tau_batch_ptrs, batch,
                         sizeof(T) * std::min(m, n));
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuGetLastError()));

  // We ignore the output value of `info` because it is only used for shape
  // checking.
  int info;
  FFI_RETURN_IF_ERROR_STATUS(GeqrfBatchedKernel<T>::Run(
      handle.get(), m, n, out_batch_ptrs, tau_batch_ptrs, &info, batch));

  return ffi::Error::Success();
}

ffi::Error GeqrfDispatch(gpuStream_t stream, ffi::ScratchAllocator scratch,
                         ffi::AnyBuffer a, ffi::Result<ffi::AnyBuffer> out,
                         ffi::Result<ffi::AnyBuffer> tau) {
  auto dataType = a.element_type();
  if (dataType != out->element_type() || dataType != tau->element_type()) {
    return ffi::Error::InvalidArgument(
        "The inputs and outputs to geqrf must have the same element type");
  }
  FFI_ASSIGN_OR_RETURN((auto [batch, rows, cols]),
                       SplitBatch2D(a.dimensions()));
  FFI_RETURN_IF_ERROR(
      CheckShape(out->dimensions(), {batch, rows, cols}, "out", "geqrf"));
  FFI_RETURN_IF_ERROR(CheckShape(
      tau->dimensions(), {batch, std::min(rows, cols)}, "tau", "geqrf"));
  if (batch > 1 && rows / batch <= 128 && cols / batch <= 128) {
    SOLVER_BLAS_DISPATCH_IMPL(GeqrfBatchedImpl, batch, rows, cols, stream,
                              scratch, a, out, tau);
  } else {
    SOLVER_DISPATCH_IMPL(GeqrfImpl, batch, rows, cols, stream, scratch, a, out,
                         tau);
  }
  return ffi::Error::InvalidArgument("Unsupported element type for geqrf");
}
}  // namespace

XLA_FFI_DEFINE_HANDLER_SYMBOL(GeqrfFfi, GeqrfDispatch,
                              ffi::Ffi::Bind()
                                  .Ctx<ffi::PlatformStream<gpuStream_t>>()
                                  .Ctx<ffi::ScratchAllocator>()
                                  .Arg<ffi::AnyBuffer>()  // a
                                  .Ret<ffi::AnyBuffer>()  // out
                                  .Ret<ffi::AnyBuffer>()  // tau
);

// Householder transformations: orgqr

namespace {
#define ORGQR_KERNEL_IMPL(type, name)                                        \
  template <>                                                                \
  struct OrgqrKernel<type> {                                                 \
    static absl::StatusOr<int> BufferSize(gpusolverDnHandle_t handle, int m, \
                                          int n, int k) {                    \
      int lwork;                                                             \
      JAX_RETURN_IF_ERROR(JAX_AS_STATUS(                                     \
          name##_bufferSize(handle, m, n, k, /*A=*/nullptr, /*lda=*/m,       \
                            /*tau=*/nullptr, &lwork)));                      \
      return lwork;                                                          \
    }                                                                        \
    static absl::Status Run(gpusolverDnHandle_t handle, int m, int n, int k, \
                            type* a, type* tau, type* workspace, int lwork,  \
                            int* info) {                                     \
      return JAX_AS_STATUS(                                                  \
          name(handle, m, n, k, a, m, tau, workspace, lwork, info));         \
    }                                                                        \
  }

template <typename T>
struct OrgqrKernel;
ORGQR_KERNEL_IMPL(float, gpusolverDnSorgqr);
ORGQR_KERNEL_IMPL(double, gpusolverDnDorgqr);
ORGQR_KERNEL_IMPL(gpuComplex, gpusolverDnCungqr);
ORGQR_KERNEL_IMPL(gpuDoubleComplex, gpusolverDnZungqr);
#undef ORGQR_KERNEL_IMPL

template <typename T>
ffi::Error OrgqrImpl(int64_t batch, int64_t rows, int64_t cols, int64_t size,
                     gpuStream_t stream, ffi::ScratchAllocator& scratch,
                     ffi::AnyBuffer a, ffi::AnyBuffer tau,
                     ffi::Result<ffi::AnyBuffer> out) {
  FFI_ASSIGN_OR_RETURN(auto m, MaybeCastNoOverflow<int>(rows));
  FFI_ASSIGN_OR_RETURN(auto n, MaybeCastNoOverflow<int>(cols));
  FFI_ASSIGN_OR_RETURN(auto k, MaybeCastNoOverflow<int>(size));

  FFI_ASSIGN_OR_RETURN(auto handle, SolverHandlePool::Borrow(stream));
  FFI_ASSIGN_OR_RETURN(int lwork,
                       OrgqrKernel<T>::BufferSize(handle.get(), m, n, k));

  FFI_ASSIGN_OR_RETURN(auto workspace,
                       AllocateWorkspace<T>(scratch, lwork, "orgqr"));
  // Note: We ignore the returned value of info because it is only used for
  // shape checking (which we already do ourselves), but it is expected to be
  // in device memory, so we need to allocate it.
  FFI_ASSIGN_OR_RETURN(auto info, AllocateWorkspace<int>(scratch, 1, "orgqr"));

  auto a_data = static_cast<T*>(a.untyped_data());
  auto tau_data = static_cast<T*>(tau.untyped_data());
  auto out_data = static_cast<T*>(out->untyped_data());
  if (a_data != out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        out_data, a_data, a.size_bytes(), gpuMemcpyDeviceToDevice, stream)));
  }

  int out_step = m * n;
  for (auto i = 0; i < batch; ++i) {
    FFI_RETURN_IF_ERROR_STATUS(OrgqrKernel<T>::Run(
        handle.get(), m, n, k, out_data, tau_data, workspace, lwork, info));
    out_data += out_step;
    tau_data += k;
  }
  return ffi::Error::Success();
}

ffi::Error OrgqrDispatch(gpuStream_t stream, ffi::ScratchAllocator scratch,
                         ffi::AnyBuffer a, ffi::AnyBuffer tau,
                         ffi::Result<ffi::AnyBuffer> out) {
  auto dataType = a.element_type();
  if (dataType != tau.element_type() || dataType != out->element_type()) {
    return ffi::Error::InvalidArgument(
        "The inputs and outputs to orgqr must have the same element type");
  }
  FFI_ASSIGN_OR_RETURN((auto [batch, rows, cols]),
                       SplitBatch2D(a.dimensions()));
  FFI_ASSIGN_OR_RETURN((auto [tau_batch, size]),
                       SplitBatch1D(tau.dimensions()));
  if (tau_batch != batch) {
    return ffi::Error::InvalidArgument(
        "The batch dimensions of the inputs to orgqr must match");
  }
  if (size > cols) {
    return ffi::Error::InvalidArgument(
        "The trailing dimension of the tau input to orgqr must be less than or "
        "equal to the number of columns of the input matrix");
  }
  FFI_RETURN_IF_ERROR(
      CheckShape(out->dimensions(), {batch, rows, cols}, "out", "orgqr"));
  SOLVER_DISPATCH_IMPL(OrgqrImpl, batch, rows, cols, size, stream, scratch, a,
                       tau, out);
  return ffi::Error::InvalidArgument("Unsupported element type for orgqr");
}
}  // namespace

XLA_FFI_DEFINE_HANDLER_SYMBOL(OrgqrFfi, OrgqrDispatch,
                              ffi::Ffi::Bind()
                                  .Ctx<ffi::PlatformStream<gpuStream_t>>()
                                  .Ctx<ffi::ScratchAllocator>()
                                  .Arg<ffi::AnyBuffer>()  // a
                                  .Arg<ffi::AnyBuffer>()  // tau
                                  .Ret<ffi::AnyBuffer>()  // out
);

#undef SOLVER_DISPATCH_IMPL


#define SYRK_KERNEL_IMPL(type, fn)                                             \
  template <>                                                                  \
  struct SyrkKernel<type> {                                                    \
    static absl::Status Run(gpublasHandle_t handle, std::int64_t n,            \
                            std::int64_t k, bool transpose,                    \
                            const type* alpha, const type* beta,               \
                            const type* a_matrix, type* c_matrix) {            \
      gpublasOperation_t op = transpose ? GPUBLAS_OP_N : GPUBLAS_OP_T;         \
      gpublasFillMode_t uplo = GPUSOLVER_FILL_MODE_UPPER;                      \
      int lda = transpose ? n : k;                                             \
      return JAX_AS_STATUS(fn(handle, uplo, op, n, k,                          \
                              alpha, a_matrix, lda, beta,                      \
                              c_matrix, n));                                   \
    }                                                                          \
  }

template <typename T>
struct SyrkKernel;

SYRK_KERNEL_IMPL(float, gpublasSsyrk);
SYRK_KERNEL_IMPL(double, gpublasDsyrk);
SYRK_KERNEL_IMPL(gpublasComplex, gpublasCsyrk);
SYRK_KERNEL_IMPL(gpublasDoubleComplex, gpublasZsyrk);
#undef SYRK_KERNEL_IMPL

template <typename T>
ffi::Error SyrkImpl(gpuStream_t stream,
                    ffi::AnyBuffer a_matrix,
                    ffi::AnyBuffer c_matrix,
                    bool transpose,
                    ffi::AnyBuffer alpha,
                    ffi::AnyBuffer beta,
                    ffi::Result<ffi::AnyBuffer> c_matrix_out) {
  FFI_ASSIGN_OR_RETURN((auto [batch, rows, cols]),
                       SplitBatch2D(a_matrix.dimensions()));
  FFI_ASSIGN_OR_RETURN((auto [batch_c, rows_c, cols_c]),
                       SplitBatch2D(c_matrix.dimensions()));
  FFI_ASSIGN_OR_RETURN((auto [batch_out, rows_out, cols_out]),
                       SplitBatch2D(c_matrix_out->dimensions()));
  if (batch != batch_c || batch != batch_out) {
    return ffi::Error(ffi::ErrorCode::kInvalidArgument,
                      "a_matrix, c_matrix and c_matrix_out must have the same "
                      "batch size.");
  }
  int n = transpose ? cols : rows;
  int k = transpose ? rows : cols;

  FFI_RETURN_IF_ERROR(
    CheckShape(c_matrix_out->dimensions().last(2), {n, n}, "out", "Syrk"));
  FFI_RETURN_IF_ERROR(
    CheckShape(c_matrix.dimensions().last(2), {n, n}, "C", "Syrk"));

  const T* a_data = static_cast<const T*>(a_matrix.untyped_data());
  T* c_data = static_cast<T*>(c_matrix.untyped_data());
  T* c_out_data = static_cast<T*>(c_matrix_out->untyped_data());

  // with alpha or beta provided as device_pointers, cublas<T>syrk will SIGSEGV
  T host_alpha;
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
    &host_alpha, alpha.untyped_data(), sizeof(T), gpuMemcpyDeviceToHost,
    stream)));

  T host_beta;
  FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
    &host_beta, beta.untyped_data(), sizeof(T), gpuMemcpyDeviceToHost,
    stream)));

  if (c_data != c_out_data) {
    FFI_RETURN_IF_ERROR_STATUS(JAX_AS_STATUS(gpuMemcpyAsync(
        c_out_data, c_data, c_matrix.size_bytes(), gpuMemcpyDeviceToDevice,
        stream)));
  }
  FFI_ASSIGN_OR_RETURN(auto handle, BlasHandlePool::Borrow(stream));
  for (int i = 0; i < batch; ++i) {
    FFI_RETURN_IF_ERROR_STATUS(SyrkKernel<T>::Run(
        handle.get(), n, k, transpose, &host_alpha, &host_beta,
        a_data + i * k * n, c_out_data + i * n * n));
  }
  return ffi::Error::Success();
}

ffi::Error SyrkDispatch(
    gpuStream_t stream,
    ffi::AnyBuffer a_matrix,
    ffi::AnyBuffer c_matrix,
    bool transpose,
    ffi::AnyBuffer alpha,
    ffi::AnyBuffer beta,
    ffi::Result<ffi::AnyBuffer> c_matrix_out) {
  auto dataType = a_matrix.element_type();
  SOLVER_BLAS_DISPATCH_IMPL(SyrkImpl, stream, a_matrix, c_matrix, transpose,
                            alpha, beta, c_matrix_out);
  return ffi::Error::InvalidArgument("Unsupported element type for Syrk");
}

XLA_FFI_DEFINE_HANDLER_SYMBOL(SyrkFfi, SyrkDispatch,
                              ffi::Ffi::Bind()
                                  .Ctx<ffi::PlatformStream<gpuStream_t>>()
                                  .Arg<ffi::AnyBuffer>()  // a_matrix
                                  .Arg<ffi::AnyBuffer>()  // c_matrix
                                  .Attr<bool>("transpose")  // transpose
                                  .Arg<ffi::AnyBuffer>()  // alpha
                                  .Arg<ffi::AnyBuffer>()  // beta
                                  .Ret<ffi::AnyBuffer>());  // c_matrix_out


}  // namespace JAX_GPU_NAMESPACE
}  // namespace jax
