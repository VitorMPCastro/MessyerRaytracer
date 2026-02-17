// tinybvh_impl.cpp — TinyBVH single-header implementation unit.
//
// WHAT: Instantiates all TinyBVH function bodies. Exactly ONE .cpp file
//       in the project must define TINYBVH_IMPLEMENTATION before including
//       tiny_bvh.h. This is that file.
//
// WHY:  TinyBVH is a single-header library (~9000 lines). Including the
//       implementation in multiple TUs would cause duplicate symbol errors.
//       This compilation unit is the only place where definitions are emitted.
//
// NOTE: We define TINYBVH_INST_IDX_BITS=32 so that tinybvh::Intersection
//       includes a separate `inst` field for instance IDs during TLAS traversal.

#define TINYBVH_INST_IDX_BITS 32
#define TINYBVH_IMPLEMENTATION
#include "thirdparty/tinybvh/tiny_bvh.h"
