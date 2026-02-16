// raytracer_server.cpp -- RayTracerServer singleton implementation.
//
// This file contains:
//   1. Mesh registration: extract object-space triangles from MeshInstance3D
//   2. Scene build: populate TLAS, flatten to RayDispatcher, build BVH, GPU upload
//   3. Ray casting: single and batch dispatch through RayDispatcher
//   4. Backend control: CPU / GPU / Auto selection with lazy GPU init
//   5. Stats exposure for profiling and debugging

#include "raytracer_server.h"
#include "core/asserts.h"

#include <godot_cpp/classes/mesh.hpp>
#include <godot_cpp/classes/engine.hpp>
#include <godot_cpp/classes/material.hpp>
#include <godot_cpp/classes/base_material3d.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector3_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cstdio>
#include <chrono>

using namespace godot;

// ============================================================================
// Singleton
// ============================================================================

RayTracerServer *RayTracerServer::singleton_ = nullptr;

RayTracerServer::RayTracerServer() {
	singleton_ = this;
}

RayTracerServer::~RayTracerServer() {
	singleton_ = nullptr;
}

// ============================================================================
// Godot binding
// ============================================================================

void RayTracerServer::_bind_methods() {
	// ---- Enum constants ----
	BIND_ENUM_CONSTANT(BACKEND_CPU);
	BIND_ENUM_CONSTANT(BACKEND_GPU);
	BIND_ENUM_CONSTANT(BACKEND_AUTO);

	// ---- Scene management ----
	ClassDB::bind_method(D_METHOD("register_mesh", "mesh_instance"), &RayTracerServer::register_mesh);
	ClassDB::bind_method(D_METHOD("unregister_mesh", "mesh_id"), &RayTracerServer::unregister_mesh);
	ClassDB::bind_method(D_METHOD("build"), &RayTracerServer::build);
	ClassDB::bind_method(D_METHOD("clear"), &RayTracerServer::clear);

	// ---- Ray casting ----
	ClassDB::bind_method(D_METHOD("cast_ray", "origin", "direction", "layer_mask"), &RayTracerServer::cast_ray, DEFVAL(0x7FFFFFFF));
	ClassDB::bind_method(D_METHOD("any_hit", "origin", "direction", "max_distance", "layer_mask"), &RayTracerServer::any_hit, DEFVAL(0x7FFFFFFF));

	// ---- Backend ----
	ClassDB::bind_method(D_METHOD("set_backend", "mode"), &RayTracerServer::set_backend);
	ClassDB::bind_method(D_METHOD("get_backend"), &RayTracerServer::get_backend);
	ClassDB::bind_method(D_METHOD("is_gpu_available"), &RayTracerServer::is_gpu_available);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "backend", PROPERTY_HINT_ENUM, "CPU,GPU,Auto"),
		"set_backend", "get_backend");

	// ---- Stats & info ----
	ClassDB::bind_method(D_METHOD("get_last_stats"), &RayTracerServer::get_last_stats);
	ClassDB::bind_method(D_METHOD("get_last_cast_ms"), &RayTracerServer::get_last_cast_ms);
	ClassDB::bind_method(D_METHOD("get_triangle_count"), &RayTracerServer::get_triangle_count);
	ClassDB::bind_method(D_METHOD("get_mesh_count"), &RayTracerServer::get_mesh_count);
	ClassDB::bind_method(D_METHOD("get_bvh_node_count"), &RayTracerServer::get_bvh_node_count);
	ClassDB::bind_method(D_METHOD("get_bvh_depth"), &RayTracerServer::get_bvh_depth);
	ClassDB::bind_method(D_METHOD("get_thread_count"), &RayTracerServer::get_thread_count);
}

// ============================================================================
// Scene management
// ============================================================================

