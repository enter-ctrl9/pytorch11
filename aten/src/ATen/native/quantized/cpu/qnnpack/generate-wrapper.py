import os


QNNPACK_SOURCES = {
    # Generic functions
    None: [
        "requantization/fp32-psimd.c",
        "requantization/fp32-scalar.c",
        "requantization/gemmlowp-scalar.c",
        "requantization/precise-psimd.c",
        "requantization/precise-scalar.c",
        "requantization/q31-scalar.c",
        "sgemm/6x8-psimd.c",
        "u8lut32norm/scalar.c",
        "x8lut/scalar.c",
    ],
    # AArch32/AArch64-specific uKernels
    "defined(__arm__) || defined(__aarch64__)": [
        "q8avgpool/mp8x9p8q-neon.c",
        "q8avgpool/up8x9-neon.c",
        "q8avgpool/up8xm-neon.c",
        "q8conv/4x8-neon.c",
        "q8conv/8x8-neon.c",
        "q8dwconv/mp8x25-neon.c",
        "q8dwconv/mp8x25-neon-per-channel.c",
        "q8dwconv/up8x9-neon.c",
        "q8dwconv/up8x9-neon-per-channel.c",
        "q8gavgpool/mp8x7p7q-neon.c",
        "q8gavgpool/up8x7-neon.c",
        "q8gavgpool/up8xm-neon.c",
        "q8gemm/4x-sumrows-neon.c",
        "q8gemm/4x8-neon.c",
        "q8gemm/4x8-dq-neon.c",
        "q8gemm/4x8c2-xzp-neon.c",
        "q8gemm/6x4-neon.c",
        "q8gemm/8x8-neon.c",
        "q8vadd/neon.c",
        "requantization/fp32-neon.c",
        "requantization/gemmlowp-neon.c",
        "requantization/precise-neon.c",
        "requantization/q31-neon.c",
        "sgemm/5x8-neon.c",
        "sgemm/6x8-neon.c",
        "u8clamp/neon.c",
        "u8maxpool/16x9p8q-neon.c",
        "u8maxpool/sub16-neon.c",
        "u8rmax/neon.c",
        "x8zip/x2-neon.c",
        "x8zip/x3-neon.c",
        "x8zip/x4-neon.c",
        "x8zip/xm-neon.c",
    ],
    # x86/x86-64-specific uKernels
    "defined(__i386__) || defined(__i686__) || defined(__x86_64__)": [
        "q8avgpool/mp8x9p8q-sse2.c",
        "q8avgpool/up8x9-sse2.c",
        "q8avgpool/up8xm-sse2.c",
        "q8conv/4x4c2-sse2.c",
        "q8dwconv/mp8x25-sse2.c",
        "q8dwconv/mp8x25-sse2-per-channel.c",
        "q8dwconv/up8x9-sse2.c",
        "q8dwconv/up8x9-sse2-per-channel.c",
        "q8gavgpool/mp8x7p7q-sse2.c",
        "q8gavgpool/up8x7-sse2.c",
        "q8gavgpool/up8xm-sse2.c",
        "q8gemm/2x4c8-sse2.c",
        "q8gemm/4x4c2-dq-sse2.c",
        "q8gemm/4x4c2-sse2.c",
        "q8vadd/sse2.c",
        "requantization/fp32-sse2.c",
        "requantization/gemmlowp-sse2.c",
        "requantization/gemmlowp-sse4.c",
        "requantization/gemmlowp-ssse3.c",
        "requantization/precise-sse2.c",
        "requantization/precise-sse4.c",
        "requantization/precise-ssse3.c",
        "requantization/q31-sse2.c",
        "requantization/q31-sse4.c",
        "requantization/q31-ssse3.c",
        "u8clamp/sse2.c",
        "u8maxpool/16x9p8q-sse2.c",
        "u8maxpool/sub16-sse2.c",
        "u8rmax/sse2.c",
        "x8zip/x2-sse2.c",
        "x8zip/x3-sse2.c",
        "x8zip/x4-sse2.c",
        "x8zip/xm-sse2.c",
    ],
    # AArch32-specific uKernels
    "defined(__arm__)": [
        "hgemm/8x8-aarch32-neonfp16arith.S",
        "q8conv/4x8-aarch32-neon.S",
        "q8dwconv/up8x9-aarch32-neon.S",
        "q8dwconv/up8x9-aarch32-neon-per-channel.S",
        "q8gemm/4x8-aarch32-neon.S",
        "q8gemm/4x8-dq-aarch32-neon.S",
        "q8gemm/4x8c2-xzp-aarch32-neon.S",
    ],
    # AArch64-specific uKernels
    "defined(__aarch64__)": [
        "q8conv/8x8-aarch64-neon.S",
        "q8gemm/8x8-aarch64-neon.S",
        "q8gemm/8x8-dq-aarch64-neon.S",
    ],
}

BANNER = "/* Auto-generated by generate-wrappers.py script. Do not modify */"


if __name__ == "__main__":
    for condition, filenames in QNNPACK_SOURCES.items():
        for filename in filenames:
            filepath = os.path.join("wrappers", filename)
            if not os.path.isdir(os.path.dirname(filepath)):
                os.makedirs(os.path.dirname(filepath))
            with open(filepath, "w") as wrapper:
                print(BANNER, file=wrapper)
                print(file=wrapper)

                # Architecture- or platform-dependent preprocessor flags can be
                # defined here. Note: platform_preprocessor_flags can't be used
                # because they are ignored by arc focus & buck project.

                if condition is None:
                    print("#include <%s>" % filename, file=wrapper)
                else:
                    # Include source file only if condition is satisfied
                    print("#if %s" % condition, file=wrapper)
                    print("#include <%s>" % filename, file=wrapper)
                    print("#endif /* %s */" % condition, file=wrapper)
