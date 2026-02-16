class_name TooltipOverlay
extends CanvasLayer
## F1-togglable keyboard shortcut overlay — anchored to a screen corner.
##
## Set `hint_text` before (or after) adding to the scene tree. Press F1
## to toggle the help panel on/off.  Starts hidden.

@onready var _panel: PanelContainer = $Panel
@onready var _label: Label = %HintLabel

## The text shown in the overlay.  Set this from your demo script.
var hint_text := "F1 — Toggle this help"


func _ready() -> void:
	_panel.visible = false
	_label.text = hint_text


func _input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo:
		if event.keycode == KEY_F1:
			_panel.visible = not _panel.visible
			get_viewport().set_input_as_handled()


## Programmatic show/hide.
func show_hints() -> void:
	_panel.visible = true


func hide_hints() -> void:
	_panel.visible = false


## Update the text at runtime.
func set_text(text: String) -> void:
	hint_text = text
	if _label:
		_label.text = text
