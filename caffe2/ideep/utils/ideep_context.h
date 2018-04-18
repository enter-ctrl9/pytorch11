/**
 * Copyright (c) 2016-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <cstdlib>
#include <ctime>
#include <random>

#include <caffe2/core/context.h>

namespace caffe2 {

class IDEEPContext final {
 public:
  typedef std::mt19937 rand_gen_type;
  IDEEPContext() : random_seed_(RandomNumberSeed()) {}
  explicit IDEEPContext(const DeviceOption& option)
      : random_seed_(
            option.has_random_seed() ? option.random_seed()
                                     : RandomNumberSeed()) {
    CAFFE_ENFORCE_EQ(option.device_type(), IDEEP);
  }

  ~IDEEPContext() noexcept {}

  inline void SwitchToDevice(int /*stream_id*/) {}
  inline void SwitchToDevice() {
    SwitchToDevice(0);
  }

  inline void WaitEvent(const Event& ev) {
    ev.Wait(IDEEP, this);
  }

  inline void Record(Event* ev, const char* err_msg = nullptr) const {
    CAFFE_ENFORCE(ev, "Event must not be null.");
    ev->Record(IDEEP, this, err_msg);
  }


  inline void FinishDeviceComputation() {}

  inline rand_gen_type& RandGenerator() {
    if (!random_generator_.get()) {
      random_generator_.reset(new rand_gen_type(random_seed_));
    }
    return *random_generator_.get();
  }

  inline static std::pair<void*, MemoryDeleter> New(size_t nbytes) {
    return GetCPUAllocator()->New(nbytes);
  }

  // Two copy functions that deals with cross-device copies.
  template <class SrcContext, class DstContext>
  inline void CopyBytes(size_t nbytes, const void* src, void* dst);

  template <typename T, class SrcContext, class DstContext>
  inline void Copy(size_t n, const T* src, T* dst) {
    if (std::is_fundamental<T>::value) {
      CopyBytes<SrcContext, DstContext>(
          n * sizeof(T),
          static_cast<const void*>(src),
          static_cast<void*>(dst));
    } else {
      for (int i = 0; i < n; ++i) {
        dst[i] = src[i];
      }
    }
  }

  template <class SrcContext, class DstContext>
  inline void
  CopyItems(const TypeMeta& meta, size_t n, const void* src, void* dst) {
    if (meta.copy()) {
      meta.copy()(src, dst, n);
    } else {
      CopyBytes<SrcContext, DstContext>(n * meta.itemsize(), src, dst);
    }
  }

  static bool HasAsyncPartDefault() {
    return false;
  }

  static bool SupportsAsyncScheduling() {
    return false;
  }

  static bool IsStreamFree(const DeviceOption& /* unused */, int /* unused */) {
    return true;
  }

 protected:
  // TODO(jiayq): instead of hard-coding a generator, make it more flexible.
  int random_seed_{1701};
  std::unique_ptr<rand_gen_type> random_generator_;
};

template <>
inline void IDEEPContext::CopyBytes<IDEEPContext, IDEEPContext>(
    size_t nbytes,
    const void* src,
    void* dst) {
  if (nbytes == 0) {
    return;
  }
  CAFFE_ENFORCE(src);
  CAFFE_ENFORCE(dst);
  memcpy(dst, src, nbytes);
}

template <>
inline void IDEEPContext::CopyBytes<CPUContext, IDEEPContext>(
    size_t nbytes,
    const void* src,
    void* dst) {
  if (nbytes == 0) {
    return;
  }
  CAFFE_ENFORCE(src);
  CAFFE_ENFORCE(dst);
  memcpy(dst, src, nbytes);
}

template <>
inline void IDEEPContext::CopyBytes<IDEEPContext, CPUContext>(
    size_t nbytes,
    const void* src,
    void* dst) {
  if (nbytes == 0) {
    return;
  }
  CAFFE_ENFORCE(src);
  CAFFE_ENFORCE(dst);
  memcpy(dst, src, nbytes);
}
} // namespace caffe2
