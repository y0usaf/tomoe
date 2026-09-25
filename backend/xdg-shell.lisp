(in-package #:tomoe)

(defparameter +xdg-shell-protocol-tables+
  '(
   ("xdg_wm_base" 7
    (
     (0 "destroy" "" ())
     (1 "create_positioner" "n" ("xdg_positioner"))
     (2 "get_xdg_surface" "no" ("xdg_surface" "wl_surface"))
     (3 "pong" "u" (""))
    )
    (
     (0 "ping" "u" (""))
    )
    )
   ("xdg_positioner" 7
    (
     (0 "destroy" "" ())
     (1 "set_size" "ii" ("" ""))
     (2 "set_anchor_rect" "iiii" ("" "" "" ""))
     (3 "set_anchor" "u" (""))
     (4 "set_gravity" "u" (""))
     (5 "set_constraint_adjustment" "u" (""))
     (6 "set_offset" "ii" ("" ""))
     (7 "set_reactive" "" ())
     (8 "set_parent_size" "ii" ("" ""))
     (9 "set_parent_configure" "u" (""))
    )
    (
    )
    )
   ("xdg_surface" 7
    (
     (0 "destroy" "" ())
     (1 "get_toplevel" "n" ("xdg_toplevel"))
     (2 "get_popup" "noo" ("xdg_popup" "xdg_surface" "xdg_positioner"))
     (3 "set_window_geometry" "iiii" ("" "" "" ""))
     (4 "ack_configure" "u" (""))
    )
    (
     (0 "configure" "u" (""))
    )
    )
   ("xdg_toplevel" 7
    (
     (0 "destroy" "" ())
     (1 "set_parent" "o" ("xdg_toplevel"))
     (2 "set_title" "s" (""))
     (3 "set_app_id" "s" (""))
     (4 "show_window_menu" "ouii" ("wl_seat" "" "" ""))
     (5 "move" "ou" ("wl_seat" ""))
     (6 "resize" "ouu" ("wl_seat" "" ""))
     (7 "set_max_size" "ii" ("" ""))
     (8 "set_min_size" "ii" ("" ""))
     (9 "set_maximized" "" ())
     (10 "unset_maximized" "" ())
     (11 "set_fullscreen" "o" ("wl_output"))
     (12 "unset_fullscreen" "" ())
     (13 "set_minimized" "" ())
    )
    (
     (0 "configure" "iia" ("" "" ""))
     (1 "close" "" ())
     (2 "configure_bounds" "ii" ("" ""))
     (3 "wm_capabilities" "a" (""))
    )
    )
   ("xdg_popup" 7
    (
     (0 "destroy" "" ())
     (1 "grab" "ou" ("wl_seat" ""))
     (2 "reposition" "ou" ("xdg_positioner" ""))
    )
    (
     (0 "configure" "iiii" ("" "" "" ""))
     (1 "popup_done" "" ())
     (2 "repositioned" "u" (""))
    )
    )
    ))
