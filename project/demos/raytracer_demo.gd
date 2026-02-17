# raytracer_demo.gd — SERVER demo: registers every mesh in the scene globally.
#
# This demo shows how to use RayTracerServer directly for scene-wide ray tracing.
# Meshes can live anywhere in the tree — the script walks the whole scene and
# registers every MeshInstance3D it finds.
#
# SETUP:
#   Node3D (this script)
#   ├── Camera3D            <-- created by script if missing
#   ├── RayTracerDebug      <-- for visualization
#   ├── MeshInstance3D      (floor)
#   ├── MeshInstance3D      (sphere)
#   └── MeshInstance3D      (box)
#
# CONTROLS:
#   WASD / Arrow keys — move forward/back/left/right
#   Mouse             — look around (click to capture)
#   Q / E             — move down / up
#   SPACE             — cast debug rays from the camera
#   TAB               — cycle debug draw mode
#   B                 — cycle backend (CPU/GPU/Auto)
#   C                 — clear debug visualization
#   ESC / P           — open settings menu
#   F1                — toggle keyboard hints

extends Node3D

@onready var cam: Camera3D = $Camera3D
@onready var debug: RayTracerDebug = $RayTracerDebug
var menu: BaseMenu
var debug_panel: DebugPanel
var tooltip: TooltipOverlay

# Movement
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

# Draw mode names for console.
var mode_names := ["Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH", "Layers"]


func _ready() -> void:
	# ---- Orient camera toward origin ----
	cam.look_at(Vector3(0, 0, 0))
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- Register every MeshInstance3D in the entire scene ----
	RayTracerServer.register_scene(self)
	RayTracerServer.build()

	# ---- Pause menu (modular) ----
	menu = preload("res://demos/ui/base_menu.tscn").instantiate()
	debug_panel = preload("res://demos/ui/debug_panel.tscn").instantiate()
	debug_panel.debug_node = debug
	menu.add_panel(debug_panel)
	menu.cast_callback = _cast_rays_from_camera
	menu.set_action_label("Cast Rays")
	add_child(menu)

	tooltip = preload("res://demos/ui/tooltip_overlay.tscn").instantiate()
	tooltip.hint_text = "[Raytracer Demo]\nWASD / Arrows — Move camera\nMouse — Look around\nQ / E — Down / Up\n\nSPACE — Cast debug rays\nTAB — Cycle draw mode\nB — Cycle backend\nC — Clear debug lines\n\nESC / P — Settings menu\nF1 — Toggle this help"
	add_child(tooltip)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	print("[ServerDemo] Registered %d meshes, %d triangles" % [RayTracerServer.get_mesh_count(), RayTracerServer.get_triangle_count()])
	print("Controls: WASD=move, Mouse=look, SPACE=cast rays, TAB=cycle mode, C=clear")
	print("          ESC / P = settings menu")


func _input(event: InputEvent) -> void:
	if menu.is_open():
		return  # All input goes to the menu.

	# Mouse look
	if event is InputEventMouseMotion and mouse_captured:
		yaw -= event.relative.x * mouse_sensitivity
		pitch -= event.relative.y * mouse_sensitivity
		pitch = clamp(pitch, -PI / 2.0, PI / 2.0)
		cam.rotation = Vector3(pitch, yaw, 0)

	# Mouse capture on click
	if event is InputEventMouseButton and event.pressed:
		if not mouse_captured:
			Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
			mouse_captured = true

	# Key actions
	if event is InputEventKey and event.pressed and not event.echo:
		match event.keycode:
			KEY_SPACE:
				_cast_rays_from_camera()
			KEY_TAB:
				_cycle_draw_mode()
			KEY_B:
				_cycle_backend()
			KEY_C:
				debug.clear_debug()
				print("Debug cleared")


func _process(delta: float) -> void:
	if menu.is_open():
		return

	mouse_captured = Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED

	var input_dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W):
		input_dir.z += 1.0
	if Input.is_key_pressed(KEY_S):
		input_dir.z -= 1.0
	if Input.is_key_pressed(KEY_A):
		input_dir.x += 1.0
	if Input.is_key_pressed(KEY_D):
		input_dir.x -= 1.0
	if Input.is_key_pressed(KEY_E):
		input_dir.y += 1.0
	if Input.is_key_pressed(KEY_Q):
		input_dir.y -= 1.0

	if input_dir != Vector3.ZERO:
		var forward := -cam.global_basis.z
		var right := cam.global_basis.x
		var up := Vector3.UP
		var move := (forward * input_dir.z + right * -input_dir.x + up * input_dir.y).normalized()
		cam.global_position += move * move_speed * delta


func _cast_rays_from_camera() -> void:
	var origin := cam.global_position
	var forward := -cam.global_basis.z
	debug.cast_debug_rays(origin, forward, debug_panel.grid_w, debug_panel.grid_h, cam.fov)

	var result = RayTracerServer.cast_ray(origin, forward)
	if result["hit"]:
		print("Center ray hit at: ", result["position"], " normal: ", result["normal"])
	else:
		print("Center ray missed")


func _cycle_draw_mode() -> void:
	var next := (debug.debug_draw_mode + 1) % mode_names.size()
	debug.debug_draw_mode = next
	debug_panel.sync_from_node()
	print("Draw mode: ", mode_names[next])
	_cast_rays_from_camera()


func _cycle_backend() -> void:
	var next := (RayTracerServer.get_backend() + 1) % 3
	RayTracerServer.set_backend(next)
	RayTracerServer.build()
	print("Backend → ", ["CPU", "GPU", "Auto"][next])
	_cast_rays_from_camera()