int RayTracerServer::register_mesh(Node *p_node) {
	ERR_FAIL_NULL_V_MSG(p_node, -1, "RayTracerServer::register_mesh: node is null");
	MeshInstance3D *mesh_inst = Object::cast_to<MeshInstance3D>(p_node);
	ERR_FAIL_NULL_V_MSG(mesh_inst, -1, "RayTracerServer::register_mesh: node must be a MeshInstance3D");

	// Extract object-space triangles and material data.
	std::vector<Triangle> tris;
	std::vector<MaterialData> materials;
	std::vector<uint32_t> material_ids;
	std::vector<TriangleUV> uvs;
	std::vector<TriangleNormals> normals;
	_extract_object_triangles(mesh_inst, tris, materials, material_ids, uvs, normals);

	if (tris.empty()) {
		ERR_PRINT("[RayTracerServer] register_mesh: mesh has no triangles");
		return -1;
	}

	// Read the Godot visibility layer mask.
	uint32_t mask = mesh_inst->get_layer_mask();

	std::unique_lock<std::shared_mutex> lock(scene_mutex_);

	// Find a reusable slot or append a new entry.
	int mesh_id = -1;
	for (size_t i = 0; i < meshes_.size(); i++) {
		if (!meshes_[i].valid) {
			mesh_id = static_cast<int>(i);
			break;
		}
	}
	if (mesh_id < 0) {
		mesh_id = static_cast<int>(meshes_.size());
		meshes_.emplace_back();
	}

	RT_ASSERT(mesh_id >= 0, "register_mesh: mesh_id must be non-negative");
	RT_ASSERT(mesh_id < static_cast<int>(meshes_.size()), "register_mesh: mesh_id must be in range");

	RegisteredMesh &entry = meshes_[mesh_id];
	entry.node_id = static_cast<uint64_t>(mesh_inst->get_instance_id());
	entry.object_tris = std::move(tris);
	entry.object_materials = std::move(materials);
	entry.object_material_ids = std::move(material_ids);
	entry.object_triangle_uvs = std::move(uvs);
	entry.object_triangle_normals = std::move(normals);
	entry.layer_mask = mask;
	entry.valid = true;

	scene_dirty_ = true;

	UtilityFunctions::print("[RayTracerServer] Registered mesh id=", mesh_id,
		" (", static_cast<int>(entry.object_tris.size()), " triangles)");

	return mesh_id;
}

void RayTracerServer::unregister_mesh(int mesh_id) {
	ERR_FAIL_COND_MSG(mesh_id < 0 || mesh_id >= static_cast<int>(meshes_.size()),
		"RayTracerServer::unregister_mesh: invalid mesh_id");

	RT_ASSERT_BOUNDS(mesh_id, static_cast<int>(meshes_.size()));
	RT_ASSERT(meshes_[mesh_id].valid, "unregister_mesh: mesh must be currently registered");

	std::unique_lock<std::shared_mutex> lock(scene_mutex_);

	meshes_[mesh_id].valid = false;
	meshes_[mesh_id].object_tris.clear();
	meshes_[mesh_id].node_id = 0;
	scene_dirty_ = true;
}

void RayTracerServer::build() {
	std::unique_lock<std::shared_mutex> lock(scene_mutex_);
	_rebuild_scene();

	RT_ASSERT(!scene_dirty_, "build: scene should not be dirty after rebuild");
	RT_ASSERT(dispatcher_.triangle_count() >= 0, "build: triangle count must be non-negative");

	const auto &sc = dispatcher_.scene();
	const char *backend_names[] = { "CPU", "GPU", "Auto" };
	const char *be = backend_names[static_cast<int>(backend_mode_)];

	if (sc.use_bvh && sc.bvh.is_built()) {
		UtilityFunctions::print("[RayTracerServer] Built scene: ",
			dispatcher_.triangle_count(), " triangles, ",
			dispatcher_.bvh_node_count(), " BVH nodes (depth ",
			dispatcher_.bvh_depth(), ") -- backend: ", be);
	} else {
		UtilityFunctions::print("[RayTracerServer] Built scene: ",
			dispatcher_.triangle_count(), " triangles (brute force) -- backend: ", be);
	}
}

