#pragma once
// cpu_feature_detect.h — Runtime CPU feature detection for SIMD dispatch.
//
// WHAT: Provides has_avx2() to detect AVX2+FMA support at runtime.
//       Used by MeshBLAS to choose between BVH4_CPU (SSE) and BVH8_CPU (AVX2).
//
// WHY:  We want maximum traversal performance on all CPUs:
//       - AVX2 + FMA → BVH8_CPU (8-wide nodes, ~40% faster than BVH4)
//       - SSE only   → BVH4_CPU (4-wide nodes, still much faster than BVH2)
//       Runtime detection means one binary works everywhere.
//
// HOW:  CPUID instruction (leaf 7, subleaf 0) → EBX bit 5 = AVX2.
//       Result is cached in a static local (computed exactly once).

#include "core/asserts.h"

#ifdef _MSC_VER
#include <intrin.h> // __cpuidex
#elif defined(__GNUC__) || defined(__clang__)
#include <cpuid.h>
#endif

namespace cpu_features {

// Returns true if this CPU supports AVX2 and FMA instructions.
// Result is cached after first call — zero overhead on subsequent calls.
inline bool has_avx2() {
	static const bool result = [] {
#ifdef _MSC_VER
		int info[4] = { 0 };
		__cpuidex(info, 7, 0);
		bool avx2 = (info[1] & (1 << 5)) != 0;  // EBX bit 5 = AVX2
		// Also check FMA (CPUID leaf 1, ECX bit 12) — BVH8_CPU requires both.
		int info1[4] = { 0 };
		__cpuid(info1, 1);
		bool fma = (info1[2] & (1 << 12)) != 0;  // ECX bit 12 = FMA
		RT_ASSERT(sizeof(info) == 16, "CPUID output array must be 4 ints");
		RT_ASSERT(sizeof(info1) == 16, "CPUID output array must be 4 ints");
		return avx2 && fma;
#elif defined(__GNUC__) || defined(__clang__)
		unsigned int eax, ebx, ecx, edx;
		if (__get_cpuid_count(7, 0, &eax, &ebx, &ecx, &edx)) {
			bool avx2 = (ebx & (1 << 5)) != 0;
			if (__get_cpuid(1, &eax, &ebx, &ecx, &edx)) {
				bool fma = (ecx & (1 << 12)) != 0;
				return avx2 && fma;
			}
		}
		return false;
#else
		return false;  // Unknown compiler — fallback to SSE (BVH4_CPU).
#endif
	}();
	return result;
}

} // namespace cpu_features
