# gi_comparison_demo.gd — A/B comparison of GI modes: None, SDFGI, and VoxelGI.
#
# WHAT:  A room scene with objects that receive indirect illumination.
#        Cycles through GI_NONE, GI_SDFGI, and GI_VOXEL_GI on RaySceneSetup
#        so the user can compare rasterizer-side GI quality and our raytracer's
#        rendering of the same geometry side-by-side.
# WHY:   Validates the VoxelGI hybrid auto-create/bake implementation.
#        Shows how GI mode affects Godot's viewport (rasterizer) while our
#        raytracer reads lights and environment per-frame regardless of GI mode.
#
# SCENE LAYOUT:
#   Node3D (this script)
#   ├── Camera3D
#   ├── RaySceneSetup       (manages WorldEnvironment, sun, GI)
#   ├── RayRenderer          (traces rays → AOV channels → ImageTexture)
#   ├── Display (CanvasLayer)
#   │   ├── %RenderView      (TextureRect — rendered image)
#   │   └── %HUD             (Label — stats overlay)
#   ├── Room                 (room_box.obj — enclosed room for GI bounces)
#   ├── SphereLeft           (matte white — shows indirect color bleed)
#   ├── SphereRight          (chrome — shows GI in reflections)
#   ├── RedWall              (red box — color bleed source, left)
#   └── GreenWall            (green box — color bleed source, right)
#
# CONTROLS:
#   WASD / Arrow keys — Move camera
#   Mouse             — Look around (click to capture)
#   Q / E             — Move down / up
#   G                 — Cycle GI mode (None → SDFGI → VoxelGI → …)
#   TAB               — Cycle render channel
#   R                 — Render single frame
#   B                 — Cycle backend (CPU/GPU/Auto)
#   F                 — Toggle auto-render
#   +/-               — Change resolution
#   L                 — Toggle shadows
#   J                 — Toggle anti-aliasing
#   H                 — Toggle render view
#   ESC / P           — Settings menu
#   F1                — Toggle keyboard hints

extends Node3D

@onready var cam: Camera3D = $Camera3D
@onready var tex_rect: TextureRect = %RenderView
@onready var hud_label: Label = %HUD
var renderer  # RayRenderer
var scene_setup  # RaySceneSetup
var menu: BaseMenu
var renderer_panel: RendererPanel
var tooltip: TooltipOverlay

# GI mode names for display.
const GI_MODE_NAMES := ["None", "SDFGI", "VoxelGI"]

# Movement.
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false


func _ready() -> void:
	# ---- Camera ----
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# ---- Nodes ----
	renderer = $RayRenderer
	scene_setup = $RaySceneSetup

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
		"[GI Comparison Demo]\n"
		+ "A/B comparison of GI modes on rasterizer\n"
		+ "Raytracer reads lights per-frame regardless\n\n"
		+ "G — Cycle GI mode (None / SDFGI / VoxelGI)\n"
		+ "WASD / Arrows — Move camera\n"
		+ "Mouse — Look around\n"
		+ "Q / E — Down / Up\n\n"
		+ "TAB — Cycle channel\n"
		+ "R — Render frame\n"
		+ "B — Cycle backend\n"
		+ "F — Toggle auto-render\n"
		+ "+/- — Change resolution\n"
		+ "L — Toggle shadows\n"
		+ "J — Toggle anti-aliasing\n"
		+ "H — Toggle render view\n\n"
		+ "ESC / P — Settings menu\n"
		+ "F1 — Toggle this help"
	)
	add_child(tooltip)

	# ---- Mouse capture ----
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	mouse_captured = true

	print("[GIComparisonDemo] Registered %d meshes, %d triangles" % [
		count, RayTracerServer.get_triangle_count()])
	print("  GI mode: %s (press G to cycle)" % GI_MODE_NAMES[scene_setup.gi_mode])
	print("  Controls: WASD=move, G=cycle GI, TAB=channel, R=render, F1=help")

	# Initial render.
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
			KEY_G:
				_cycle_gi_mode()
			KEY_TAB:
				_cycle_channel()
			KEY_R:
				_do_render()
			KEY_B:
				_cycle_backend()
			KEY_F:
				renderer_panel.auto_render = not renderer_panel.auto_render
				renderer_panel.sync_from_node()
			KEY_L:
				renderer.shadows_enabled = not renderer.shadows_enabled
				renderer_panel.sync_from_node()
				print("Shadows: ", "ON" if renderer.shadows_enabled else "OFF")
				_do_render()
			KEY_J:
				renderer.aa_enabled = not renderer.aa_enabled
				renderer_panel.sync_from_node()
				print("Anti-Aliasing: ", "ON" if renderer.aa_enabled else "OFF")
				_do_render()
			KEY_H:
				tex_rect.visible = not tex_rect.visible
			KEY_EQUAL:
				_change_resolution(1)
			KEY_MINUS:
				_change_resolution(-1)


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


func _cycle_gi_mode() -> void:
	# Cycle: 0 (None) → 1 (SDFGI) → 2 (VoxelGI) → 0 …
	var next_mode: int = (scene_setup.gi_mode + 1) % 3
	scene_setup.gi_mode = next_mode
	scene_setup.apply()
	print("[GIComparisonDemo] GI mode → %s" % GI_MODE_NAMES[next_mode])
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
	var trace: float = renderer.get_trace_ms()
	var shadow: float = renderer.get_shadow_ms()
	var shade: float = renderer.get_shade_ms()
	var gi_name: String = GI_MODE_NAMES[scene_setup.gi_mode]
	var aa_info := ""
	if renderer.aa_enabled:
		aa_info = "  AA:%d/%d" % [renderer.get_accumulation_count(), renderer.aa_max_samples]
	hud_label.text = "[GI:%s]  %s  %dx%d  %.1fms (trace:%.1f shadow:%.1f shade:%.1f)%s" % [
		gi_name, ch, res.x, res.y, total, trace, shadow, shade, aa_info]
