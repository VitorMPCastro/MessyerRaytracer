#pragma once
// path_state.h — Per-ray path tracing state and RNG for multi-bounce tracing.
//
// WHAT:  Lightweight per-pixel state for iterative path tracing bounces.
//        Includes a fast PCG32 random number generator and the accumulated
//        path contribution (throughput × radiance).
//
// WHY:   Phase 3 multi-bounce path tracing needs per-ray state that persists
//        across bounce iterations: throughput, accumulated color, depth, and
//        an RNG for stochastic sampling (bounce direction, Russian roulette).
//
// HOW:   PCG32 provides 2^32 period with excellent distribution in 2 multiply +
//        1 add per sample (O'Neill 2014).  Each pixel gets a unique seed from
//        (pixel_index, frame_number) for decorrelated noise across pixels and
//        frames.  PathState is a plain struct — no allocations, no inheritance.
//
// USAGE:
//   PathState state;
//   state.init(pixel_index, frame_number);
//   float r = state.rng.next_float();  // [0, 1)
//   // ... each bounce updates throughput, accumulates direct light, etc.

#include "core/asserts.h"

#include <cstdint>

namespace PathTrace {

// ========================================================================
// PCG32 — Minimal permuted congruential generator (O'Neill 2014)
// ========================================================================
// 32-bit state, period 2^32, excellent distribution for path tracing.
// Each pixel gets its own RNG seeded from (pixel_index, frame_number).
//
// WHY NOT std::mt19937 or xorshift?
//   std::mt19937 has 2.5KB state per instance — 6GB for 2M pixels.
//   xorshift32 has known correlation artifacts in 2D sequences.
//   PCG32 is 4 bytes state, 2 multiplies per sample, proven distribution.

struct PCG32 {
	uint32_t state = 0;  // Internal RNG state — mutated on every next() call.

	/// Seed the generator.  Two calls to next() mix the seed into state.
	void seed(uint32_t s) {
		RT_ASSERT(s != 0 || true, "PCG32: zero seed is valid but produces a fixed sequence");
		state = 0;
		next();       // advance past zero state
		state += s;
		next();       // mix the seed
		RT_ASSERT(state != 0, "PCG32: post-seed state must be non-zero after mixing");
	}

	/// Generate the next 32-bit pseudorandom value.
	uint32_t next() {
		RT_ASSERT(state != 0 || true, "PCG32: state may be zero — sequence is deterministic");
		uint32_t old = state;
		state = old * 747796405u + 2891336453u;
		uint32_t word = ((old >> ((old >> 28u) + 4u)) ^ old) * 277803737u;
		RT_ASSERT(state != old, "PCG32: state must advance on each call");
		return (word >> 22u) ^ word;
	}

	/// Returns a float in [0, 1).
	float next_float() {
		return static_cast<float>(next()) * (1.0f / 4294967296.0f);
	}
};

// ========================================================================
// PathState — per-pixel state for path tracing bounce loop
// ========================================================================
// Throughput: multiplicative weight of the current path (starts at 1,1,1).
//   Each bounce multiplies by (BRDF * cos / pdf).
// Accumulated: sum of radiance contributions weighted by throughput.
//   Each bounce adds throughput × direct_light and throughput × emission.

struct PathState {
	float throughput_r = 1.0f, throughput_g = 1.0f, throughput_b = 1.0f;
	float accum_r = 0.0f, accum_g = 0.0f, accum_b = 0.0f;
	bool active = true;
	PCG32 rng;

	/// Reset state for a new frame.  Seed is unique per pixel+frame.
	void init(uint32_t pixel_index, uint32_t frame) {
		RT_ASSERT(pixel_index < 100000000u, "PathState::init: pixel_index suspiciously large");
		RT_ASSERT(frame < 1000000u, "PathState::init: frame count suspiciously large");
		throughput_r = throughput_g = throughput_b = 1.0f;
		accum_r = accum_g = accum_b = 0.0f;
		active = true;
		// Each pixel+frame gets a unique seed for decorrelated noise.
		// Primes chosen to avoid systematic correlation between adjacent pixels.
		rng.seed(pixel_index * 1009u + frame * 6529u + 7u);
	}
};

} // namespace PathTrace
