class_name RendererPanel
extends VBoxContainer
## Renderer settings panel — controls for the RayRenderer node.
##
## Set `renderer_node` before adding to scene, then call `sync_from_node()`.

## The RayRenderer (GDExtension type, untyped to avoid LSP errors).
var renderer_node

## Parent BaseMenu for fire_cast.
var _menu: BaseMenu

## Whether the user wants continuous rendering.
var auto_render := true

var _res_presets: Array[Vector2i] = [
	Vector2i(160, 120), Vector2i(320, 240), Vector2i(640, 480),
	Vector2i(960, 720), Vector2i(1280, 960), Vector2i(1920, 1080),
]
var _channel_names: Array[String] = [
	"Color", "Normal", "Depth", "Barycentric", "Position", "PrimID", "HitMask",
	"Albedo", "Wireframe", "UV", "Fresnel"
]

@onready var _channel_btn: OptionButton = %ChannelBtn
@onready var _res_btn: OptionButton = %ResBtn
@onready var _pos_spin: SpinBox = %PosSpin
@onready var _shadows_check: CheckBox = %ShadowsCheck
@onready var _aa_check: CheckBox = %AACheck
@onready var _aa_spin: SpinBox = %AASpin
@onready var _auto_check: CheckBox = %AutoRender


func _ready() -> void:
	_channel_btn.item_selected.connect(_on_channel)
	_res_btn.item_selected.connect(_on_resolution)
	_pos_spin.value_changed.connect(func(v: float):
		if renderer_node: renderer_node.position_range = v; _fire())
	_shadows_check.toggled.connect(func(on: bool):
		if renderer_node: renderer_node.shadows_enabled = on; _fire())
	_aa_check.toggled.connect(func(on: bool):
		if renderer_node: renderer_node.aa_enabled = on; _fire())
	_aa_spin.value_changed.connect(func(v: float):
		if renderer_node: renderer_node.aa_max_samples = int(v); _fire())
	_auto_check.toggled.connect(func(on: bool): auto_render = on)

	sync_from_node()


## Read current values from renderer_node into the UI controls.
func sync_from_node() -> void:
	if not renderer_node:
		return
	_channel_btn.selected = renderer_node.render_channel
	_pos_spin.value = renderer_node.position_range

	# Find matching resolution preset.
	var cur: Vector2i = renderer_node.resolution
	for i in range(_res_presets.size()):
		if _res_presets[i] == cur:
			_res_btn.selected = i
			break

	_shadows_check.button_pressed = renderer_node.shadows_enabled
	_aa_check.button_pressed = renderer_node.aa_enabled
	_aa_spin.value = renderer_node.aa_max_samples
	_auto_check.button_pressed = auto_render


## Set render channel by index (called from keyboard shortcut).
func set_channel(idx: int) -> void:
	if renderer_node and idx >= 0 and idx < _channel_names.size():
		renderer_node.render_channel = idx
		_channel_btn.selected = idx
		_fire()


## Set resolution by preset index (called from keyboard shortcut).
func set_resolution_index(idx: int) -> void:
	if renderer_node and idx >= 0 and idx < _res_presets.size():
		renderer_node.resolution = _res_presets[idx]
		_res_btn.selected = idx
		_fire()


func current_res_index() -> int:
	return _res_btn.selected


func _on_channel(idx: int) -> void:
	if renderer_node:
		renderer_node.render_channel = idx
	_fire()


func _on_resolution(idx: int) -> void:
	if renderer_node and idx >= 0 and idx < _res_presets.size():
		renderer_node.resolution = _res_presets[idx]
	_fire()


func _fire() -> void:
	if _menu:
		_menu.fire_cast()


## Return last render timing (ms) for stats display.
func get_timing_ms() -> float:
	if renderer_node:
		return renderer_node.get_render_ms()
	return 0.0
