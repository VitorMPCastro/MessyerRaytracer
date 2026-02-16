class_name LayerPanel
extends VBoxContainer
## Visibility layer toggle panel — controls 3 layer checkboxes.
##
## Set `debug_node` and/or `probe_node` before adding to scene.

var debug_node: RayTracerDebug
var probe_node: RayTracerProbe
var layer_enabled := [true, true, true]

var _menu: BaseMenu

@onready var _checks: Array[CheckBox] = [%Layer1, %Layer2, %Layer3]


func _ready() -> void:
	for i in range(3):
		var idx := i  # capture for lambda
		_checks[i].toggled.connect(func(on: bool): _on_toggled(idx, on))


## Sync checkbox state from layer_enabled array.
func sync_from_state() -> void:
	for i in range(3):
		_checks[i].set_pressed_no_signal(layer_enabled[i])


## Toggle a layer by index (0-based) — used from keyboard shortcut.
func toggle_layer(idx: int) -> void:
	if idx < 0 or idx >= 3:
		return
	layer_enabled[idx] = not layer_enabled[idx]
	_checks[idx].set_pressed_no_signal(layer_enabled[idx])
	_apply_mask()


func _on_toggled(idx: int, on: bool) -> void:
	layer_enabled[idx] = on
	_apply_mask()


func _apply_mask() -> void:
	var mask := 0
	for i in range(3):
		if layer_enabled[i]:
			mask |= (1 << i)
	if debug_node:
		debug_node.debug_layer_mask = mask
	if probe_node:
		probe_node.layer_mask = mask
	if _menu:
		_menu.fire_cast()
