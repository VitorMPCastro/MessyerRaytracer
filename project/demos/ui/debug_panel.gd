class_name DebugPanel
extends VBoxContainer
## Debug visualizer panel — controls for RayTracerDebug node.
##
## Set `debug_node` before adding to scene, then call `sync_from_node()`.

var debug_node: RayTracerDebug

## The parent BaseMenu calls this after a control changes.
var _menu: BaseMenu

## Grid dimensions — demos read these.
var grid_w: int = 16
var grid_h: int = 12

@onready var _mode_btn: OptionButton = %ModeBtn
@onready var _grid_w: SpinBox = %GridW
@onready var _grid_h: SpinBox = %GridH
@onready var _miss_spin: SpinBox = %MissSpin
@onready var _norm_spin: SpinBox = %NormSpin
@onready var _heat_dist: SpinBox = %HeatDistSpin
@onready var _heat_cost: SpinBox = %HeatCostSpin
@onready var _bvh_spin: SpinBox = %BvhSpin
@onready var _clear_btn: Button = %ClearBtn


func _ready() -> void:
	_mode_btn.item_selected.connect(_on_mode)
	_grid_w.value_changed.connect(func(v: float): grid_w = int(v); _fire())
	_grid_h.value_changed.connect(func(v: float): grid_h = int(v); _fire())
	_miss_spin.value_changed.connect(func(v: float):
		if debug_node: debug_node.debug_ray_miss_length = v)
	_norm_spin.value_changed.connect(func(v: float):
		if debug_node: debug_node.debug_normal_length = v)
	_heat_dist.value_changed.connect(func(v: float):
		if debug_node: debug_node.debug_heatmap_max_distance = v)
	_heat_cost.value_changed.connect(func(v: float):
		if debug_node: debug_node.debug_heatmap_max_cost = int(v))
	_bvh_spin.value_changed.connect(func(v: float):
		if debug_node: debug_node.debug_bvh_depth = int(v))
	_clear_btn.pressed.connect(func():
		if debug_node: debug_node.clear_debug())

	# Sync initial values from the debug node.
	sync_from_node()


## Read current values from the debug node into the UI controls.
func sync_from_node() -> void:
	if not debug_node:
		return
	_mode_btn.selected = debug_node.debug_draw_mode
	_miss_spin.value = debug_node.debug_ray_miss_length
	_norm_spin.value = debug_node.debug_normal_length
	_heat_dist.value = debug_node.debug_heatmap_max_distance
	_heat_cost.value = debug_node.debug_heatmap_max_cost
	_bvh_spin.value = debug_node.debug_bvh_depth
	_grid_w.value = grid_w
	_grid_h.value = grid_h


func _on_mode(idx: int) -> void:
	if debug_node:
		debug_node.debug_draw_mode = idx
	_fire()


func _fire() -> void:
	if _menu:
		_menu.fire_cast()


## Return last cast timing (ms) for stats display.
func get_timing_ms() -> float:
	if debug_node:
		return debug_node.get_last_cast_ms()
	return 0.0
