class_name PauseMenu
extends CanvasLayer
## @deprecated — Use the modular UI system in demos/ui/ instead.
##
## This monolithic menu has been replaced by:
##   - BaseMenu      (demos/ui/base_menu.tscn)  — shared chrome
##   - DebugPanel    (demos/ui/debug_panel.tscn) — debug controls
##   - RendererPanel (demos/ui/renderer_panel.tscn) — renderer controls
##   - LayerPanel    (demos/ui/layer_panel.tscn) — layer toggles
##   - TooltipOverlay(demos/ui/tooltip_overlay.tscn) — F1 hints
##
## Kept for reference only.  All demos now use the modular panels.
##
## OLD USAGE:
##   var menu := PauseMenu.new()
##   menu.debug_node = $RayTracerDebug
##   menu.cast_callback = _cast_rays_from_camera
##   add_child(menu)

## The RayTracerDebug node to control.
var debug_node: RayTracerDebug

## The RayRenderer node to control (set by renderer_demo).
var renderer_node  # RayRenderer (GDExtension type resolved at runtime)

## Extra per-demo options toggle (layer_demo sets this).
var layer_demo_mode := false

## Layer enable state (3 bools) — only used in layer_demo_mode.
var layer_enabled := [true, true, true]

## Probe node — set by layer_demo for updating layer_mask.
var probe_node: RayTracerProbe

## Called after any setting change so the demo can re-cast rays.
var cast_callback: Callable

## Ray grid width / height — demos read these.
var grid_w := 16
var grid_h := 12

# ── Internal UI references ──────────────────────────────────────────────────
var _dim: ColorRect
var _root: ScrollContainer   # direct child of CanvasLayer — holds panel
var _panel: PanelContainer
var _visible := false

# Option buttons / labels
var _backend_btn: OptionButton
var _mode_btn: OptionButton
var _grid_w_spin: SpinBox
var _grid_h_spin: SpinBox
var _miss_len_spin: SpinBox
var _normal_len_spin: SpinBox
var _heat_dist_spin: SpinBox
var _heat_cost_spin: SpinBox
var _bvh_depth_spin: SpinBox
var _layer_checks: Array[CheckBox] = []
var _stats_label: Label
var _gpu_label: Label

# Renderer controls
var _channel_btn: OptionButton
var _res_btn: OptionButton
var _pos_spin: SpinBox
var _auto_render_check: CheckBox

# backend / draw mode name arrays
var _backend_names := ["CPU", "GPU", "Auto"]
var _mode_names := ["Rays", "Normals", "Distance", "Heatmap", "Overheat", "BVH", "Layers"]
var _channel_names: Array[String] = ["Color", "Normal", "Depth", "Barycentric", "Position", "PrimID", "HitMask"]
var _res_presets: Array[Vector2i] = [
	Vector2i(160, 120), Vector2i(320, 240), Vector2i(640, 480),
	Vector2i(960, 720), Vector2i(1280, 960),
]


func _ready() -> void:
	layer = 100  # Draw on top of everything.
	_build_ui()
	_dim.visible = false
	_root.visible = false


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_ESCAPE or event.keycode == KEY_P:
			toggle_menu()
			get_viewport().set_input_as_handled()


func toggle_menu() -> void:
	_visible = not _visible
	_dim.visible = _visible
	_root.visible = _visible
	if _visible:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		_refresh_stats()
	else:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)


func is_open() -> bool:
	return _visible


# ── Build the entire UI tree programmatically ────────────────────────────────

