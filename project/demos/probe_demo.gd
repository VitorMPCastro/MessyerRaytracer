# probe_demo.gd — PROBE demo: registers only meshes under the probe node.
#
# This demo shows how to use RayTracerProbe for localized ray tracing.
# Only MeshInstance3D children of the probe are registered — meshes outside
# the probe subtree are invisible to rays, even if they're in the scene.
#
# SETUP:
#   Node3D (this script)
#   ├── Camera3D             <-- created by script if missing
#   ├── RayTracerDebug       <-- for visualization
#   ├── RayTracerProbe       <-- auto_register = true
#   │   ├── MeshInstance3D   (floor)
#   │   ├── MeshInstance3D   (sphere)
#   │   └── MeshInstance3D   (box)
#   └── MeshInstance3D       <-- NOT registered (outside probe!)
#
# CONTROLS:
#   WASD / Arrow keys — move forward/back/left/right
#   Mouse             — look around (click to capture)
#   Q / E             — move down / up
#   SPACE             — cast debug rays from the camera
#   TAB               — cycle debug draw mode
#   R                 — cast a single ray from the probe's position (down)
#   C                 — clear debug visualization
#   ESC / P           — open settings menu

extends Node3D

@onready var probe: RayTracerProbe = $RayTracerProbe
@onready var debug: RayTracerDebug = $RayTracerDebug

var cam: Camera3D
var menu: PauseMenu

# Movement
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

# Draw mode names for console.
var mode_names := ["Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH", "Layers"]


func _ready() -> void:
	# ---- Camera ----
	cam = Camera3D.new()
	cam.name = "DebugCamera"
	cam.position = Vector3(0, 2, -5)
	cam.fov = 75.0
	add_child(cam)
	cam.make_current()
	cam.look_at(Vector3(0, 0, 0))
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- Build (Probe auto-registered its children on _ready) ----
	RayTracerServer.build()

	# ---- Pause menu ----
	menu = PauseMenu.new()
	menu.debug_node = debug
	menu.cast_callback = _cast_rays_from_camera
	add_child(menu)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	print("[ProbeDemo] %d triangles registered (only meshes under the probe)" % RayTracerServer.get_triangle_count())
	print("Controls: WASD=move, Mouse=look, SPACE=cast grid, TAB=cycle mode, R=probe ray, C=clear")
	print("          ESC / P = settings menu")
	print("NOTE: The red cone is OUTSIDE the probe — rays pass right through it!")


func _input(event: InputEvent) -> void:
	if menu.is_open():
		return

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
			KEY_R:
				_cast_probe_ray()
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
	debug.cast_debug_rays(origin, forward, menu.grid_w, menu.grid_h, cam.fov)


func _cycle_draw_mode() -> void:
	var next := (debug.debug_draw_mode + 1) % mode_names.size()
	debug.debug_draw_mode = next
	print("Draw mode: ", mode_names[next])
	_cast_rays_from_camera()


## Cast a single ray from the probe's position straight down.
func _cast_probe_ray() -> void:
	var result := probe.cast_ray(Vector3.DOWN)
	if result["hit"]:
		print("[Probe] Hit at ", result["position"], " (dist=%.2f)" % result["distance"])
	else:
		print("[Probe] Miss (no geometry below probe)")
