# panorama_demo.gd — Demonstrates sky/environment IBL and tone mapping.
#
# WHAT:  Reflective metallic spheres under a procedural sky gradient, with
#        togglable tone mapping modes (Linear, Reinhard, Filmic, ACES).
#        Shows how the raytracer reads sky and ambient parameters from
#        WorldEnvironment — no hardcoded colors.
# WHY:   Validates Phase 1.4 (IBL / Image-Based Lighting) — sky gradient sampling,
#        ambient lighting contribution, and tone mapping pipeline.
#
# SCENE LAYOUT:
#   Node3D (this script)
#   ├── Camera3D
#   ├── DirectionalLight3D   (sun — read by raytracer per frame)
#   ├── WorldEnvironment     (sky, ambient, tone mapping — read per frame)
#   ├── RayRenderer          (traces rays → AOV channels → ImageTexture)
#   ├── Display (CanvasLayer)
#   │   ├── %RenderView      (TextureRect — rendered image)
#   │   └── %HUD             (Label — stats overlay)
#   ├── Floor                (large matte plane)
#   ├── ChromeSphere         (metallic=1, roughness=0.02 — mirror-like)
#   ├── BrushedSphere        (metallic=1, roughness=0.3 — blurry reflection)
#   ├── DielectricSphere     (metallic=0, roughness=0.1 — Fresnel visible)
#   └── MatteSphere          (metallic=0, roughness=0.8 — ambient only)
#
# CONTROLS:
#   WASD / Arrow keys — Move camera
#   Mouse             — Look around (click to capture)
#   Q / E             — Move down / up
#   TAB               — Cycle render channel
#   R                 — Render single frame
#   B                 — Cycle backend (CPU/GPU/Auto)
#   F                 — Toggle auto-render
#   +/-               — Change resolution
#   L                 — Toggle shadows
#   J                 — Toggle anti-aliasing
#   H                 — Toggle render view
#   T                 — Cycle tone mapping mode
#   ESC / P           — Settings menu
#   F1                — Toggle keyboard hints

extends Node3D

@onready var cam: Camera3D = $Camera3D
@onready var tex_rect: TextureRect = %RenderView
@onready var hud_label: Label = %HUD
@onready var world_env: WorldEnvironment = $WorldEnvironment
var renderer  # RayRenderer
var menu: BaseMenu
var renderer_panel: RendererPanel
var tooltip: TooltipOverlay

# Movement.
var move_speed := 5.0
var mouse_sensitivity := 0.002
var pitch := 0.0
var yaw := 0.0
var mouse_captured := false

# Tone mapping cycling.
var current_tonemap := 3  # Start at ACES
var _tonemap_names := ["Linear", "Reinhard", "Filmic", "ACES"]


func _ready() -> void:
	# ---- Camera ----
	pitch = cam.rotation.x
	yaw = cam.rotation.y

	# Sun and sky configured on DirectionalLight3D + WorldEnvironment in the .tscn.
	# RayRenderer reads per frame (Godot-Native principle).

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
		"[Panorama Demo]\n"
		+ "Sky gradient + ambient on reflective spheres\n"
		+ "T — Cycle tone mapping: Linear / Reinhard / Filmic / ACES\n\n"
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

	print("[PanoramaDemo] Registered %d meshes, %d triangles" % [
		count, RayTracerServer.get_triangle_count()])
	var sky_e := 1.0
	if world_env and world_env.environment and world_env.environment.sky:
		var mat = world_env.environment.sky.sky_material
		if mat is ProceduralSkyMaterial:
			sky_e = mat.energy_multiplier
	print("  Tonemap: %s | Sky energy: %.1f" % [
		_tonemap_names[current_tonemap], sky_e])
	print("  Controls: WASD=move, T=tonemap, TAB=channel, R=render, F1=help")

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
			KEY_T:
				_cycle_tonemap()
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


func _cycle_tonemap() -> void:
	current_tonemap = (current_tonemap + 1) % _tonemap_names.size()
	if world_env and world_env.environment:
		world_env.environment.tonemap_mode = current_tonemap
	print("Tonemap: ", _tonemap_names[current_tonemap])
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
	var shade: float = renderer.get_shade_ms()
	var tm: String = _tonemap_names[current_tonemap]
	var aa_info := ""
	if renderer.aa_enabled:
		aa_info = "  AA:%d/%d" % [renderer.get_accumulation_count(), renderer.aa_max_samples]
	hud_label.text = "%s  %dx%d  %.1fms (trace:%.1f shade:%.1f)  [%s]%s" % [
		ch, res.x, res.y, total, trace, shade, tm, aa_info]
