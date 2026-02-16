# rt_graphics_demo.gd — RT Graphics demo: showcases RaySceneSetup + RayRenderer.
#
# Demonstrates the full graphics pipeline from Phases 3-5:
#   - RaySceneSetup managing environment, sky, sun, tonemapping, post-FX
#   - RayRenderer producing ray-traced AOV images
#   - Quality presets (Low / Medium / High / Ultra)
#   - SDFGI, SSAO, glow, fog toggling
#   - Cornell-box inspired scene with varied materials
#
# SCENE LAYOUT:
#   Node3D (this script)
#   ├── Camera3D
#   ├── RaySceneSetup      (manages WorldEnvironment, DirectionalLight3D, Compositor)
#   ├── RayRenderer         (traces rays → AOV channels → ImageTexture)
#   ├── CanvasLayer         (display)
#   │   ├── TextureRect     (rendered image)
#   │   └── Label           (HUD)
#   └── Scene geometry      (floor, walls, spheres, boxes)
#
# CONTROLS:
#   WASD / Arrow keys — Move camera
#   Mouse             — Look around (click to capture)
#   Q / E             — Move down / up
#   TAB               — Cycle render channel
#   1-7               — Select channel directly
#   R                 — Render single frame
#   B                 — Cycle backend (CPU/GPU/Auto)
#   F                 — Toggle auto-render
#   +/-               — Change resolution
#   G                 — Cycle quality preset
#   T                 — Toggle SSAO
#   Y                 — Toggle glow
#   F1                — Toggle keyboard hints
#   ESC / P           — Settings menu

extends Node3D

@onready var cam: Camera3D = $Camera3D
@onready var tex_rect: TextureRect = %RenderView
@onready var hud_label: Label = %HUD
var renderer  # RayRenderer
var scene_setup  # RaySceneSetup
var menu: BaseMenu
var renderer_panel: RendererPanel
var tooltip: TooltipOverlay

# Movement
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

# Scene setup state
var current_preset := 1  # Start at MEDIUM
var _preset_names := ["Low", "Medium", "High", "Ultra"]


func _ready() -> void:
	# ---- Camera ----
	cam.look_at(Vector3(0, 0.5, 0))
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- RaySceneSetup (manages environment, sun, compositor) ----
	scene_setup = $RaySceneSetup
	scene_setup.apply_preset(current_preset)

	# Override some defaults for a nice demo look.
	scene_setup.sun_energy = 1.2
	scene_setup.sun_rotation_degrees = Vector3(-40.0, -25.0, 0.0)
	scene_setup.tonemap_mode = 3  # ACES
	scene_setup.tonemap_exposure = 1.1
	scene_setup.ssao_enabled = true
	scene_setup.ssao_intensity = 1.5
	scene_setup.glow_enabled = true
	scene_setup.glow_intensity = 0.6
	scene_setup.sky_energy = 0.8
	scene_setup.apply()

	# ---- RayRenderer ----
	renderer = $RayRenderer

	# ---- Register meshes ----
	var count := 0
	for mesh_inst in _find_all_meshes(self):
		var id := RayTracerServer.register_mesh(mesh_inst)
		if id >= 0:
			count += 1
	RayTracerServer.build()

	# ---- Pause menu ----
	menu = preload("res://demos/ui/base_menu.tscn").instantiate()
	renderer_panel = preload("res://demos/ui/renderer_panel.tscn").instantiate()
	renderer_panel.renderer_node = renderer
	menu.add_panel(renderer_panel)
	menu.cast_callback = _do_render
	menu.set_action_label("Render Frame")
	add_child(menu)

	# ---- Tooltip overlay ----
	tooltip = preload("res://demos/ui/tooltip_overlay.tscn").instantiate()
	tooltip.hint_text = (
		"[RT Graphics Demo]\n"
		+ "WASD / Arrows — Move camera\n"
		+ "Mouse — Look around\n"
		+ "Q / E — Down / Up\n\n"
		+ "1-7 — Select channel\n"
		+ "TAB — Cycle channel\n"
		+ "R — Render frame\n"
		+ "B — Cycle backend\n"
		+ "F — Toggle auto-render\n"
		+ "+/- — Change resolution\n\n"
		+ "G — Cycle quality preset\n"
		+ "T — Toggle SSAO\n"
		+ "Y — Toggle glow\n\n"
		+ "ESC / P — Settings menu\n"
		+ "F1 — Toggle this help"
	)
	add_child(tooltip)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	print("[RT Graphics Demo] Registered %d meshes, %d triangles" % [
		count, RayTracerServer.get_triangle_count()])
	print("  Preset: %s | SSAO: %s | Glow: %s" % [
		_preset_names[current_preset],
		"ON" if scene_setup.ssao_enabled else "OFF",
		"ON" if scene_setup.glow_enabled else "OFF"])
	print("  Controls: WASD=move, R=render, G=preset, T=SSAO, Y=glow, F1=help")

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

	if event is InputEventMouseMotion and mouse_captured:
		yaw -= event.relative.x * mouse_sensitivity
		pitch -= event.relative.y * mouse_sensitivity
		pitch = clamp(pitch, -PI / 2.0, PI / 2.0)
		cam.rotation = Vector3(pitch, yaw, 0)

	if event is InputEventMouseButton and event.pressed:
		if not mouse_captured:
			Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
			mouse_captured = true

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
			KEY_EQUAL:
				_change_resolution(1)
			KEY_MINUS:
				_change_resolution(-1)
			KEY_G:
				_cycle_preset()
			KEY_T:
				_toggle_ssao()
			KEY_Y:
				_toggle_glow()
			KEY_1: renderer_panel.set_channel(0); _do_render()
			KEY_2: renderer_panel.set_channel(1); _do_render()
			KEY_3: renderer_panel.set_channel(2); _do_render()
			KEY_4: renderer_panel.set_channel(3); _do_render()
			KEY_5: renderer_panel.set_channel(4); _do_render()
			KEY_6: renderer_panel.set_channel(5); _do_render()
			KEY_7: renderer_panel.set_channel(6); _do_render()


