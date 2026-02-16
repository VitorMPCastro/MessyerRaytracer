class_name BaseMenu
extends CanvasLayer
## Modular base menu — hosts the chrome (dim, panel, backend, stats, buttons).
##
## Panels (debug_panel, renderer_panel, layer_panel) are added into %PanelSlot.
##
## USAGE:
##   var menu = preload("res://demos/ui/base_menu.tscn").instantiate()
##   menu.add_panel(preload("res://demos/ui/renderer_panel.tscn").instantiate())
##   menu.cast_callback = my_callback
##   add_child(menu)

## Called when "Action" button is pressed or any control changes.
var cast_callback: Callable

# ── Node refs (scene-unique names) ──────────────────────────────────────────
@onready var _dim: ColorRect = $Dim
@onready var _scroll: ScrollContainer = $Scroll
@onready var _backend_btn: OptionButton = %BackendBtn
@onready var _gpu_label: Label = %GpuLabel
@onready var _panel_slot: VBoxContainer = %PanelSlot
@onready var _stats_label: Label = %StatsLabel
@onready var _action_btn: Button = %ActionBtn
@onready var _resume_btn: Button = %ResumeBtn
@onready var _quit_btn: Button = %QuitBtn

var _visible := false
var _last_timing_ms := 0.0
var _action_label_override := ""
var _backend_names: Array[String] = ["CPU", "GPU", "Auto"]

# Panels track themselves here so stats can query them.
var _panels: Array[Control] = []


func _ready() -> void:
	_backend_btn.selected = RayTracerServer.get_backend()
	_backend_btn.item_selected.connect(_on_backend_changed)
	_action_btn.pressed.connect(func(): fire_cast())
	_resume_btn.pressed.connect(func(): toggle_menu())
	_quit_btn.pressed.connect(func(): get_tree().quit())
	if _action_label_override != "":
		_action_btn.text = "  %s  " % _action_label_override
	_refresh_gpu_label()


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_ESCAPE or event.keycode == KEY_P:
			toggle_menu()
			get_viewport().set_input_as_handled()


## Add a sub-panel scene into the panel slot.
func add_panel(panel: Control) -> void:
	# Wire the back-reference so the panel can call fire_cast.
	panel.set("_menu", self)
	# Insert a separator before each panel.
	var sep := HSeparator.new()
	%PanelSlot.add_child(sep)
	%PanelSlot.add_child(panel)
	_panels.append(panel)


## Toggle menu visibility.
func toggle_menu() -> void:
	_visible = not _visible
	_dim.visible = _visible
	_scroll.visible = _visible
	if _visible:
		Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
		_refresh_stats()
	else:
		Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)


func is_open() -> bool:
	return _visible


## Fire the cast callback and refresh stats.
func fire_cast() -> void:
	if cast_callback.is_valid():
		cast_callback.call()
	# Collect timing from whichever panel provides it.
	_last_timing_ms = 0.0
	for p in _panels:
		if p.has_method("get_timing_ms"):
			var t: float = p.get_timing_ms()
			if t > 0.0:
				_last_timing_ms = t
	_refresh_stats()


## Set the action button label (e.g. "Cast Rays" or "Render").
## Safe to call before the menu enters the tree.
func set_action_label(text: String) -> void:
	_action_label_override = text
	if _action_btn:
		_action_btn.text = "  %s  " % text


# ── Callbacks ────────────────────────────────────────────────────────────────

func _on_backend_changed(idx: int) -> void:
	RayTracerServer.set_backend(idx)
	RayTracerServer.build()
	_refresh_gpu_label()
	fire_cast()
	print("[Menu] Backend → ", _backend_names[idx])


func _refresh_stats() -> void:
	if not _stats_label:
		return
	var tri := RayTracerServer.get_triangle_count()
	var meshes := RayTracerServer.get_mesh_count()
	var bvh_depth := RayTracerServer.get_bvh_depth()
	var threads := RayTracerServer.get_thread_count()
	var gpu_str := "Yes" if RayTracerServer.is_gpu_available() else "No"
	var timing_str := ""
	if _last_timing_ms > 0.0:
		timing_str = " | %.2f ms" % _last_timing_ms
	_stats_label.text = "%d tris | %d meshes | BVH depth %d | %d threads | GPU: %s%s" % [
		tri, meshes, bvh_depth, threads, gpu_str, timing_str]


func _refresh_gpu_label() -> void:
	if not _gpu_label:
		return
	if RayTracerServer.is_gpu_available():
		_gpu_label.text = "  GPU compute available ✓"
		_gpu_label.add_theme_color_override("font_color", Color(0.4, 0.8, 0.4))
	else:
		_gpu_label.text = "  GPU compute not available ✗"
		_gpu_label.add_theme_color_override("font_color", Color(0.8, 0.4, 0.4))