void RayTracerServer::clear() {
	std::unique_lock<std::shared_mutex> lock(scene_mutex_);
	meshes_.clear();
	tlas_.clear();
	dispatcher_.scene().clear();
	scene_dirty_ = true;
}

// ============================================================================
// Ray casting
// ============================================================================

Dictionary RayTracerServer::cast_ray(const Vector3 &origin, const Vector3 &direction,
		int layer_mask) {
	std::shared_lock<std::shared_mutex> lock(scene_mutex_);
	Dictionary result;

	Ray r(origin, direction.normalized());
	uint32_t mask = static_cast<uint32_t>(layer_mask);
	Intersection hit = dispatcher_.cast_ray(r, nullptr, mask);

	result["hit"] = hit.hit();
	result["position"] = hit.position;
	result["normal"] = hit.normal;
	result["distance"] = hit.t;
	result["prim_id"] = static_cast<int>(hit.prim_id);
	result["hit_layers"] = static_cast<int>(hit.hit_layers);

	return result;
}

bool RayTracerServer::any_hit(const Vector3 &origin, const Vector3 &direction,
		float max_distance, int layer_mask) {
	std::shared_lock<std::shared_mutex> lock(scene_mutex_);
	Ray r(origin, direction.normalized());
	r.t_max = max_distance;
	uint32_t mask = static_cast<uint32_t>(layer_mask);
	return dispatcher_.any_hit(r, nullptr, mask);
}

void RayTracerServer::cast_rays_batch(const Ray *rays, Intersection *results,
		int count, RayStats *stats, uint32_t query_mask) {
	std::shared_lock<std::shared_mutex> lock(scene_mutex_);
	dispatcher_.cast_rays(rays, results, count, stats, query_mask);
}

// ============================================================================
// Module API — structured query interface
// ============================================================================

void RayTracerServer::submit(const RayQuery &query, RayQueryResult &result) {
	ERR_FAIL_COND_MSG(query.rays == nullptr || query.count <= 0,
		"RayTracerServer::submit: query has no rays");

	RT_ASSERT_NOT_NULL(query.rays);
	RT_ASSERT(query.count > 0, "submit: query count must be positive");

	std::shared_lock<std::shared_mutex> lock(scene_mutex_);

	auto t0 = std::chrono::steady_clock::now();

	RayStats *stats_ptr = query.collect_stats ? &result.stats : nullptr;

	switch (query.mode) {
		case RayQuery::NEAREST: {
			ERR_FAIL_COND_MSG(result.hits == nullptr,
				"RayTracerServer::submit(NEAREST): result.hits is null");
			dispatcher_.cast_rays(query.rays, result.hits, query.count,
				stats_ptr, query.layer_mask, query.coherent);
			break;
		}
		case RayQuery::ANY_HIT: {
			ERR_FAIL_COND_MSG(result.hit_flags == nullptr,
				"RayTracerServer::submit(ANY_HIT): result.hit_flags is null");
			dispatcher_.any_hit_rays(query.rays, result.hit_flags, query.count,
				stats_ptr, query.layer_mask, query.coherent);
			break;
		}
	}

	auto t1 = std::chrono::steady_clock::now();
	result.elapsed_ms = std::chrono::duration<float, std::milli>(t1 - t0).count();
	result.count = query.count;
}

// ============================================================================
// Backend control
// ============================================================================