func _process(delta: float) -> void:
	if menu.is_open():
		return

	mouse_captured = Input.get_mouse_mode() == Input.MOUSE_MODE_CAPTURED

	var input_dir := Vector3.ZERO
	if Input.is_key_pressed(KEY_W): input_dir.z += 1.0
	if Input.is_key_pressed(KEY_S): input_dir.z -= 1.0
	if Input.is_key_pressed(KEY_A): input_dir.x += 1.0
	if Input.is_key_pressed(KEY_D): input_dir.x -= 1.0
	if Input.is_key_pressed(KEY_E): input_dir.y += 1.0
	if Input.is_key_pressed(KEY_Q): input_dir.y -= 1.0

	if input_dir != Vector3.ZERO:
		var forward := -cam.global_basis.z
		var right := cam.global_basis.x
		var up := Vector3.UP
		var move := (forward * input_dir.z + right * -input_dir.x + up * input_dir.y).normalized()
		cam.global_position += move * move_speed * delta

	if renderer_panel.auto_render:
		_do_render()


func _do_render() -> void:
	renderer.render_frame()
	tex_rect.texture = renderer.get_texture()
	_update_hud()


# ---- Scene setup controls ----

func _cycle_preset() -> void:
	current_preset = (current_preset + 1) % _preset_names.size()
	scene_setup.apply_preset(current_preset)
	print("Quality Preset: ", _preset_names[current_preset])
	_do_render()


func _toggle_ssao() -> void:
	scene_setup.ssao_enabled = not scene_setup.ssao_enabled
	scene_setup.apply()
	print("SSAO: ", "ON" if scene_setup.ssao_enabled else "OFF")
	_do_render()


func _toggle_glow() -> void:
	scene_setup.glow_enabled = not scene_setup.glow_enabled
	scene_setup.apply()
	print("Glow: ", "ON" if scene_setup.glow_enabled else "OFF")
	_do_render()


func _cycle_channel() -> void:
	var names := renderer_panel._channel_names
	var next: int = (renderer.render_channel + 1) % names.size()
	renderer_panel.set_channel(next)
	_do_render()


func _change_resolution(delta: int) -> void:
	var cur := renderer_panel.current_res_index()
	var new_idx: int = clampi(cur + delta, 0, renderer_panel._res_presets.size() - 1)
	renderer_panel.set_resolution_index(new_idx)
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
	var preset: String = _preset_names[current_preset]
	hud_label.text = "%s  %dx%d  %.1fms (gen:%.1f trace:%.1f shade:%.1f conv:%.1f)  [%s]" % [
		ch, res.x, res.y, total, raygen, trace, shade, conv, preset
	]
