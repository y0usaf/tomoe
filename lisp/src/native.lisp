(in-package #:tomoe)

(defmacro define-native ((foreign-name name) result &rest arguments)
  (let ((entry (gensym "NATIVE-")) (names (mapcar #'first arguments)))
    `(progn
       (sb-alien:define-alien-routine (,foreign-name ,entry) ,result ,@arguments)
       (defun ,name ,names
         ;; wlroots uses C's non-trapping IEEE arithmetic, including viewport
         ;; damage ratios with a zero-sized buffer on unmap. Do not leak SBCL's
         ;; enabled float traps into C; restore them before returning to Lisp.
         (sb-int:with-float-traps-masked (:invalid :divide-by-zero :overflow)
           (,entry ,@names))))))

(define-native ("tomoe_abi_version" %abi) sb-alien:int)
(define-native ("tomoe_create" %create) (* t) (name sb-alien:c-string))
(define-native ("tomoe_destroy" %destroy) sb-alien:void (server (* t)))
(define-native ("tomoe_step" %step) sb-alien:int
  (server (* t)) (timeout sb-alien:int))
(define-native ("tomoe_next_event" %event) sb-alien:c-string (server (* t)))
(define-native ("tomoe_place" %place) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (x sb-alien:int) (y sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (visible sb-alien:int))
(define-native ("tomoe_focus" %focus) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(define-native ("tomoe_window_state" %window-state) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (fullscreen sb-alien:int) (maximize sb-alien:int))
(define-native ("tomoe_layer" %layer) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (layer sb-alien:int) (exclusive-zone sb-alien:int)
  (keyboard sb-alien:int) (visible sb-alien:int))
(define-native ("tomoe_grab" %grab) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (mode sb-alien:int))
(define-native ("tomoe_close" %close) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(define-native ("tomoe_keysym" %keysym) sb-alien:unsigned-int (name sb-alien:c-string))
(define-native ("tomoe_clear_bindings" %clear-bindings) sb-alien:void (server (* t)))
(define-native ("tomoe_bind" %bind) sb-alien:int
  (server (* t)) (modifiers sb-alien:unsigned-int) (keysym sb-alien:unsigned-int)
  (owner sb-alien:c-string) (command sb-alien:c-string))

(define-native ("tomoe_outputs_begin" %outputs-begin) sb-alien:int (server (* t)))
(define-native ("tomoe_output" %output) sb-alien:int
  (server (* t)) (name sb-alien:c-string) (mode sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (refresh sb-alien:int) (scale sb-alien:int)
  (x sb-alien:int) (y sb-alien:int) (positioned sb-alien:int))
(define-native ("tomoe_outputs_apply" %outputs-apply) sb-alien:c-string (server (* t)))

(defun configure-native-outputs (backend outputs)
  (unless (= (%outputs-begin backend) 1) (error "Cannot allocate output configuration."))
  (dolist (output outputs)
    (unless (= 1 (%output backend (getf output :name)
                          (ecase (getf output :mode) (:preferred 0) (:max 1) (:exact 2))
                          (getf output :width) (getf output :height) (getf output :refresh-mhz)
                          (getf output :scale-120) (getf output :x) (getf output :y)
                          (if (getf output :positioned) 1 0)))
      (error "Unsupported output mode for ~A; see :MODES in inspect." (getf output :name))))
  (let ((message (%outputs-apply backend)))
    (when message (error "~A" message))))

(defun open-backend (socket)
  (let ((library (sb-ext:posix-getenv "TOMOE_LISP_BACKEND")))
    (unless library (error "TOMOE_LISP_BACKEND must name libtomoe-backend.so."))
    (sb-alien:load-shared-object library))
  (unless (= (%abi) 3) (error "Native ABI mismatch, expected 3."))
  (let ((server (%create socket)))
    (when (sb-alien:null-alien server) (error "Cannot start wlroots. See native error above."))
    server))
