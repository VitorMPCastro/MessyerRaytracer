extends Node3D
## Visibility Layers Demo
##
## Demonstrates how the raytracer respects Godot's VisualInstance3D.layers.
## Three colored boxes are placed on different render layers.
## Debug rays are cast with the Layers draw mode so you can see each layer's
## color.  Press 1-3 to toggle filtering by that layer.
##
## Scene tree expected:
##   Node3D (this script)
##   ├── Camera3D            <-- or created by script
##   ├── RayTracerProbe
##   ├── RayTracerDebug
##   ├── FloorMesh   (Layer 1 -- default)
##   ├── BoxLayer1   (Layer 1)
##   ├── BoxLayer2   (Layer 2)
##   └── BoxLayer3   (Layer 3)
##
## CONTROLS:
##   WASD / Arrow keys — move forward/back/left/right
##   Mouse             — look around (click to capture)
##   Q / E             — move down / up
##   SPACE             — re-cast debug rays from the camera
##   TAB               — cycle debug draw mode
##   1 / 2 / 3         — toggle visibility layer 1/2/3 (quick keys)
##   C                 — clear debug visualization
##   ESC / P           — open settings menu

@onready var probe: RayTracerProbe = $RayTracerProbe
@onready var debug: RayTracerDebug = $RayTracerDebug

@onready var cam: Camera3D = $Camera3D
var menu: BaseMenu
var debug_panel: DebugPanel
var layer_panel: LayerPanel
var tooltip: TooltipOverlay

# Camera movement
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

# Draw mode names for console output.
var mode_names := ["Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH", "Layers"]


func _ready() -> void:
	# ---- Orient camera toward origin ----
	cam.look_at(Vector3(0, 0, 0))
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- Register all MeshInstance3D children with the server ----
	for child in get_children():
		if child is MeshInstance3D:
			RayTracerServer.register_mesh(child)
	RayTracerServer.build()

	# Set up debug node.
	debug.debug_enabled = true
	debug.debug_draw_mode = RayTracerDebug.DRAW_LAYERS

	# ---- Pause menu (modular: debug + layers) ----
	menu = preload("res://demos/ui/base_menu.tscn").instantiate()

	debug_panel = preload("res://demos/ui/debug_panel.tscn").instantiate()
	debug_panel.debug_node = debug
	debug_panel.grid_w = 48
	debug_panel.grid_h = 36
	menu.add_panel(debug_panel)

	layer_panel = preload("res://demos/ui/layer_panel.tscn").instantiate()
	layer_panel.debug_node = debug
	layer_panel.probe_node = probe
	menu.add_panel(layer_panel)

	menu.cast_callback = _cast_rays_from_camera
	menu.set_action_label("Cast Rays")
	add_child(menu)

	tooltip = preload("res://demos/ui/tooltip_overlay.tscn").instantiate()
	tooltip.hint_text = "[Layer Demo]\nWASD / Arrows — Move camera\nMouse — Look around\nQ / E — Down / Up\n\n1 / 2 / 3 — Toggle Layer 1/2/3\nSPACE — Cast debug rays\nTAB — Cycle draw mode\nB — Cycle backend\nC — Clear debug lines\n\nESC / P — Settings menu\nF1 — Toggle this help"
	add_child(tooltip)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	# Initial cast.
	_cast_rays_from_camera()

	print("")
	print("=== Visibility Layers Demo ===")
	print("Controls: WASD=move, Mouse=look, SPACE=cast rays, C=clear")
	print("  TAB    — cycle debug draw mode")
	print("  1/2/3  — toggle visibility Layer 1/2/3")
	print("  ESC/P  — settings menu")
	print("")


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

	if not event is InputEventKey or not event.pressed or event.echo:
		return
	var key := event as InputEventKey

	match key.keycode:
		KEY_1:
			_toggle_layer(0)
		KEY_2:
			_toggle_layer(1)
		KEY_3:
			_toggle_layer(2)
		KEY_TAB:
			_cycle_draw_mode()
		KEY_B:
			_cycle_backend()
		KEY_SPACE:
			_cast_rays_from_camera()
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
	var cam_pos := cam.global_position
	var cam_fwd := -cam.global_basis.z
	debug.cast_debug_rays(cam_pos, cam_fwd, debug_panel.grid_w, debug_panel.grid_h, cam.fov)


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


## Toggle a layer via quick-key (0-indexed) and synchronise with the panel.
func _toggle_layer(idx: int) -> void:
	layer_panel.toggle_layer(idx)
	print("Layer mask: ", _mask_string())


func _mask_string() -> String:
	var parts: Array[String] = []
	for i in range(3):
		if layer_panel.layer_enabled[i]:
			parts.append("Layer%d:ON" % (i + 1))
		else:
			parts.append("Layer%d:off" % (i + 1))
	return " | ".join(parts)
