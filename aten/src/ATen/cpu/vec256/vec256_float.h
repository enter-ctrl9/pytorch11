#pragma once

#include "intrinsics.h"
#include "vec256_base.h"
#if defined(__AVX__) && !defined(_MSC_VER)
#include <sleef.h>
#endif

namespace at {
namespace vec256 {
namespace {

#if defined(__AVX__) && !defined(_MSC_VER)

template <> class Vec256<float> {
private:
  __m256 values;
public:
  static constexpr int64_t size = 8;
  Vec256() {}
  Vec256(__m256 v) : values(v) {}
  Vec256(float val) {
    values = _mm256_set1_ps(val);
  }
  operator __m256() const {
    return values;
  }
  template <int64_t mask>
  static Vec256<float> blend(Vec256<float> a, Vec256<float> b) {
    return _mm256_blend_ps(a.values, b.values, mask);
  }
  static Vec256<float> set(Vec256<float> a, Vec256<float> b, int64_t count = size) {
    switch (count) {
      case 0:
        return a;
      case 1:
        return blend<1>(a, b);
      case 2:
        return blend<3>(a, b);
      case 3:
        return blend<7>(a, b);
      case 4:
        return blend<15>(a, b);
      case 5:
        return blend<31>(a, b);
      case 6:
        return blend<63>(a, b);
      case 7:
        return blend<127>(a, b);
    }
    return b;
  }
  static Vec256<float> loadu(const void* ptr, int64_t count = size, int64_t stride = 1) {
    if (count == size && stride == 1)
      return _mm256_loadu_ps(reinterpret_cast<const float*>(ptr));

// #ifdef __AVX2__
//     if(count == size) {
//       __m256i vindex = _mm256_set_epi32(
//           ((int32_t)7 * stride),
//           ((int32_t)6 * stride),
//           ((int32_t)5 * stride),
//           ((int32_t)4 * stride),
//           ((int32_t)3 * stride),
//           ((int32_t)2 * stride),
//           ((int32_t)1 * stride),
//           0);
//       return _mm256_i32gather_ps(reinterpret_cast<const float*>(ptr), vindex, 1);
//     }
// #endif

    __at_align32__ float tmp_values[size];
    if (stride == 1) {
      std::memcpy(
          tmp_values,
          reinterpret_cast<const float*>(ptr),
          count * sizeof(float));
    } else {
      for (int64_t i = 0; i < count; i++) {
        tmp_values[i] = reinterpret_cast<const float*>(ptr)[i * stride];
      }
    }
    return _mm256_load_ps(tmp_values);
  }
  void store(void* ptr, int64_t count = size, int64_t stride = 1) const {
    if (count == size && stride == 1) {
      _mm256_storeu_ps(reinterpret_cast<float*>(ptr), values);
    } else {
      float tmp_values[size];
      _mm256_storeu_ps(reinterpret_cast<float*>(tmp_values), values);
      if (stride == 1) {
        std::memcpy(ptr, tmp_values, count * sizeof(float));
      } else {
        for (int64_t i = 0; i < count; i++) {
          reinterpret_cast<float*>(ptr)[i * stride] = tmp_values[i];
        }
      }
    }
  }
  const float& operator[](int idx) const  = delete;
  float& operator[](int idx) = delete;
  Vec256<float> map(float (*f)(float)) const {
    __at_align32__ float tmp[8];
    store(tmp);
    for (int64_t i = 0; i < 8; i++) {
      tmp[i] = f(tmp[i]);
    }
    return loadu(tmp);
  }
  Vec256<float> abs() const {
    auto mask = _mm256_set1_ps(-0.f);
    return _mm256_andnot_ps(mask, values);
  }
  Vec256<float> acos() const {
    return Vec256<float>(Sleef_acosf8_u10(values));
  }
  Vec256<float> asin() const {
    return Vec256<float>(Sleef_asinf8_u10(values));
  }
  Vec256<float> atan() const {
    return Vec256<float>(Sleef_atanf8_u10(values));
  }
  Vec256<float> erf() const {
    return Vec256<float>(Sleef_erff8_u10(values));
  }
  Vec256<float> erfc() const {
    return Vec256<float>(Sleef_erfcf8_u15(values));
  }
  Vec256<float> exp() const {
    return Vec256<float>(Sleef_expf8_u10(values));
  }
  Vec256<float> expm1() const {
    return Vec256<float>(Sleef_expm1f8_u10(values));
  }
  Vec256<float> log() const {
    return Vec256<float>(Sleef_logf8_u10(values));
  }
  Vec256<float> log2() const {
    return Vec256<float>(Sleef_log2f8_u10(values));
  }
  Vec256<float> log10() const {
    return Vec256<float>(Sleef_log10f8_u10(values));
  }
  Vec256<float> log1p() const {
    return Vec256<float>(Sleef_log1pf8_u10(values));
  }
  Vec256<float> sin() const {
    return map(std::sin);
  }
  Vec256<float> sinh() const {
    return map(std::sinh);
  }
  Vec256<float> cos() const {
    return map(std::cos);
  }
  Vec256<float> cosh() const {
    return map(std::cosh);
  }
  Vec256<float> ceil() const {
    return _mm256_ceil_ps(values);
  }
  Vec256<float> floor() const {
    return _mm256_floor_ps(values);
  }
  Vec256<float> neg() const {
    return _mm256_xor_ps(_mm256_set1_ps(-0.f), values);
  }
  Vec256<float> round() const {
    return _mm256_round_ps(values, (_MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
  }
  Vec256<float> tan() const {
    return map(std::tan);
  }
  Vec256<float> tanh() const {
    return Vec256<float>(Sleef_tanhf8_u10(values));
  }
  Vec256<float> trunc() const {
    return _mm256_round_ps(values, (_MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
  }
  Vec256<float> sqrt() const {
    return _mm256_sqrt_ps(values);
  }
  Vec256<float> reciprocal() const {
    return _mm256_div_ps(_mm256_set1_ps(1), values);
  }
  Vec256<float> rsqrt() const {
    return _mm256_div_ps(_mm256_set1_ps(1), _mm256_sqrt_ps(values));
  }
};

template <>
Vec256<float> inline operator+(const Vec256<float>& a, const Vec256<float>& b) {
  return _mm256_add_ps(a, b);
}

template <>
Vec256<float> inline operator-(const Vec256<float>& a, const Vec256<float>& b) {
  return _mm256_sub_ps(a, b);
}

template <>
Vec256<float> inline operator*(const Vec256<float>& a, const Vec256<float>& b) {
  return _mm256_mul_ps(a, b);
}

template <>
Vec256<float> inline operator/(const Vec256<float>& a, const Vec256<float>& b) {
  return _mm256_div_ps(a, b);
}

template <>
Vec256<float> inline max(const Vec256<float>& a, const Vec256<float>& b) {
  //NB: The order here matters because of NaN propagation!
  //See [NaN propagation] in vec256 base class
  return _mm256_max_ps(b, a);
}

template <>
Vec256<float> inline min(const Vec256<float>& a, const Vec256<float>& b) {
  return _mm256_min_ps(a, b);
}

#endif

}}}
