extends Node3D
## Example usage of RayTracerServer, RayTracerProbe, and RayTracerDebug.
##
## Scene tree for this script:
##   Node3D (this)
##   ├── RayTracerProbe  (auto_register = true)
##   │   └── MeshInstance3D  (your geometry)
##   └── RayTracerDebug

@onready var probe: RayTracerProbe = $RayTracerProbe
@onready var debug: RayTracerDebug = $RayTracerDebug


func _ready() -> void:
	# Build acceleration structures (probe auto-registered its children).
	RayTracerServer.build()

	# Cast a single ray downward from the probe's position:
	var result := probe.cast_ray(Vector3.DOWN)
	if result["hit"]:
		print("Hit at ", result["position"], " normal=", result["normal"])

	# Visualize a grid of debug rays:
	debug.debug_draw_mode = RayTracerDebug.DRAW_HEATMAP
	debug.cast_debug_rays(
		global_position, -global_basis.z, 16, 12, 60.0
	)