func _build_ui() -> void:
	# ── Background dim (direct child of CanvasLayer) ──
	_dim = ColorRect.new()
	_dim.color = Color(0, 0, 0, 0.45)
	_dim.set_anchors_preset(Control.PRESET_FULL_RECT)
	_dim.mouse_filter = Control.MOUSE_FILTER_IGNORE
	add_child(_dim)

	# ── Scrollable root (direct child of CanvasLayer — sibling of dim) ──
	# A full-rect ScrollContainer lets the panel scroll on small screens
	# and keeps mouse coordinates correct (no nested anchor offsets).
	_root = ScrollContainer.new()
	_root.set_anchors_preset(Control.PRESET_FULL_RECT)
	_root.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	add_child(_root)

	# ── CenterContainer inside scroll — centres the panel ──
	var center := CenterContainer.new()
	center.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	center.size_flags_vertical = Control.SIZE_EXPAND_FILL
	center.custom_minimum_size = _root.get_viewport_rect().size  # at least viewport-sized
	_root.add_child(center)

	# ── Panel ──
	_panel = PanelContainer.new()
	_panel.custom_minimum_size = Vector2(420, 0)

	# Dark semi-transparent StyleBox
	var style := StyleBoxFlat.new()
	style.bg_color = Color(0.10, 0.10, 0.12, 0.94)
	style.corner_radius_top_left = 8
	style.corner_radius_top_right = 8
	style.corner_radius_bottom_left = 8
	style.corner_radius_bottom_right = 8
	style.content_margin_left = 18
	style.content_margin_right = 18
	style.content_margin_top = 14
	style.content_margin_bottom = 14
	style.border_width_left = 1
	style.border_width_right = 1
	style.border_width_top = 1
	style.border_width_bottom = 1
	style.border_color = Color(0.35, 0.45, 0.65, 0.6)
	_panel.add_theme_stylebox_override("panel", style)

	center.add_child(_panel)

	var vbox := VBoxContainer.new()
	vbox.add_theme_constant_override("separation", 6)
	_panel.add_child(vbox)

	# ── Title ──
	var title := Label.new()
	title.text = "⚙  Raytracer Settings"
	title.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	title.add_theme_font_size_override("font_size", 20)
	vbox.add_child(title)

	_add_separator(vbox)

	# ── Backend ──
	var backend_row := _make_row("Backend")
	vbox.add_child(backend_row)
	_backend_btn = OptionButton.new()
	for bn in _backend_names:
		_backend_btn.add_item(bn)
	_backend_btn.selected = RayTracerServer.get_backend()
	_backend_btn.item_selected.connect(_on_backend_changed)
	backend_row.add_child(_backend_btn)

	_gpu_label = Label.new()
	_gpu_label.add_theme_font_size_override("font_size", 12)
	_gpu_label.add_theme_color_override("font_color", Color(0.6, 0.6, 0.6))
	_refresh_gpu_label()
	vbox.add_child(_gpu_label)

	# ══════════════════════════════════════════════════════════════════════
	# ── Renderer Section (only when renderer_node is set) ──
	# ══════════════════════════════════════════════════════════════════════
	if renderer_node:
		_add_separator(vbox)
		var render_title := Label.new()
		render_title.text = "🎨  Renderer"
		render_title.add_theme_font_size_override("font_size", 16)
		vbox.add_child(render_title)

		# Channel selector
		var ch_row := _make_row("Channel")
		vbox.add_child(ch_row)
		_channel_btn = OptionButton.new()
		for cn in _channel_names:
			_channel_btn.add_item(cn)
		_channel_btn.selected = renderer_node.render_channel
		_channel_btn.item_selected.connect(_on_channel_changed)
		ch_row.add_child(_channel_btn)

		# Resolution selector
		var res_row := _make_row("Resolution")
		vbox.add_child(res_row)
		_res_btn = OptionButton.new()
		for i in range(_res_presets.size()):
			var r: Vector2i = _res_presets[i]
			_res_btn.add_item("%dx%d" % [r.x, r.y])
		# Find current index
		var cur_res: Vector2i = renderer_node.resolution
		for i in range(_res_presets.size()):
			if _res_presets[i] == cur_res:
				_res_btn.selected = i
				break
		_res_btn.item_selected.connect(_on_resolution_changed)
		res_row.add_child(_res_btn)

		# Position range
		var pos_row := _make_row("Pos. Range")
		vbox.add_child(pos_row)
		_pos_spin = _make_spin(0.1, 10000.0, renderer_node.position_range, 1.0)
		_pos_spin.value_changed.connect(func(v: float):
			renderer_node.position_range = v; _fire_cast())
		pos_row.add_child(_pos_spin)

		# Auto-render toggle
		_auto_render_check = CheckBox.new()
		_auto_render_check.text = "Auto-render every frame"
		_auto_render_check.button_pressed = true
		_auto_render_check.toggled.connect(_on_auto_render_toggled)
		vbox.add_child(_auto_render_check)

	_add_separator(vbox)

	# ══════════════════════════════════════════════════════════════════════
	# ── Debug Section (only when debug_node is set) ──
	# ══════════════════════════════════════════════════════════════════════
	if debug_node:
		var debug_title := Label.new()
		debug_title.text = "🔍  Debug Visualizer"
		debug_title.add_theme_font_size_override("font_size", 16)
		vbox.add_child(debug_title)

		var mode_row := _make_row("Draw Mode")
		vbox.add_child(mode_row)
		_mode_btn = OptionButton.new()
		for mn in _mode_names:
			_mode_btn.add_item(mn)
		_mode_btn.selected = debug_node.debug_draw_mode
		_mode_btn.item_selected.connect(_on_mode_changed)
		mode_row.add_child(_mode_btn)

		# ── Grid Resolution ──
		var grid_row := _make_row("Grid W×H")
		vbox.add_child(grid_row)
		_grid_w_spin = _make_spin(4, 128, grid_w, 4)
		_grid_w_spin.value_changed.connect(func(v: float): grid_w = int(v); _fire_cast())
		grid_row.add_child(_grid_w_spin)
		var x_label := Label.new()
		x_label.text = "×"
		grid_row.add_child(x_label)
		_grid_h_spin = _make_spin(4, 96, grid_h, 4)
		_grid_h_spin.value_changed.connect(func(v: float): grid_h = int(v); _fire_cast())
		grid_row.add_child(_grid_h_spin)

		_add_separator(vbox)

		# ── Ray Settings ──
		var miss_row := _make_row("Miss Length")
		vbox.add_child(miss_row)
		_miss_len_spin = _make_spin(0.5, 200, debug_node.debug_ray_miss_length, 0.5)
		_miss_len_spin.value_changed.connect(func(v: float):
			if debug_node: debug_node.debug_ray_miss_length = v)
		miss_row.add_child(_miss_len_spin)

		var norm_row := _make_row("Normal Length")
		vbox.add_child(norm_row)
		_normal_len_spin = _make_spin(0.01, 5.0, debug_node.debug_normal_length, 0.05)
		_normal_len_spin.value_changed.connect(func(v: float):
			if debug_node: debug_node.debug_normal_length = v)
		norm_row.add_child(_normal_len_spin)

		_add_separator(vbox)

		# ── Heatmap Settings ──
		var hdist_row := _make_row("Heatmap Dist")
		vbox.add_child(hdist_row)
		_heat_dist_spin = _make_spin(1, 500, debug_node.debug_heatmap_max_distance, 5)
		_heat_dist_spin.value_changed.connect(func(v: float):
			if debug_node: debug_node.debug_heatmap_max_distance = v)
		hdist_row.add_child(_heat_dist_spin)

		var hcost_row := _make_row("Heatmap Cost")
		vbox.add_child(hcost_row)
		_heat_cost_spin = _make_spin(1, 500, debug_node.debug_heatmap_max_cost, 5)
		_heat_cost_spin.value_changed.connect(func(v: float):
			if debug_node: debug_node.debug_heatmap_max_cost = int(v))
		hcost_row.add_child(_heat_cost_spin)

		# ── BVH Depth ──
		var bvh_row := _make_row("BVH Depth")
		vbox.add_child(bvh_row)
		_bvh_depth_spin = _make_spin(-1, 32, debug_node.debug_bvh_depth, 1)
		_bvh_depth_spin.value_changed.connect(func(v: float):
			if debug_node: debug_node.debug_bvh_depth = int(v))
		bvh_row.add_child(_bvh_depth_spin)

	# ── Visibility Layers (layer_demo only) ──
	if layer_demo_mode:
		_add_separator(vbox)
		var layer_title := Label.new()
		layer_title.text = "Visibility Layers"
		layer_title.add_theme_font_size_override("font_size", 14)
		vbox.add_child(layer_title)

		var layer_row := HBoxContainer.new()
		layer_row.add_theme_constant_override("separation", 12)
		vbox.add_child(layer_row)

		for i in range(3):
			var cb := CheckBox.new()
			cb.text = "Layer %d" % (i + 1)
			cb.button_pressed = layer_enabled[i]
			var idx := i  # capture
			cb.toggled.connect(func(on: bool): _on_layer_toggled(idx, on))
			layer_row.add_child(cb)
			_layer_checks.append(cb)

	_add_separator(vbox)

	# ── Stats ──
	_stats_label = Label.new()
	_stats_label.add_theme_font_size_override("font_size", 12)
	_stats_label.add_theme_color_override("font_color", Color(0.75, 0.85, 0.75))
	vbox.add_child(_stats_label)

	# ── Buttons row ──
	var btn_row := HBoxContainer.new()
	btn_row.alignment = BoxContainer.ALIGNMENT_CENTER
	btn_row.add_theme_constant_override("separation", 12)
	vbox.add_child(btn_row)

	if debug_node:
		var cast_btn := Button.new()
		cast_btn.text = "  Cast Rays  "
		cast_btn.pressed.connect(func(): _fire_cast())
		btn_row.add_child(cast_btn)

		var clear_btn := Button.new()
		clear_btn.text = "  Clear  "
		clear_btn.pressed.connect(func():
			if debug_node: debug_node.clear_debug())
		btn_row.add_child(clear_btn)

	if renderer_node:
		var render_btn := Button.new()
		render_btn.text = "  Render Frame  "
		render_btn.pressed.connect(func(): _fire_cast())
		btn_row.add_child(render_btn)

	var close_btn := Button.new()
	close_btn.text = "  Resume  "
	close_btn.pressed.connect(func(): toggle_menu())
	btn_row.add_child(close_btn)

	var quit_btn := Button.new()
	quit_btn.text = "  Quit  "
	quit_btn.pressed.connect(func(): get_tree().quit())
	btn_row.add_child(quit_btn)

	# ── Footer hint ──
	var hint := Label.new()
	hint.text = "ESC / P to toggle this menu"
	hint.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	hint.add_theme_font_size_override("font_size", 11)
	hint.add_theme_color_override("font_color", Color(0.5, 0.5, 0.5))
	vbox.add_child(hint)


