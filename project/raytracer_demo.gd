# raytracer_demo.gd — Attach this script to a Node3D in your scene.
#
# SETUP:
#   1. Create a scene with this structure:
#        Node3D (this script)
#        ├── Camera3D       <-- added by this script if missing
#        └── RayTracerBase
#            ├── MeshInstance3D (e.g., a BoxMesh)
#            ├── MeshInstance3D (e.g., a PlaneMesh as floor)
#            └── MeshInstance3D (e.g., a SphereMesh)
#
# CONTROLS:
#   WASD / Arrow keys — move forward/back/left/right
#   Mouse             — look around (click window first to capture mouse)
#   Q / E             — move down / up
#   SPACE             — cast debug rays from the camera
#   C                 — clear debug visualization
#   ESC               — release mouse / quit

extends Node3D

@onready var raytracer = $RayTracerBase

# Camera node — created automatically if not present.
var cam: Camera3D

# Movement settings
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

func _ready() -> void:
	# Create a Camera3D if one doesn't already exist.
	cam = Camera3D.new()
	cam.name = "DebugCamera"
	cam.position = Vector3(0, 2, -5)
	cam.fov = 75.0
	add_child(cam)
	cam.make_current()

	# Look toward the origin (where your meshes probably are).
	cam.look_at(Vector3(0, 0, 0))
	# Store the initial rotation so WASD works correctly.
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# Capture the mouse for FPS-style look.
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	# Build the triangle scene from all MeshInstance3D children of RayTracerBase.
	raytracer.build_scene()
	print("Scene ready with ", raytracer.get_triangle_count(), " triangles")
	print("Controls: WASD=move, Mouse=look, SPACE=cast rays, C=clear, ESC=release mouse")

func _input(event: InputEvent) -> void:
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
	if event is InputEventKey and event.pressed:
		match event.keycode:
			KEY_SPACE:
				_cast_rays_from_camera()
			KEY_C:
				raytracer.clear_debug()
				print("Debug cleared")
			KEY_ESCAPE:
				if mouse_captured:
					Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
					mouse_captured = false
				else:
					get_tree().quit()

func _process(delta: float) -> void:
	# WASD movement relative to camera facing direction.
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
		# Move along camera's local axes.
		var forward := -cam.global_basis.z
		var right := cam.global_basis.x
		var up := Vector3.UP

		var move := (forward * input_dir.z + right * -input_dir.x + up * input_dir.y).normalized()
		cam.global_position += move * move_speed * delta

func _cast_rays_from_camera() -> void:
	var origin := cam.global_position
	var forward := -cam.global_basis.z  # Camera looks along -Z in local space

	# Cast a 16x12 grid of debug rays with 90° field of view.
	raytracer.cast_debug_rays(origin, forward, 16, 12, 90.0)

	# Also cast a single center ray and print the result.
	var result = raytracer.cast_ray(origin, forward)
	if result["hit"]:
		print("Center ray hit at: ", result["position"], " normal: ", result["normal"])
	else:
		print("Center ray missed")
