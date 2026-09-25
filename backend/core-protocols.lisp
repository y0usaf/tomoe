(in-package #:tomoe)

(defparameter +core-protocol-tables+
  '(
   ("wl_callback" 1
    (
    )
    (
     (0 "done" "u" (""))
    )
    )
   ("wl_compositor" 7
    (
     (0 "create_surface" "n" ("wl_surface"))
     (1 "create_region" "n" ("wl_region"))
     (2 "release" "" ())
    )
    (
    )
    )
   ("wl_shm_pool" 3
    (
     (0 "create_buffer" "niiiiu" ("wl_buffer" "" "" "" "" ""))
     (1 "destroy" "" ())
     (2 "resize" "i" (""))
    )
    (
    )
    )
   ("wl_shm" 3
    (
     (0 "create_pool" "nhi" ("wl_shm_pool" "" ""))
     (1 "release" "" ())
    )
    (
     (0 "format" "u" (""))
    )
    )
   ("wl_buffer" 1
    (
     (0 "destroy" "" ())
    )
    (
     (0 "release" "" ())
    )
    )
   ("wl_data_offer" 4
    (
     (0 "accept" "u?s" ("" ""))
     (1 "receive" "sh" ("" ""))
     (2 "destroy" "" ())
     (3 "finish" "" ())
     (4 "set_actions" "uu" ("" ""))
    )
    (
     (0 "offer" "s" (""))
     (1 "source_actions" "u" (""))
     (2 "action" "u" (""))
    )
    )
   ("wl_data_source" 4
    (
     (0 "offer" "s" (""))
     (1 "destroy" "" ())
     (2 "set_actions" "u" (""))
    )
    (
     (0 "target" "?s" (""))
     (1 "send" "sh" ("" ""))
     (2 "cancelled" "" ())
     (3 "dnd_drop_performed" "" ())
     (4 "dnd_finished" "" ())
     (5 "action" "u" (""))
    )
    )
   ("wl_data_device" 4
    (
     (0 "start_drag" "?oo?ou" ("wl_data_source" "wl_surface" "wl_surface" ""))
     (1 "set_selection" "?ou" ("wl_data_source" ""))
     (2 "release" "" ())
    )
    (
     (0 "data_offer" "n" ("wl_data_offer"))
     (1 "enter" "uoff?o" ("" "wl_surface" "" "" "wl_data_offer"))
     (2 "leave" "" ())
     (3 "motion" "uff" ("" "" ""))
     (4 "drop" "" ())
     (5 "selection" "?o" ("wl_data_offer"))
    )
    )
   ("wl_data_device_manager" 4
    (
     (0 "create_data_source" "n" ("wl_data_source"))
     (1 "get_data_device" "no" ("wl_data_device" "wl_seat"))
     (2 "release" "" ())
    )
    (
    )
    )
   ("wl_surface" 7
    (
     (0 "destroy" "" ())
     (1 "attach" "?oii" ("wl_buffer" "" ""))
     (2 "damage" "iiii" ("" "" "" ""))
     (3 "frame" "n" ("wl_callback"))
     (4 "set_opaque_region" "?o" ("wl_region"))
     (5 "set_input_region" "?o" ("wl_region"))
     (6 "commit" "" ())
     (7 "set_buffer_transform" "i" (""))
     (8 "set_buffer_scale" "i" (""))
     (9 "damage_buffer" "iiii" ("" "" "" ""))
     (10 "offset" "ii" ("" ""))
     (11 "get_release" "n" ("wl_callback"))
    )
    (
     (0 "enter" "o" ("wl_output"))
     (1 "leave" "o" ("wl_output"))
     (2 "preferred_buffer_scale" "i" (""))
     (3 "preferred_buffer_transform" "u" (""))
    )
    )
   ("wl_seat" 11
    (
     (0 "get_pointer" "n" ("wl_pointer"))
     (1 "get_keyboard" "n" ("wl_keyboard"))
     (2 "get_touch" "n" ("wl_touch"))
     (3 "release" "" ())
    )
    (
     (0 "capabilities" "u" (""))
     (1 "name" "s" (""))
    )
    )
   ("wl_pointer" 11
    (
     (0 "set_cursor" "u?oii" ("" "wl_surface" "" ""))
     (1 "release" "" ())
    )
    (
     (0 "enter" "uoff" ("" "wl_surface" "" ""))
     (1 "leave" "uo" ("" "wl_surface"))
     (2 "motion" "uff" ("" "" ""))
     (3 "button" "uuuu" ("" "" "" ""))
     (4 "axis" "uuf" ("" "" ""))
     (5 "frame" "" ())
     (6 "axis_source" "u" (""))
     (7 "axis_stop" "uu" ("" ""))
     (8 "axis_discrete" "ui" ("" ""))
     (9 "axis_value120" "ui" ("" ""))
     (10 "axis_relative_direction" "uu" ("" ""))
     (11 "warp" "ff" ("" ""))
    )
    )
   ("wl_keyboard" 11
    (
     (0 "release" "" ())
    )
    (
     (0 "keymap" "uhu" ("" "" ""))
     (1 "enter" "uoa" ("" "wl_surface" ""))
     (2 "leave" "uo" ("" "wl_surface"))
     (3 "key" "uuuu" ("" "" "" ""))
     (4 "modifiers" "uuuuu" ("" "" "" "" ""))
     (5 "repeat_info" "ii" ("" ""))
    )
    )
   ("wl_touch" 11
    (
     (0 "release" "" ())
    )
    (
     (0 "down" "uuoiff" ("" "" "wl_surface" "" "" ""))
     (1 "up" "uui" ("" "" ""))
     (2 "motion" "uiff" ("" "" "" ""))
     (3 "frame" "" ())
     (4 "cancel" "" ())
     (5 "shape" "iff" ("" "" ""))
     (6 "orientation" "if" ("" ""))
    )
    )
   ("wl_output" 4
    (
     (0 "release" "" ())
    )
    (
     (0 "geometry" "iiiiissi" ("" "" "" "" "" "" "" ""))
     (1 "mode" "uiii" ("" "" "" ""))
     (2 "done" "" ())
     (3 "scale" "i" (""))
     (4 "name" "s" (""))
     (5 "description" "s" (""))
    )
    )
   ("wl_region" 7
    (
     (0 "destroy" "" ())
     (1 "add" "iiii" ("" "" "" ""))
     (2 "subtract" "iiii" ("" "" "" ""))
    )
    (
    )
    )
   ("wl_subcompositor" 1
    (
     (0 "destroy" "" ())
     (1 "get_subsurface" "noo" ("wl_subsurface" "wl_surface" "wl_surface"))
    )
    (
    )
    )
   ("wl_subsurface" 1
    (
     (0 "destroy" "" ())
     (1 "set_position" "ii" ("" ""))
     (2 "place_above" "o" ("wl_surface"))
     (3 "place_below" "o" ("wl_surface"))
     (4 "set_sync" "" ())
     (5 "set_desync" "" ())
    )
    (
    )
    )
    ))