void RayTracerServer::set_backend(int mode) {
	if (mode < 0 || mode > static_cast<int>(BACKEND_AUTO)) { return; }
	RT_ASSERT(mode >= 0 && mode <= static_cast<int>(BACKEND_AUTO), "set_backend: mode in valid range");
	backend_mode_ = static_cast<BackendMode>(mode);
	RT_ASSERT(backend_mode_ >= BACKEND_CPU && backend_mode_ <= BACKEND_AUTO,
		"set_backend: backend_mode_ must be valid after assignment");

	switch (backend_mode_) {
		case BACKEND_CPU:
			dispatcher_.set_backend(RayDispatcher::Backend::CPU);
			break;

		case BACKEND_GPU:
			dispatcher_.set_backend(RayDispatcher::Backend::GPU);
			if (!dispatcher_.gpu_available()) {
				if (!dispatcher_.initialize_gpu()) {
					UtilityFunctions::print("[RayTracerServer] GPU init failed -- falling back to CPU");
					dispatcher_.set_backend(RayDispatcher::Backend::CPU);
					backend_mode_ = BACKEND_CPU;
					return;
				}
				dispatcher_.upload_to_gpu();
			}
			break;

		case BACKEND_AUTO:
			dispatcher_.set_backend(RayDispatcher::Backend::AUTO);
			if (!dispatcher_.gpu_available()) {
				dispatcher_.initialize_gpu(); // Best-effort
			}
			break;
	}
}

int RayTracerServer::get_backend() const { return static_cast<int>(backend_mode_); }

bool RayTracerServer::is_gpu_available() const { return dispatcher_.gpu_available(); }

// ============================================================================
// Stats & info
// ============================================================================

Dictionary RayTracerServer::get_last_stats() const {
	RT_ASSERT(last_stats_.hits <= last_stats_.rays_cast || last_stats_.rays_cast == 0,
		"get_last_stats: hits must not exceed rays_cast");
	RT_ASSERT_FINITE(last_cast_ms_);

	Dictionary d;
	d["rays_cast"] = static_cast<int64_t>(last_stats_.rays_cast);
	d["tri_tests"] = static_cast<int64_t>(last_stats_.tri_tests);
	d["bvh_nodes_visited"] = static_cast<int64_t>(last_stats_.bvh_nodes_visited);
	d["hits"] = static_cast<int64_t>(last_stats_.hits);
	d["avg_tri_tests_per_ray"] = last_stats_.avg_tri_tests_per_ray();
	d["avg_nodes_per_ray"] = last_stats_.avg_nodes_per_ray();
	d["hit_rate_percent"] = last_stats_.hit_rate_percent();
	d["cast_time_ms"] = last_cast_ms_;
	return d;
}

float RayTracerServer::get_last_cast_ms() const { return last_cast_ms_; }

int RayTracerServer::get_triangle_count() const { return dispatcher_.triangle_count(); }

int RayTracerServer::get_mesh_count() const {
	int count = 0;
	for (const auto &m : meshes_) {
		if (m.valid) { count++; }
	}
	return count;
}

int RayTracerServer::get_bvh_node_count() const { return dispatcher_.bvh_node_count(); }
int RayTracerServer::get_bvh_depth() const { return dispatcher_.bvh_depth(); }
int RayTracerServer::get_thread_count() const { return static_cast<int>(dispatcher_.thread_count()); }

// ============================================================================
// Internal: extract object-space triangles from a MeshInstance3D
// ============================================================================

