#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/engine.hpp>

#include "raytracer_server.h"
#include "raytracer_probe.h"
#include "raytracer_debug.h"
#include "ray_batch.h"
#include "modules/graphics/ray_renderer.h"
#include "modules/graphics/ray_scene_setup.h"
#include "modules/graphics/rt_compositor_base.h"
#include "modules/graphics/rt_reflection_effect.h"

using namespace godot;

void initialize_gdextension_types(ModuleInitializationLevel p_level)
{
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	GDREGISTER_CLASS(RayTracerServer);
	GDREGISTER_CLASS(RayTracerProbe);
	GDREGISTER_CLASS(RayTracerDebug);
	GDREGISTER_CLASS(RayBatch);
	GDREGISTER_CLASS(RayRenderer);
	GDREGISTER_CLASS(RaySceneSetup);
	GDREGISTER_ABSTRACT_CLASS(RTCompositorBase);
	GDREGISTER_CLASS(RTReflectionEffect);

	Engine::get_singleton()->register_singleton("RayTracerServer", memnew(RayTracerServer));
}

void uninitialize_gdextension_types(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	Engine *engine = Engine::get_singleton();
	RayTracerServer *server = RayTracerServer::get_singleton();
	if (engine && server) {
		engine->unregister_singleton("RayTracerServer");
		memdelete(server);
	}
}

extern "C"
{
	// Initialization
	GDExtensionBool GDE_EXPORT raytracer_library_init(GDExtensionInterfaceGetProcAddress p_get_proc_address, GDExtensionClassLibraryPtr p_library, GDExtensionInitialization *r_initialization)
	{
		GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);
		init_obj.register_initializer(initialize_gdextension_types);
		init_obj.register_terminator(uninitialize_gdextension_types);
		init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

		return init_obj.init();
	}
}