#pragma once
// asserts.h — Tiger Style assertion macros for the raytracer.
//
// PHILOSOPHY (Tiger Style / TigerBeetle):
//   "Programming the negative space" — assert what should NOT happen.
//   NASA Power of 10 Rule #5: "At least 2 runtime assertions per function."
//   A failed assertion is a bug caught early; continuing with corrupt state
//   is ALWAYS worse than crashing.
//
// THREE TIERS:
//   RT_ASSERT()      — Debug only (stripped in release via NDEBUG).
//                       Use for assumptions during development.
//   RT_VERIFY()      — ALWAYS active, even in production.
//                       Use for invariants that must NEVER be violated.
//                       A failed RT_VERIFY means data corruption or logic error.
//   RT_SLOW_ASSERT() — Only when RT_SLOW_CHECKS defined.
//                       For expensive validations (full BVH integrity, etc.)
//
// CONVENIENCE MACROS:
//   RT_ASSERT_VALID_RAY(ray)       — Validates ray struct fields
//   RT_ASSERT_FINITE(value)        — No NaN or Inf
//   RT_ASSERT_NOT_NULL(ptr)        — Null pointer check
//   RT_ASSERT_BOUNDS(idx, size)    — Array bounds check (idx < size, idx >= 0)
//   RT_ASSERT_POSITIVE(val)        — Must be > 0
//   RT_ASSERT_NORMALIZED(vec, eps) — Vector approximately unit length
//   RT_UNREACHABLE(message)        — Code path that should never execute
//
// Usage:
//   RT_VERIFY(!triangles.empty(), "Cannot build BVH from empty array");
//   RT_ASSERT_BOUNDS(node_idx, node_count_);
//   RT_ASSERT_VALID_RAY(ray);

#include <cstdio>
#include <cstdlib>
#include <cmath>

// RT_DEBUG_ACTIVE is 1 when building debug targets.
// Godot's SCons sets NDEBUG for release/template_release builds.
#if !defined(NDEBUG) || defined(RT_FORCE_ASSERTS)
#define RT_DEBUG_ACTIVE 1
#else
#define RT_DEBUG_ACTIVE 0
#endif

// ---- Internal abort helper (shared by RT_ASSERT and RT_VERIFY) ----
#define RT_ASSERT_FAIL_(tag, condition, message) \
	do { \
		std::fprintf(stderr, \
			"[%s FAILED] %s\n  File: %s\n  Line: %d\n  Condition: %s\n", \
			(tag), (message), __FILE__, __LINE__, #condition); \
		std::abort(); \
	} while (0)

// ---- RT_ASSERT — Debug only (zero cost in release) ----
#if RT_DEBUG_ACTIVE
#define RT_ASSERT(condition, message) \
	do { \
		if (!(condition)) { \
			RT_ASSERT_FAIL_("RT_ASSERT", condition, message); \
		} \
	} while (0)
#else
#define RT_ASSERT(condition, message) ((void)0)
#endif

// ---- RT_VERIFY — ALWAYS active, even in production ----
// Use for invariants whose violation means data corruption.
// Tiger Style: "keep assertions in production."
#define RT_VERIFY(condition, message) \
	do { \
		if (!(condition)) { \
			RT_ASSERT_FAIL_("RT_VERIFY", condition, message); \
		} \
	} while (0)

// ---- Slow assertion ----
// Only active when BOTH RT_SLOW_CHECKS is defined AND we're in debug mode.
// Use for expensive validations (e.g., verifying entire BVH integrity).
#if defined(RT_SLOW_CHECKS) && RT_DEBUG_ACTIVE
#define RT_SLOW_ASSERT(condition, message) RT_ASSERT(condition, message)
#else
#define RT_SLOW_ASSERT(condition, message) ((void)0)
#endif

// ---- Convenience macros ----

// Validate a Ray struct (catches NaN/Inf early).
#define RT_ASSERT_VALID_RAY(ray) \
	RT_ASSERT((ray).is_valid(), "Invalid ray: NaN/Inf in origin or direction, or t_min > t_max")

// Check that a float is finite (not NaN, not Inf).
#define RT_ASSERT_FINITE(value) \
	RT_ASSERT(std::isfinite(value), "Value is NaN or Inf: " #value)

// Check that a pointer is not null.
#define RT_ASSERT_NOT_NULL(ptr) \
	RT_ASSERT((ptr) != nullptr, "Null pointer: " #ptr)

// Array bounds check (Tiger Style: assert loop bounds).
// Checks 0 <= idx < size.
#define RT_ASSERT_BOUNDS(idx, size) \
	RT_ASSERT((idx) >= 0 && static_cast<size_t>(idx) < static_cast<size_t>(size), \
		"Index out of bounds: " #idx " must be in [0, " #size ")")

// Unsigned overload — only checks idx < size (no sign check needed).
#define RT_ASSERT_BOUNDS_U(idx, size) \
	RT_ASSERT(static_cast<size_t>(idx) < static_cast<size_t>(size), \
		"Index out of bounds: " #idx " must be < " #size)

// Value must be strictly positive (> 0).
#define RT_ASSERT_POSITIVE(val) \
	RT_ASSERT((val) > 0, "Value must be positive: " #val)

// Vector must be approximately unit length (default epsilon = 1e-3).
#define RT_ASSERT_NORMALIZED(vec, eps) \
	RT_ASSERT(std::abs((vec).length_squared() - 1.0f) < (eps), \
		"Vector not normalized: " #vec)

// Code that should never execute (impossible branches, default cases).
// Always active (like RT_VERIFY) since reaching this IS the bug.
#define RT_UNREACHABLE(message) \
	do { \
		std::fprintf(stderr, \
			"[RT_UNREACHABLE] %s\n  File: %s\n  Line: %d\n", \
			(message), __FILE__, __LINE__); \
		std::abort(); \
	} while (0)
