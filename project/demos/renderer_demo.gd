# renderer_demo.gd — RENDERER demo: produces ray-traced images with AOV channels.
#
# This demo shows how to use RayRenderer to produce actual rendered images
# from ray tracing. A TextureRect displays the output, and you can cycle
# through all 7 AOV channels with TAB.
#
# SETUP (auto-created by script):
#   Node3D (this script)
#   ├── Camera3D          <-- FPS camera with mouse look
#   ├── RayRenderer       <-- traces rays, produces image
#   ├── CanvasLayer
#   │   ├── TextureRect   <-- displays the rendered image
#   │   └── Label         <-- HUD showing stats
#   └── MeshInstance3D... <-- scene geometry
#
# CONTROLS:
#   WASD / Arrow keys — move forward/back/left/right
#   Mouse             — look around (click to capture)
#   Q / E             — move down / up
#   1-7               — select render channel directly
#   TAB               — cycle render channel
#   R                 — render a frame
#   B                 — cycle backend (CPU/GPU/Auto)
#   F                 — toggle auto-render (freeze)
#   +/-               — increase/decrease resolution
#   ESC / P           — open settings menu
#   F1                — toggle keyboard hints

extends Node3D

@onready var cam: Camera3D = $Camera3D
@onready var tex_rect: TextureRect = %RenderView
@onready var hud_label: Label = %HUD
var renderer  # RayRenderer (GDExtension type resolved at runtime)
var menu: BaseMenu
var renderer_panel: RendererPanel
var tooltip: TooltipOverlay

# Movement
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false


func _ready() -> void:
	# ---- Orient camera toward origin ----
	cam.look_at(Vector3(0, 0, 0))
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- RayRenderer (scene node — properties set in .tscn) ----
	renderer = $RayRenderer

	# ---- Register meshes globally ----
	var count := 0
	for mesh_inst in _find_all_meshes(self):
		var id := RayTracerServer.register_mesh(mesh_inst)
		if id >= 0:
			count += 1
	RayTracerServer.build()

	# ---- Pause menu (modular: renderer panel) ----
	menu = preload("res://demos/ui/base_menu.tscn").instantiate()
	renderer_panel = preload("res://demos/ui/renderer_panel.tscn").instantiate()
	renderer_panel.renderer_node = renderer
	menu.add_panel(renderer_panel)
	menu.cast_callback = _do_render
	menu.set_action_label("Render Frame")
	add_child(menu)

	tooltip = preload("res://demos/ui/tooltip_overlay.tscn").instantiate()
	tooltip.hint_text = "[Renderer Demo]\nWASD / Arrows — Move camera\nMouse — Look around\nQ / E — Down / Up\n\n1-7 — Select channel directly\nTAB — Cycle channel\nR — Render one frame\nB — Cycle backend\nF — Toggle auto-render\n+/- — Change resolution\n\nESC / P — Settings menu\nF1 — Toggle this help"
	add_child(tooltip)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	print("[RendererDemo] Registered %d meshes, %d triangles" % [count, RayTracerServer.get_triangle_count()])
	print("Controls: WASD=move, Mouse=look, R=render, TAB=cycle channel, +/-=resolution")
	print("          1-7=channel, B=backend, F=auto-render toggle, F1=help")
	print("          ESC / P = settings menu")

	# Initial render
	_do_render()


func _find_all_meshes(root: Node) -> Array[MeshInstance3D]:
	var result: Array[MeshInstance3D] = []
	for child in root.get_children():
		if child is MeshInstance3D:
			result.append(child)
		result.append_array(_find_all_meshes(child))
	return result


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
			KEY_TAB:
				_cycle_channel()
			KEY_R:
				_do_render()
			KEY_B:
				_cycle_backend()
			KEY_F:
				renderer_panel.auto_render = not renderer_panel.auto_render
				renderer_panel.sync_from_node()
				print("Auto-render: ", "ON" if renderer_panel.auto_render else "OFF")
			KEY_EQUAL:  # +
				_change_resolution(1)
			KEY_MINUS:  # -
				_change_resolution(-1)
			KEY_1:
				renderer_panel.set_channel(0)
				_do_render()
			KEY_2:
				renderer_panel.set_channel(1)
				_do_render()
			KEY_3:
				renderer_panel.set_channel(2)
				_do_render()
			KEY_4:
				renderer_panel.set_channel(3)
				_do_render()
			KEY_5:
				renderer_panel.set_channel(4)
				_do_render()
			KEY_6:
				renderer_panel.set_channel(5)
				_do_render()
			KEY_7:
				renderer_panel.set_channel(6)
				_do_render()


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

	# Auto-render every frame when enabled
	if renderer_panel.auto_render:
		_do_render()


func _do_render() -> void:
	renderer.render_frame()
	tex_rect.texture = renderer.get_texture()
	_update_hud()


func _cycle_channel() -> void:
	var names := renderer_panel._channel_names
	var next: int = (renderer.render_channel + 1) % names.size()
	renderer_panel.set_channel(next)
	print("Channel: ", names[next])
	_do_render()


func _change_resolution(delta: int) -> void:
	var cur := renderer_panel.current_res_index()
	var new_idx: int = clampi(cur + delta, 0, renderer_panel._res_presets.size() - 1)
	renderer_panel.set_resolution_index(new_idx)
	print("Resolution: ", renderer_panel._res_presets[new_idx])
	_do_render()


func _cycle_backend() -> void:
	var next := (RayTracerServer.get_backend() + 1) % 3
	RayTracerServer.set_backend(next)
	RayTracerServer.build()
	print("Backend → ", ["CPU", "GPU", "Auto"][next])
	_do_render()


func _update_hud() -> void:
	var res: Vector2i = renderer.resolution
	var ch: String = renderer_panel._channel_names[renderer.render_channel]
	var total: float = renderer.get_render_ms()
	var raygen: float = renderer.get_raygen_ms()
	var trace: float = renderer.get_trace_ms()
	var shade: float = renderer.get_shade_ms()
	var conv: float = renderer.get_convert_ms()
	hud_label.text = "%s  %dx%d  %.1fms (gen:%.1f trace:%.1f shade:%.1f conv:%.1f)" % [
		ch, res.x, res.y, total, raygen, trace, shade, conv
	]