# ── Helpers ──────────────────────────────────────────────────────────────────

func _make_row(label_text: String) -> HBoxContainer:
	var row := HBoxContainer.new()
	row.add_theme_constant_override("separation", 8)
	var lbl := Label.new()
	lbl.text = label_text
	lbl.custom_minimum_size.x = 120
	row.add_child(lbl)
	return row


func _make_spin(min_val: float, max_val: float, value: float, step: float) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = min_val
	spin.max_value = max_val
	spin.step = step
	spin.value = value
	spin.custom_minimum_size.x = 80
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return spin


func _add_separator(parent: Control) -> void:
	var sep := HSeparator.new()
	sep.add_theme_constant_override("separation", 4)
	parent.add_child(sep)


# ── Callbacks ────────────────────────────────────────────────────────────────

func _on_backend_changed(idx: int) -> void:
	RayTracerServer.set_backend(idx)
	# GPU needs a rebuild.
	RayTracerServer.build()
	_refresh_gpu_label()
	_fire_cast()
	print("[Menu] Backend → ", _backend_names[idx])


func _on_mode_changed(idx: int) -> void:
	if debug_node:
		debug_node.debug_draw_mode = idx
	_fire_cast()


func _on_channel_changed(idx: int) -> void:
	if renderer_node:
		renderer_node.render_channel = idx
	_fire_cast()


