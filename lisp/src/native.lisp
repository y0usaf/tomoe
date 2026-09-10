(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_abi_version" %abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_create" %create) (* t) (name sb-alien:c-string))
(sb-alien:define-alien-routine ("tomoe_destroy" %destroy) sb-alien:void (server (* t)))
(sb-alien:define-alien-routine ("tomoe_step" %step) sb-alien:int
  (server (* t)) (timeout sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_next_event" %event) sb-alien:c-string (server (* t)))
(sb-alien:define-alien-routine ("tomoe_place" %place) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (x sb-alien:int) (y sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (visible sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_focus" %focus) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(sb-alien:define-alien-routine ("tomoe_close" %close) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(sb-alien:define-alien-routine ("tomoe_keysym" %keysym) sb-alien:unsigned-int (name sb-alien:c-string))
(sb-alien:define-alien-routine ("tomoe_clear_bindings" %clear-bindings) sb-alien:void (server (* t)))
(sb-alien:define-alien-routine ("tomoe_bind" %bind) sb-alien:int
  (server (* t)) (modifiers sb-alien:unsigned-int) (keysym sb-alien:unsigned-int)
  (owner sb-alien:c-string) (command sb-alien:c-string))

(defun open-backend (socket)
  (let ((library (sb-ext:posix-getenv "TOMOE_LISP_BACKEND")))
    (unless library (error "TOMOE_LISP_BACKEND must name libtomoe-backend.so."))
    (sb-alien:load-shared-object library))
  (unless (= (%abi) 1) (error "Native ABI mismatch, expected 1."))
  (let ((server (%create socket)))
    (when (sb-alien:null-alien server) (error "Cannot start wlroots. See native error above."))
    server))
