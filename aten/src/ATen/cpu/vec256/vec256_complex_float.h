#pragma once

#include <ATen/cpu/vec256/intrinsics.h>
#include <ATen/cpu/vec256/vec256_base.h>
#if defined(__AVX__) && !defined(_MSC_VER)
#include <sleef.h>
#endif

namespace at {
namespace vec256 {
// See Note [Acceptable use of anonymous namespace in header]
namespace {

#if defined(__AVX__) && !defined(_MSC_VER)

template <> class Vec256<std::complex<float>> {
private:
  __m256 values;
public:
  using value_type = std::complex<float>;
  static constexpr int size() {
    return 4;
  }
  Vec256() {}
  Vec256(__m256 v) : values(v) {}
  Vec256(std::complex<float> val) {
    float real_value = std::real(val);
    float imag_value = std::imag(val);
    values = _mm256_setr_ps(real_value, imag_value,
                            real_value, imag_value,
                            real_value, imag_value,
                            real_value, imag_value
                            );
  }
  Vec256(std::complex<float> val1, std::complex<float> val2, std::complex<float> val3, std::complex<float> val4) {
    values = _mm256_setr_ps(std::real(val1), std::imag(val1),
                            std::real(val2), std::imag(val2),
                            std::real(val3), std::imag(val3),
                            std::real(val4), std::imag(val4)
                            );
  }
  operator __m256() const {
    return values;
  }
  template <int64_t mask>
  static Vec256<std::complex<float>> blend(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
     // convert std::complex<V> index mask to V index mask: xy -> xxyy
    switch (mask) {
      case 0:
        return a;
      case 1:
        return _mm256_blend_ps(a.values, b.values, 0x03); //b0000 0001 = b0000 0011
      case 2:
        return _mm256_blend_ps(a.values, b.values, 0x0C); //b0000 0010 = b0000 1100
      case 3:
        return _mm256_blend_ps(a.values, b.values, 0x0F); //b0000 0011 = b0000 1111
      case 4:
        return _mm256_blend_ps(a.values, b.values, 0x30); //b0000 0100 = b0011 0000
      case 5:
        return _mm256_blend_ps(a.values, b.values, 0x33); //b0000 0101 = b0011 0011
      case 6:
        return _mm256_blend_ps(a.values, b.values, 0x3C); //b0000 0110 = b0011 1100
      case 7:
        return _mm256_blend_ps(a.values, b.values, 0x3F); //b0000 0111 = b0011 1111
      case 8:
        return _mm256_blend_ps(a.values, b.values, 0xC0); //b0000 1000 = b1100 0000
      case 9:
        return _mm256_blend_ps(a.values, b.values, 0xC3); //b0000 1001 = b1100 0011
      case 10:
        return _mm256_blend_ps(a.values, b.values, 0xCC); //b0000 1010 = b1100 1100
      case 11:
        return _mm256_blend_ps(a.values, b.values, 0xCF); //b0000 1011 = b1100 1111
      case 12:
        return _mm256_blend_ps(a.values, b.values, 0xF0); //b0000 1100 = b1111 0000
      case 13:
        return _mm256_blend_ps(a.values, b.values, 0xF3); //b0000 1101 = b1111 0011
      case 14:
        return _mm256_blend_ps(a.values, b.values, 0xFC); //b0000 1110 = b1111 1100
    }
    return b;
  }
  static Vec256<std::complex<float>> blendv(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b,
                               const Vec256<std::complex<float>>& mask) {
    // convert std::complex<V> index mask to V index mask: xy -> xxyy
    auto mask_ = _mm256_unpacklo_ps(mask.values, mask.values);
    return _mm256_blendv_ps(a.values, b.values, mask_);

  }
  static Vec256<std::complex<float>> arange(std::complex<float> base = 0., std::complex<float> step = 1.) {
    return Vec256<std::complex<float>>(base,
                                        base + step,
                                        base + std::complex<float>(2)*step,
                                        base + std::complex<float>(3)*step);
  }
  static Vec256<std::complex<float>> set(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b,
                            int64_t count = size()) {
    switch (count) {
      case 0:
        return a;
      case 1:
        return blend<1>(a, b);
      case 2:
        return blend<3>(a, b);
      case 3:
        return blend<7>(a, b);
    }
    return b;
  }
  static Vec256<std::complex<float>> loadu(const void* ptr, int64_t count = size()) {
    if (count == size())
      return _mm256_loadu_ps(reinterpret_cast<const float*>(ptr));

    __at_align32__ float tmp_values[2*size()];
    std::memcpy(
        tmp_values,
        reinterpret_cast<const float*>(ptr),
        count * sizeof(std::complex<float>));
    return _mm256_load_ps(tmp_values);
  }
  void store(void* ptr, int count = size()) const {
    if (count == size()) {
      _mm256_storeu_ps(reinterpret_cast<float*>(ptr), values);
    } else if (count > 0) {
      float tmp_values[2*size()];
      _mm256_storeu_ps(reinterpret_cast<float*>(tmp_values), values);
      std::memcpy(ptr, tmp_values, count * sizeof(std::complex<float>));
    }
  }
  const std::complex<float>& operator[](int idx) const  = delete;
  std::complex<float>& operator[](int idx) = delete;
  Vec256<std::complex<float>> map(std::complex<float> (*f)(const std::complex<float> &)) const {
    __at_align32__ std::complex<float> tmp[size()];
    store(tmp);
    for (int i = 0; i < size(); i++) {
      tmp[i] = f(tmp[i]);
    }
    return loadu(tmp);
  }
  __m256 abs_2_() const {
    auto val_2 = _mm256_mul_ps(values, values);     // a*a     b*b
    auto ret = _mm256_hadd_ps(val_2, val_2);        // a*a+b*b a*a+b*b
    return _mm256_permute_ps(ret, 0xD8);
  }
  __m256 abs_() const {
    return _mm256_sqrt_ps(abs_2_());                // abs     abs
  }
  Vec256<std::complex<float>> abs() const {
    const __m256 real_mask = _mm256_castsi256_ps(_mm256_setr_epi32(0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000,
                                                                   0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000));
    return _mm256_and_ps(abs_(), real_mask);        // abs     0
  }
  __m256 angle_() const {
    //angle = atan2(b/a)
    auto b_a = _mm256_permute_ps(values, 0xB1);     // b        a
    return Sleef_atan2f8_u10(values, b_a);          // 90-angle angle
  }
  Vec256<std::complex<float>> angle() const {
    const __m256 real_mask = _mm256_castsi256_ps(_mm256_setr_epi32(0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000,
                                                                   0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000));
    auto angle = _mm256_permute_ps(angle_(), 0xB1); // angle    90-angle
    return _mm256_and_ps(angle, real_mask);         // angle    0
  }
  __m256 real_() const {
    const __m256 real_mask = _mm256_castsi256_ps(_mm256_setr_epi32(0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000,
                                                                   0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000));
    return _mm256_and_ps(values, real_mask);
  }
  Vec256<std::complex<float>> real() const {
    return real_();
  }
  __m256 imag_() const {
    const __m256 imag_mask = _mm256_castsi256_ps(_mm256_setr_epi32(0x00000000, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF,
                                                                   0x00000000, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF));
    return _mm256_and_ps(values, imag_mask);
  }
  Vec256<std::complex<float>> imag() const {
    return _mm256_permute_ps(imag_(), 0xB1);        //b        a
  }
  __m256 conj_() const {
    const __m256 sign_mask = _mm256_setr_ps(0.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0, -0.0);
    return _mm256_xor_ps(values, sign_mask);        // a       -b
  }
  Vec256<std::complex<float>> conj() const {
    return conj_();
  }
  Vec256<std::complex<float>> log() const {
    // Most trigonomic ops use the log() op to improve complex number performance.
    return map(std::log);
  }
  Vec256<std::complex<float>> log2() const {
    const __m256 log2_ = _mm256_set1_ps(std::log(2));
    return _mm256_div_ps(log(), log2_);
  }
  Vec256<std::complex<float>> log10() const {
    const __m256 log10_ = _mm256_set1_ps(std::log(10));
    return _mm256_div_ps(log(), log10_);
  }
  Vec256<std::complex<float>> log1p() const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> asin() const {
    // asin(x)
    // = -i*ln(iz + sqrt(1 -z^2))
    // = -i*ln((ai - b) + sqrt(1 - (a + bi)*(a + bi)))
    // = -i*ln((-b + ai) + sqrt(1 - (a**2 - b**2) - 2*abi))
    const __m256 one = _mm256_set1_ps(1);

    auto conj = conj_();
    auto b_a = _mm256_permute_ps(conj, 0xB1);                         //-b        a
    auto ab = _mm256_mul_ps(conj, b_a);                               //-ab       -ab
    auto im = _mm256_add_ps(ab, ab);                                  //-2ab      -2ab

    auto val_2 = _mm256_mul_ps(values, values);                       // a*a      b*b
    auto re = _mm256_hsub_ps(val_2, _mm256_permute_ps(val_2, 0xB1));  // a*a-b*b  b*b-a*a
    re = _mm256_permute_ps(re, 0xD8);
    re = _mm256_sub_ps(one, re);

    auto root = Vec256(_mm256_blend_ps(re, im, 0xAA)).sqrt();         //sqrt(re + i*im)
    auto ln = Vec256(_mm256_add_ps(b_a, root)).log();                 //ln(iz + sqrt())
    return Vec256(_mm256_permute_ps(ln.values, 0xB1)).conj();         //-i*ln()
  }
  Vec256<std::complex<float>> acos() const {
    // acos(x) = pi/2 - asin(x)
    const __m256 pi_2 = _mm256_setr_ps(M_PI/2, 0.0, M_PI/2, 0.0, M_PI/2, 0.0, M_PI/2, 0.0);
    return _mm256_sub_ps(pi_2, asin());
  }
  Vec256<std::complex<float>> atan() const;
  Vec256<std::complex<float>> atan2(const Vec256<std::complex<float>> &b) const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> erf() const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> erfc() const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> exp() const {
    return map(std::exp);
  }
  Vec256<std::complex<float>> expm1() const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> sin() const {
    return map(std::sin);
  }
  Vec256<std::complex<float>> sinh() const {
    return map(std::sinh);
  }
  Vec256<std::complex<float>> cos() const {
    return map(std::cos);
  }
  Vec256<std::complex<float>> cosh() const {
    return map(std::cosh);
  }
  Vec256<std::complex<float>> ceil() const {
    return _mm256_ceil_ps(values);
  }
  Vec256<std::complex<float>> floor() const {
    return _mm256_floor_ps(values);
  }
  Vec256<std::complex<float>> neg() const {
    auto zero = _mm256_setzero_ps();
    return _mm256_sub_ps(zero, values);
  }
  Vec256<std::complex<float>> round() const {
    return _mm256_round_ps(values, (_MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
  }
  Vec256<std::complex<float>> tan() const {
    return map(std::tan);
  }
  Vec256<std::complex<float>> tanh() const {
    return map(std::tanh);
  }
  Vec256<std::complex<float>> trunc() const {
    return _mm256_round_ps(values, (_MM_FROUND_TO_ZERO | _MM_FROUND_NO_EXC));
  }
  Vec256<std::complex<float>> sqrt() const {
    //   sqrt(a + bi)
    // = sqrt(2)/2 * [sqrt(sqrt(a**2 + b**2) + a) + sgn(b)*sqrt(sqrt(a**2 + b**2) - a)i]
    // = sqrt(2)/2 * [sqrt(abs() + a) + sgn(b)*sqrt(abs() - a)i]

    const __m256 scalar = _mm256_set1_ps(std::sqrt(2)/2);              //sqrt(2)/2      sqrt(2)/2
    const __m256 sign_mask = _mm256_setr_ps(0.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0, -0.0);
    auto sign = _mm256_and_ps(values, sign_mask);
    auto factor = _mm256_or_ps(scalar, sign);

    auto a_a = _mm256_xor_ps(_mm256_moveldup_ps(values), sign_mask);   // a             -a
    auto res_re_im = _mm256_sqrt_ps(_mm256_add_ps(abs_(), a_a));       // sqrt(abs + a) sqrt(abs - a)
    return _mm256_mul_ps(factor, res_re_im);
  }
  Vec256<std::complex<float>> reciprocal() const;
  Vec256<std::complex<float>> rsqrt() const {
    return sqrt().reciprocal();
  }
  Vec256<std::complex<float>> pow(const Vec256<std::complex<float>> &exp) const {
    __at_align32__ std::complex<double> x_tmp[size()];
    __at_align32__ std::complex<double> y_tmp[size()];
    store(x_tmp);
    exp.store(y_tmp);
    for (int i = 0; i < size(); i++) {
      x_tmp[i] = std::pow(x_tmp[i], y_tmp[i]);
    }
    return loadu(x_tmp);
  }
  // Comparison using the _CMP_**_OQ predicate.
  //   `O`: get false if an operand is NaN
  //   `Q`: do not raise if an operand is NaN
  Vec256<std::complex<float>> operator==(const Vec256<std::complex<float>>& other) const {
    return _mm256_cmp_ps(values, other.values, _CMP_EQ_OQ);
  }
  Vec256<std::complex<float>> operator!=(const Vec256<std::complex<float>>& other) const {
    return _mm256_cmp_ps(values, other.values, _CMP_NEQ_OQ);
  }
  Vec256<std::complex<float>> operator<(const Vec256<std::complex<float>>& other) const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> operator<=(const Vec256<std::complex<float>>& other) const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> operator>(const Vec256<std::complex<float>>& other) const {
    AT_ERROR("not supported for complex numbers");
  }
  Vec256<std::complex<float>> operator>=(const Vec256<std::complex<float>>& other) const {
    AT_ERROR("not supported for complex numbers");
  }
};

template <> Vec256<std::complex<float>> inline operator+(const Vec256<std::complex<float>> &a, const Vec256<std::complex<float>> &b) {
  return _mm256_add_ps(a, b);
}

template <> Vec256<std::complex<float>> inline operator-(const Vec256<std::complex<float>> &a, const Vec256<std::complex<float>> &b) {
  return _mm256_sub_ps(a, b);
}

template <> Vec256<std::complex<float>> inline operator*(const Vec256<std::complex<float>> &a, const Vec256<std::complex<float>> &b) {
  //(a + bi)  * (c + di) = (ac - bd) + (ad + bc)i
  const __m256 sign_mask = _mm256_setr_ps(0.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0, -0.0);
  auto ac_bd = _mm256_mul_ps(a, b);         //ac       bd

  auto d_c = _mm256_permute_ps(b, 0xB1);    //d        c
  d_c = _mm256_xor_ps(sign_mask, d_c);      //d       -c
  auto ad_bc = _mm256_mul_ps(a, d_c);       //ad      -bc

  auto ret = _mm256_hsub_ps(ac_bd, ad_bc);  //ac - bd  ad + bc
  ret = _mm256_permute_ps(ret, 0xD8);
  return ret;
}

template <> Vec256<std::complex<float>> inline operator/(const Vec256<std::complex<float>> &a, const Vec256<std::complex<float>> &b) {
  //re + im*i = (a + bi)  / (c + di)
  //re = (ac + bd)/abs_2()
  //im = (bc - ad)/abs_2()
  const __m256 sign_mask = _mm256_setr_ps(-0.0, 0.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0);
  auto ac_bd = _mm256_mul_ps(a, b);         //ac       bd

  auto d_c = _mm256_permute_ps(b, 0xB1);    //d        c
  d_c = _mm256_xor_ps(sign_mask, d_c);      //-d       c
  auto ad_bc = _mm256_mul_ps(a, d_c);       //-ad      bc

  auto re_im = _mm256_hadd_ps(ac_bd, ad_bc);//ac + bd  bc - ad
  re_im = _mm256_permute_ps(re_im, 0xD8);
  return _mm256_div_ps(re_im, b.abs_2_());
}

// reciprocal. Implement this here so we can use multiplication.
Vec256<std::complex<float>> Vec256<std::complex<float>>::reciprocal() const {
  //re + im*i = (a + bi)  / (c + di)
  //re = (ac + bd)/abs_2() = c/abs_2()
  //im = (bc - ad)/abs_2() = d/abs_2()
  const __m256 sign_mask = _mm256_setr_ps(0.0, -0.0, 0.0, -0.0, 0.0, -0.0, 0.0, -0.0);
  auto c_d = _mm256_xor_ps(sign_mask, values);    //c       -d
  return _mm256_div_ps(c_d, abs_2_());
}

Vec256<std::complex<float>> Vec256<std::complex<float>>::atan() const {
  // atan(x) = i/2 * ln((i + z)/(i - z))
  const __m256 i = _mm256_setr_ps(0.0, 1.0, 0.0, 1.0, 0.0, 1.0, 0.0, 1.0);
  const Vec256 i_half = _mm256_setr_ps(0.0, 0.5, 0.0, 0.5, 0.0, 0.5, 0.0, 0.5);

  auto sum = Vec256(_mm256_add_ps(i, values));                      // a        1+b
  auto sub = Vec256(_mm256_sub_ps(i, values));                      // -a       1-b
  auto ln = (sum/sub).log();                                        // ln((i + z)/(i - z))
  return i_half*ln;                                                 // i/2*ln()
}

template <>
Vec256<std::complex<float>> inline maximum(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
  auto abs_a = a.abs_2_();
  auto abs_b = b.abs_2_();
  auto mask = _mm256_cmp_ps(abs_a, abs_b, _CMP_LT_OQ);
  auto max = _mm256_blendv_ps(a, b, mask);
  // Exploit the fact that all-ones is a NaN.
  auto isnan = _mm256_cmp_ps(abs_a, abs_b, _CMP_UNORD_Q);
  return _mm256_or_ps(max, isnan);
}

template <>
Vec256<std::complex<float>> inline minimum(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
  auto abs_a = a.abs_2_();
  auto abs_b = b.abs_2_();
  auto mask = _mm256_cmp_ps(abs_a, abs_b, _CMP_GT_OQ);
  auto min = _mm256_blendv_ps(a, b, mask);
  // Exploit the fact that all-ones is a NaN.
  auto isnan = _mm256_cmp_ps(abs_a, abs_b, _CMP_UNORD_Q);
  return _mm256_or_ps(min, isnan);
}

template <>
Vec256<std::complex<float>> inline clamp(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& min, const Vec256<std::complex<float>>& max) {
  auto abs_a = a.abs_2_();
  auto abs_min = min.abs_2_();
  auto max_mask = _mm256_cmp_ps(abs_a, abs_min, _CMP_LT_OQ);
  auto abs_max = max.abs_2_();
  auto min_mask = _mm256_cmp_ps(abs_a, abs_max, _CMP_GT_OQ);
  return _mm256_blendv_ps(_mm256_blendv_ps(a, min, max_mask), max, min_mask);
}

template <>
Vec256<std::complex<float>> inline clamp_min(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& min) {
  auto abs_a = a.abs_2_();
  auto abs_min = min.abs_2_();
  auto max_mask = _mm256_cmp_ps(abs_a, abs_min, _CMP_LT_OQ);
  return _mm256_blendv_ps(a, min, max_mask);
}

template <>
Vec256<std::complex<float>> inline clamp_max(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& max) {
  auto abs_a = a.abs_2_();
  auto abs_max = max.abs_2_();
  auto min_mask = _mm256_cmp_ps(abs_a, abs_max, _CMP_GT_OQ);
  return _mm256_blendv_ps(a, max, min_mask);
}

template <>
Vec256<std::complex<float>> inline operator&(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
  return _mm256_and_ps(a, b);
}

template <>
Vec256<std::complex<float>> inline operator|(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
  return _mm256_or_ps(a, b);
}

template <>
Vec256<std::complex<float>> inline operator^(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b) {
  return _mm256_xor_ps(a, b);
}

#ifdef __AVX2__
template <> inline Vec256<std::complex<float>> fmadd(const Vec256<std::complex<float>>& a, const Vec256<std::complex<float>>& b, const Vec256<std::complex<float>>& c) {
  return a * b + c;
}
#endif

#endif

}}}
