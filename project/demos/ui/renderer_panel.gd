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
@onready var _depth_spin: SpinBox = %DepthSpin
@onready var _pos_spin: SpinBox = %PosSpin
@onready var _sun_x: SpinBox = %SunX
@onready var _sun_y: SpinBox = %SunY
@onready var _sun_z: SpinBox = %SunZ
@onready var _auto_check: CheckBox = %AutoRender


func _ready() -> void:
	_channel_btn.item_selected.connect(_on_channel)
	_res_btn.item_selected.connect(_on_resolution)
	_depth_spin.value_changed.connect(func(v: float):
		if renderer_node: renderer_node.depth_range = v; _fire())
	_pos_spin.value_changed.connect(func(v: float):
		if renderer_node: renderer_node.position_range = v; _fire())
	_sun_x.value_changed.connect(func(_v: float): _on_sun_changed())
	_sun_y.value_changed.connect(func(_v: float): _on_sun_changed())
	_sun_z.value_changed.connect(func(_v: float): _on_sun_changed())
	_auto_check.toggled.connect(func(on: bool): auto_render = on)

	sync_from_node()


## Read current values from renderer_node into the UI controls.
func sync_from_node() -> void:
	if not renderer_node:
		return
	_channel_btn.selected = renderer_node.render_channel
	_depth_spin.value = renderer_node.depth_range
	_pos_spin.value = renderer_node.position_range

	var sun: Vector3 = renderer_node.sun_direction
	_sun_x.set_value_no_signal(sun.x)
	_sun_y.set_value_no_signal(sun.y)
	_sun_z.set_value_no_signal(sun.z)

	# Find matching resolution preset.
	var cur: Vector2i = renderer_node.resolution
	for i in range(_res_presets.size()):
		if _res_presets[i] == cur:
			_res_btn.selected = i
			break

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


func _on_sun_changed() -> void:
	if not renderer_node:
		return
	var d := Vector3(_sun_x.value, _sun_y.value, _sun_z.value)
	if d.length_squared() > 0.001:
		renderer_node.sun_direction = d.normalized()
	_fire()


func _fire() -> void:
	if _menu:
		_menu.fire_cast()


## Return last render timing (ms) for stats display.
func get_timing_ms() -> float:
	if renderer_node:
		return renderer_node.get_render_ms()
	return 0.0