void RayTracerServer::_extract_object_triangles(MeshInstance3D *mesh_inst,
		std::vector<Triangle> &out_tris,
		std::vector<MaterialData> &out_materials,
		std::vector<uint32_t> &out_material_ids,
		std::vector<TriangleUV> &out_uvs,
		std::vector<TriangleNormals> &out_normals) {
	RT_ASSERT_NOT_NULL(mesh_inst);

	Ref<Mesh> mesh = mesh_inst->get_mesh();
	if (mesh.is_null()) { return; }

	int surface_count = mesh->get_surface_count();

	for (int surf = 0; surf < surface_count; surf++) {
		Array arrays = mesh->surface_get_arrays(surf);
		if (arrays.size() == 0) { continue; }

		PackedVector3Array vertices = arrays[Mesh::ARRAY_VERTEX];
		if (vertices.size() == 0) { continue; }

		PackedInt32Array indices;
		if (arrays.size() > Mesh::ARRAY_INDEX &&
				arrays[Mesh::ARRAY_INDEX].get_type() == Variant::PACKED_INT32_ARRAY) {
			indices = arrays[Mesh::ARRAY_INDEX];
		}

		// ---- Extract UVs for this surface ----
		PackedVector2Array tex_uvs;
		if (arrays.size() > Mesh::ARRAY_TEX_UV &&
				arrays[Mesh::ARRAY_TEX_UV].get_type() == Variant::PACKED_VECTOR2_ARRAY) {
			tex_uvs = arrays[Mesh::ARRAY_TEX_UV];
		}
		bool has_uvs = tex_uvs.size() > 0;

		// ---- Extract vertex normals for this surface ----
		PackedVector3Array vert_normals;
		if (arrays.size() > Mesh::ARRAY_NORMAL &&
				arrays[Mesh::ARRAY_NORMAL].get_type() == Variant::PACKED_VECTOR3_ARRAY) {
			vert_normals = arrays[Mesh::ARRAY_NORMAL];
		}
		bool has_normals = vert_normals.size() > 0;

		// ---- Extract material for this surface ----
		MaterialData mat;
		Ref<Material> godot_mat = mesh_inst->get_active_material(surf);
		if (godot_mat.is_valid()) {
			BaseMaterial3D *base = Object::cast_to<BaseMaterial3D>(godot_mat.ptr());
			if (base) {
				mat.albedo           = base->get_albedo();
				mat.metallic         = base->get_metallic();
				mat.roughness        = base->get_roughness();
				mat.specular         = base->get_specular();
				mat.emission         = base->get_emission();
				mat.emission_energy  = base->get_emission_energy_multiplier();

				// ---- Extract albedo texture (Phase 2) ----
				Ref<Texture2D> albedo_tex = base->get_texture(BaseMaterial3D::TEXTURE_ALBEDO);
				if (albedo_tex.is_valid()) {
					Ref<Image> img = albedo_tex->get_image();
					if (img.is_valid() && !img->is_empty()) {
						// Decompress if needed so get_pixel() works.
						if (img->is_compressed()) {
							img->decompress();
						}
						mat.albedo_texture      = img;
						mat.tex_width           = img->get_width();
						mat.tex_height          = img->get_height();
						mat.has_albedo_texture  = true;
					}
				}
			}
		}
		uint32_t mat_idx = static_cast<uint32_t>(out_materials.size());
		out_materials.push_back(mat);

		uint32_t base_id = static_cast<uint32_t>(out_tris.size());

		// Object-space: no transform applied. The TLAS instance transform
		// will be applied during scene flattening in _rebuild_scene().
		if (indices.size() > 0) {
			for (int i = 0; i + 2 < indices.size(); i += 3) {
				Vector3 a = vertices[indices[i]];
				Vector3 b = vertices[indices[i + 1]];
				Vector3 c = vertices[indices[i + 2]];
				out_tris.push_back(Triangle(a, b, c, base_id + (i / 3)));
				out_material_ids.push_back(mat_idx);

				TriangleUV tri_uv;
				if (has_uvs) {
					tri_uv.uv0 = tex_uvs[indices[i]];
					tri_uv.uv1 = tex_uvs[indices[i + 1]];
					tri_uv.uv2 = tex_uvs[indices[i + 2]];
				}
				out_uvs.push_back(tri_uv);

				TriangleNormals tri_n;
				if (has_normals) {
					tri_n.n0 = vert_normals[indices[i]];
					tri_n.n1 = vert_normals[indices[i + 1]];
					tri_n.n2 = vert_normals[indices[i + 2]];
				} else {
					// Fallback: use flat face normal for all vertices.
					Vector3 fn = out_tris.back().normal;
					tri_n.n0 = tri_n.n1 = tri_n.n2 = fn;
				}
				out_normals.push_back(tri_n);
			}
		} else {
			for (int i = 0; i + 2 < vertices.size(); i += 3) {
				Vector3 a = vertices[i];
				Vector3 b = vertices[i + 1];
				Vector3 c = vertices[i + 2];
				out_tris.push_back(Triangle(a, b, c, base_id + (i / 3)));
				out_material_ids.push_back(mat_idx);

				TriangleUV tri_uv;
				if (has_uvs) {
					tri_uv.uv0 = tex_uvs[i];
					tri_uv.uv1 = tex_uvs[i + 1];
					tri_uv.uv2 = tex_uvs[i + 2];
				}
				out_uvs.push_back(tri_uv);

				TriangleNormals tri_n;
				if (has_normals) {
					tri_n.n0 = vert_normals[i];
					tri_n.n1 = vert_normals[i + 1];
					tri_n.n2 = vert_normals[i + 2];
				} else {
					Vector3 fn = out_tris.back().normal;
					tri_n.n0 = tri_n.n1 = tri_n.n2 = fn;
				}
				out_normals.push_back(tri_n);
			}
		}
	}
}