func _on_resolution_changed(idx: int) -> void:
	if renderer_node and idx >= 0 and idx < _res_presets.size():
		renderer_node.resolution = _res_presets[idx]
	_fire_cast()


func _on_auto_render_toggled(on: bool) -> void:
	# The demo script reads this via menu.auto_render_enabled
	auto_render_enabled = on


## Public flag — demo scripts read this to gate continuous rendering.
var auto_render_enabled := true


func _on_layer_toggled(layer_idx: int, on: bool) -> void:
	layer_enabled[layer_idx] = on
	var mask := 0
	for i in range(3):
		if layer_enabled[i]:
			mask |= (1 << i)
	if debug_node:
		debug_node.debug_layer_mask = mask
	if probe_node:
		probe_node.layer_mask = mask
	_fire_cast()


func _fire_cast() -> void:
	if cast_callback.is_valid():
		cast_callback.call()
	_refresh_stats()


func _refresh_stats() -> void:
	if not _stats_label:
		return
	var tri := RayTracerServer.get_triangle_count()
	var meshes := RayTracerServer.get_mesh_count()
	var depth := RayTracerServer.get_bvh_depth()
	var threads := RayTracerServer.get_thread_count()
	var gpu_str := "Yes" if RayTracerServer.is_gpu_available() else "No"
	var ms := 0.0
	if debug_node:
		ms = debug_node.get_last_cast_ms()
	elif renderer_node:
		ms = renderer_node.get_render_ms()
	_stats_label.text = "%d tris | %d meshes | BVH depth %d | %d threads | GPU: %s | Last: %.2f ms" % [
		tri, meshes, depth, threads, gpu_str, ms]


func _refresh_gpu_label() -> void:
	if not _gpu_label:
		return
	if RayTracerServer.is_gpu_available():
		_gpu_label.text = "  GPU compute available ✓"
		_gpu_label.add_theme_color_override("font_color", Color(0.4, 0.8, 0.4))
	else:
		_gpu_label.text = "  GPU compute not available ✗"
		_gpu_label.add_theme_color_override("font_color", Color(0.8, 0.4, 0.4))
