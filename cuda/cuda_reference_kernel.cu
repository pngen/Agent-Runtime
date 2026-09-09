// Agent Runtime - CUDA reference kernel.
// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// SPDX-License-Identifier: Apache-2.0

#include <cuda_runtime.h>

__global__ void vector_add_kernel(const float* a, const float* b, float* c, int n) {
  const int index = blockIdx.x * blockDim.x + threadIdx.x;
  if (index < n) {
    c[index] = a[index] + b[index];
  }
}

extern "C" int agent_runtime_cuda_vector_add(const float* a, const float* b, float* c, int n,
                                             int* blocks, int* threads) {
  if (n <= 0) {
    return 1;
  }
  const int block_size = 256;
  const int grid_size = (n + block_size - 1) / block_size;
  vector_add_kernel<<<grid_size, block_size>>>(a, b, c, n);
  const cudaError_t error = cudaGetLastError();
  if (error != cudaSuccess) {
    return 2;
  }
  if (blocks != nullptr) {
    *blocks = grid_size;
  }
  if (threads != nullptr) {
    *threads = block_size;
  }
  return 0;
}