// ============================================================================
// Internal: rebuild TLAS + flatten to RayDispatcher scene
// ============================================================================

void RayTracerServer::_rebuild_scene() {
	// 1. Rebuild TLAS from registered meshes.
	//    For each registered mesh:
	//      a) Create a BLAS with stored object-space triangles
	//      b) Build the BLAS BVH
	//      c) Add an instance with the node's current global_transform
	tlas_.clear();

	for (size_t i = 0; i < meshes_.size(); i++) {
		if (!meshes_[i].valid) { continue; }

		// Validate that the node still exists.
		Object *obj = ObjectDB::get_instance(ObjectID(meshes_[i].node_id));
		MeshInstance3D *mesh_inst = obj ? Object::cast_to<MeshInstance3D>(obj) : nullptr;
		if (!mesh_inst) {
			// Node was freed without unregistering -- clean up silently.
			meshes_[i].valid = false;
			continue;
		}

		// Re-read the live visibility layer mask (may have changed since registration).
		meshes_[i].layer_mask = mesh_inst->get_layer_mask();

		// Create BLAS in the TLAS with copy of stored object-space triangles.
		uint32_t blas_id = tlas_.add_mesh();
		MeshBLAS &blas = tlas_.mesh(blas_id);
		blas.triangles = meshes_[i].object_tris;
		tlas_.build_blas(blas_id);

		// Add instance with the node's current world transform.
		Transform3D xform = mesh_inst->get_global_transform();
		tlas_.add_instance(blas_id, xform);
	}

	if (tlas_.instance_count() > 0) {
		tlas_.build_tlas();
	}

	// 2. Flatten all instance triangles to world space for the flat dispatcher.
	//    This preserves SIMD packet traversal on CPU and the existing GPU shader.
	//    Triangles are stamped with their mesh's visibility layer mask.
	dispatcher_.scene().clear();
	scene_materials_.clear();
	scene_material_ids_.clear();
	scene_triangle_uvs_.clear();
	scene_triangle_normals_.clear();

	RT_ASSERT(dispatcher_.scene().triangles.empty(),
		"_rebuild_scene: scene must be empty after clear");

	const auto &instances = tlas_.instances();
	uint32_t tri_offset = 0;

	// Build per-BLAS mappings (layer mask + material info).
	// Each BLAS was added in the same order as the valid meshes above.
	struct BlasInfo {
		uint32_t layer_mask;
		uint32_t material_offset;
		const std::vector<uint32_t> *material_ids;
		const std::vector<TriangleUV> *triangle_uvs;
		const std::vector<TriangleNormals> *triangle_normals;
	};
	std::vector<BlasInfo> blas_info;
	uint32_t mat_offset = 0;
	for (size_t i = 0; i < meshes_.size(); i++) {
		if (!meshes_[i].valid) { continue; }
		BlasInfo bi;
		bi.layer_mask = meshes_[i].layer_mask;
		bi.material_offset = mat_offset;
		bi.material_ids = &meshes_[i].object_material_ids;
		bi.triangle_uvs = &meshes_[i].object_triangle_uvs;
		bi.triangle_normals = &meshes_[i].object_triangle_normals;
		blas_info.push_back(bi);
		for (const auto &m : meshes_[i].object_materials) {
			scene_materials_.push_back(m);
		}
		mat_offset += static_cast<uint32_t>(meshes_[i].object_materials.size());
	}

	for (const auto &inst : instances) {
		const MeshBLAS &blas = tlas_.mesh(inst.blas_id);
		uint32_t inst_layer_mask = (inst.blas_id < blas_info.size())
			? blas_info[inst.blas_id].layer_mask : 0xFFFFFFFF;
		const BlasInfo *bi = (inst.blas_id < blas_info.size())
			? &blas_info[inst.blas_id] : nullptr;
		for (const auto &tri : blas.triangles) {
			Vector3 a = inst.transform.xform(tri.v0);
			Vector3 b = inst.transform.xform(tri.v1);
			Vector3 c = inst.transform.xform(tri.v2);
			Triangle world_tri(a, b, c, tri_offset++, inst_layer_mask);
			dispatcher_.scene().triangles.push_back(world_tri);

			// Map this flattened triangle to its global material index.
			uint32_t global_mat_id = 0;
			if (bi && bi->material_ids && tri.id < bi->material_ids->size()) {
				global_mat_id = (*bi->material_ids)[tri.id] + bi->material_offset;
			}
			scene_material_ids_.push_back(global_mat_id);

			// Map this flattened triangle to its UVs.
			TriangleUV uv;
			if (bi && bi->triangle_uvs && tri.id < bi->triangle_uvs->size()) {
				uv = (*bi->triangle_uvs)[tri.id];
			}
			scene_triangle_uvs_.push_back(uv);

			// Map this flattened triangle to its world-space vertex normals.
			TriangleNormals tn;
			if (bi && bi->triangle_normals && tri.id < bi->triangle_normals->size()) {
				const TriangleNormals &obj_n = (*bi->triangle_normals)[tri.id];
				// Transform normals by the instance's basis (rotation/scale only).
				// For non-uniform scale the correct transform is the inverse-transpose,
				// but uniform scale is the common case and this is close enough.
				Basis normal_basis = inst.transform.basis;
				tn.n0 = normal_basis.xform(obj_n.n0).normalized();
				tn.n1 = normal_basis.xform(obj_n.n1).normalized();
				tn.n2 = normal_basis.xform(obj_n.n2).normalized();
			} else {
				// Fallback: use the world-space face normal.
				Vector3 fn = world_tri.normal;
				tn.n0 = tn.n1 = tn.n2 = fn;
			}
			scene_triangle_normals_.push_back(tn);
		}
	}

	// 3. Build flat BVH (and upload to GPU if the pipeline is ready).
	dispatcher_.build();
	scene_dirty_ = false;

	RT_ASSERT(scene_material_ids_.size() == scene_triangle_uvs_.size(),
		"_rebuild_scene: material_ids and triangle_uvs count must match");
}

// ============================================================================
// Scene shade data accessor
// ============================================================================

SceneShadeData RayTracerServer::get_scene_shade_data() const {
	RT_ASSERT(scene_material_ids_.size() == scene_triangle_uvs_.size(),
		"get_scene_shade_data: material_ids and triangle_uvs count must match");

	SceneShadeData data;
	data.materials      = scene_materials_.data();
	data.material_count = static_cast<int>(scene_materials_.size());
	data.material_ids   = scene_material_ids_.data();
	data.triangle_count = static_cast<int>(scene_material_ids_.size());
	data.triangle_uvs   = scene_triangle_uvs_.data();
	data.triangle_normals = scene_triangle_normals_.data();

	RT_ASSERT(data.material_count >= 0, "get_scene_shade_data: material_count must be non-negative");
	RT_ASSERT(data.triangle_count >= 0, "get_scene_shade_data: triangle_count must be non-negative");

	return data;
}
