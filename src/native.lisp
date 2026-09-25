(in-package #:tomoe)

(defmacro define-native ((foreign-name name) result &rest arguments)
  (let ((entry (gensym "NATIVE-")) (names (mapcar #'first arguments)))
    `(progn
       (sb-alien:define-alien-routine (,foreign-name ,entry) ,result ,@arguments)
       (defun ,name ,names
         (sb-int:with-float-traps-masked (:invalid :divide-by-zero :overflow)
           (,entry ,@names))))))

(define-native ("tomoe_abi_version" %abi) sb-alien:int)
(define-native ("tomoe_display_name" %display-name) sb-alien:c-string (server (* t)))
(define-native ("tomoe_activation_token" %activation-token) sb-alien:c-string (server (* t)))
(define-native ("tomoe_activation_revoke" %activation-revoke) sb-alien:void
  (server (* t)) (token sb-alien:c-string))
(define-native ("tomoe_create" %create) (* t) (name sb-alien:c-string))
(define-native ("tomoe_destroy" %destroy) sb-alien:void (server (* t)))
(define-native ("tomoe_step" %step) sb-alien:int
  (server (* t)) (timeout sb-alien:int))
(define-native ("tomoe_next_event" %event) sb-alien:c-string (server (* t)))
(define-native ("tomoe_event_count" %event-count) sb-alien:int (server (* t)))
(define-native ("tomoe_event_barrier" %event-barrier) sb-alien:int (server (* t)))
(define-native ("tomoe_place" %place) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (x sb-alien:int) (y sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (visible sb-alien:int))
(define-native ("tomoe_set_view" %set-view) sb-alien:void
  (server (* t)) (x sb-alien:int) (y sb-alien:int) (zoom sb-alien:double))
(define-native ("tomoe_hit_test" %hit-test) sb-alien:c-string
  (server (* t)) (screen-x sb-alien:double) (screen-y sb-alien:double))
(define-native ("tomoe_focus" %focus) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(define-native ("tomoe_keyboard_focus" %keyboard-focus) sb-alien:unsigned-int
  (server (* t)))
(define-native ("tomoe_window_state" %window-state) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (fullscreen sb-alien:int) (maximize sb-alien:int))
(define-native ("tomoe_layer" %layer) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int) (layer sb-alien:int) (exclusive-zone sb-alien:int)
  (keyboard sb-alien:int) (visible sb-alien:int))
(define-native ("tomoe_grab" %grab) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-int) (mode sb-alien:int))
(define-native ("tomoe_grab_id" %grab-id) sb-alien:unsigned-int (server (* t)))
(define-native ("tomoe_grab_mode" %grab-mode) sb-alien:int (server (* t)))
(define-native ("tomoe_close" %close) sb-alien:void
  (server (* t)) (id sb-alien:unsigned-int))
(define-native ("tomoe_screenshot" %screenshot) sb-alien:void
  (server (* t)) (interactive sb-alien:int))
(define-native ("tomoe_keysym" %keysym) sb-alien:unsigned-int (name sb-alien:c-string))
(define-native ("tomoe_clear_bindings" %clear-bindings) sb-alien:void (server (* t)))
(define-native ("tomoe_bind" %bind) sb-alien:int
  (server (* t)) (modifiers sb-alien:unsigned-int) (keysym sb-alien:unsigned-int)
  (owner sb-alien:c-string) (command sb-alien:c-string) (release sb-alien:c-string)
  (source-id sb-alien:unsigned-long-long))
(define-native ("tomoe_binding_current" %binding-current) sb-alien:int
  (server (* t)) (binding-id sb-alien:unsigned-long-long))

(define-native ("tomoe_ui_text_size" %ui-text-size-raw) sb-alien:unsigned-long-long
  (text sb-alien:c-string) (font sb-alien:c-string) (size sb-alien:double) (line-height sb-alien:double))
(defun %ui-text-size (text font size line-height)
  (%ui-text-size-raw text font (%double-float size) (%double-float line-height)))
(define-native ("tomoe_present_ui_surface" %present-ui-surface) sb-alien:int
  (server (* t)) (owner sb-alien:c-string) (source-id sb-alien:unsigned-long-long)
  (name sb-alien:c-string) (output sb-alien:c-string) (signature sb-alien:c-string)
  (x sb-alien:int) (y sb-alien:int) (width sb-alien:int) (height sb-alien:int) (layer sb-alien:int))
(define-native ("tomoe_present_ui_clip" %present-ui-clip) sb-alien:int
  (server (* t)) (x sb-alien:int) (y sb-alien:int) (width sb-alien:int) (height sb-alien:int))
(define-native ("tomoe_present_ui_rect" %present-ui-rect) sb-alien:int
  (server (* t)) (x sb-alien:double) (y sb-alien:double) (width sb-alien:double) (height sb-alien:double)
  (rgba sb-alien:unsigned-int) (radius sb-alien:double) (stroke sb-alien:double))
(define-native ("tomoe_present_ui_text" %present-ui-text) sb-alien:int
  (server (* t)) (x sb-alien:double) (y sb-alien:double) (text sb-alien:c-string) (font sb-alien:c-string)
  (size sb-alien:double) (line-height sb-alien:double) (rgba sb-alien:unsigned-int))
(define-native ("tomoe_present_ui_arc" %present-ui-arc) sb-alien:int
  (server (* t)) (cx sb-alien:double) (cy sb-alien:double) (radius sb-alien:double)
  (thickness sb-alien:double) (start sb-alien:double) (end sb-alien:double) (rgba sb-alien:unsigned-int))
(define-native ("tomoe_present_ui_hit" %present-ui-hit) sb-alien:int
  (server (* t)) (key sb-alien:c-string) (command sb-alien:c-string) (hover sb-alien:c-string)
  (x sb-alien:int) (y sb-alien:int) (width sb-alien:int) (height sb-alien:int))
(define-native ("tomoe_present_ui_end" %present-ui-end) sb-alien:int (server (* t)))
(define-native ("tomoe_ui_callback_current" %ui-callback-current) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-long-long))
(define-native ("tomoe_ui_stats" %ui-stats) sb-alien:c-string (server (* t)))
(define-native ("tomoe_ui_asset_load" %ui-asset-load) sb-alien:unsigned-long-long
  (server (* t)) (owner sb-alien:c-string) (source-id sb-alien:unsigned-long-long)
  (kind sb-alien:int) (path sb-alien:c-string) (name sb-alien:c-string))
(define-native ("tomoe_ui_asset_size" %ui-asset-size) sb-alien:unsigned-long-long
  (server (* t)) (id sb-alien:unsigned-long-long))
(define-native ("tomoe_ui_assets_discard" %ui-assets-discard) sb-alien:void (server (* t)))
(define-native ("tomoe_present_ui_asset_ref" %present-ui-asset-ref) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-long-long))
(define-native ("tomoe_present_ui_asset" %present-ui-asset) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-long-long)
  (x sb-alien:double) (y sb-alien:double) (width sb-alien:double) (height sb-alien:double)
  (tint sb-alien:int) (rgba sb-alien:unsigned-int))

(defun native-ui-asset-loader (backend)
  (when backend
    (lambda (declaration tree)
      (let* ((image-p (eq (getf tree :kind) :image))
             (raw (getf tree (if image-p :src :path)))
             (path (cond ((or (null raw) (zerop (length raw))) "")
                         ((char= (char raw 0) #\/) raw)
                         (t
                          (let ((source (getf declaration :source)))
                            (unless (and (stringp source) (plusp (length source))
                                         (char= (char source 0) #\/))
                              (error "Relative shell asset paths require a declaring source."))
                            (concatenate 'string (directory-namestring source) raw)))))
             (id (%ui-asset-load backend (getf declaration :owner) (getf declaration :source-id)
                                 (if image-p 1 2) path (if image-p "" (getf tree :name)))))
        (when (zerop id) (error "Cannot prepare shell asset ~S." (or raw (getf tree :name))))
        (let ((dimensions (%ui-asset-size backend id)))
          (when (= dimensions #xffffffffffffffff) (error "Shell asset lost during preparation."))
          (values id dimensions))))))

(define-native ("tomoe_outputs_begin" %outputs-begin) sb-alien:int (server (* t)))
(define-native ("tomoe_outputs_pending" %outputs-pending) sb-alien:int (server (* t)))
(define-native ("tomoe_output_hold" %output-hold) sb-alien:int
  (server (* t)) (name sb-alien:c-string))
(define-native ("tomoe_output" %output) sb-alien:int
  (server (* t)) (name sb-alien:c-string) (mode sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (refresh sb-alien:int) (scale sb-alien:int)
  (x sb-alien:int) (y sb-alien:int) (positioned sb-alien:int))
(define-native ("tomoe_output_options" %output-options) sb-alien:int
  (server (* t)) (name sb-alien:c-string) (enabled sb-alien:int)
  (mirror sb-alien:c-string) (adaptive-sync sb-alien:int))
(define-native ("tomoe_outputs_apply" %outputs-apply) sb-alien:c-string (server (* t)))
(define-native ("tomoe_outputs_preview" %outputs-preview) sb-alien:c-string (server (* t)))
(define-native ("tomoe_outputs_revision" %outputs-revision) sb-alien:unsigned-long-long
  (server (* t)))
(define-native ("tomoe_outputs_current" %outputs-current) sb-alien:c-string
  (server (* t)))
(define-native ("tomoe_layers_begin" %layers-begin) sb-alien:int (server (* t)))
(define-native ("tomoe_layers_output" %layers-output) sb-alien:int
  (server (* t)) (name sb-alien:c-string) (x sb-alien:int) (y sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (scale sb-alien:int))
(define-native ("tomoe_layers_surface" %layers-surface) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-int) (layer sb-alien:int) (zone sb-alien:int)
  (keyboard sb-alien:int) (visible sb-alien:int))
(define-native ("tomoe_layers_preview" %layers-preview) sb-alien:c-string (server (* t)))
(define-native ("tomoe_present_begin" %present-begin) sb-alien:int
  (server (* t)) (x sb-alien:int) (y sb-alien:int) (zoom sb-alien:double)
  (focus sb-alien:unsigned-int) (restack sb-alien:int) (outputs-changed sb-alien:int)
  (bindings-changed sb-alien:int) (grab-id sb-alien:unsigned-int) (grab-mode sb-alien:int))
(define-native ("tomoe_present_bind" %present-bind) sb-alien:int
  (server (* t)) (modifiers sb-alien:unsigned-int) (keysym sb-alien:unsigned-int)
  (owner sb-alien:c-string) (command sb-alien:c-string) (release sb-alien:c-string)
  (source-id sb-alien:unsigned-long-long))
(define-native ("tomoe_present_keyboard" %present-keyboard) sb-alien:int
  (server (* t)) (rules sb-alien:c-string) (model sb-alien:c-string)
  (layout sb-alien:c-string) (variant sb-alien:c-string) (options sb-alien:c-string)
  (repeat-rate sb-alien:int) (repeat-delay sb-alien:int))
(define-native ("tomoe_present_window" %present-window) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-int) (x sb-alien:int) (y sb-alien:int)
  (width sb-alien:int) (height sb-alien:int) (visible sb-alien:int)
  (fullscreen sb-alien:int) (maximize sb-alien:int))
(define-native ("tomoe_present_settings" %present-settings) sb-alien:int (server (* t)))
(define-native ("tomoe_present_setting" %present-setting) sb-alien:int
  (server (* t)) (key sb-alien:c-string) (value sb-alien:double))
(define-native ("tomoe_present_setting_text" %present-setting-text) sb-alien:int
  (server (* t)) (key sb-alien:c-string) (text sb-alien:c-string))
(define-native ("tomoe_present_window_style" %present-window-style) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-int) (radius sb-alien:int) (blur sb-alien:int)
  (tearing sb-alien:int) (focused sb-alien:long-long) (unfocused sb-alien:long-long))
(define-native ("tomoe_present_keyboard_grab" %present-keyboard-grab) sb-alien:int
  (server (* t)) (owner sb-alien:c-string) (source-id sb-alien:unsigned-long-long)
  (otherwise sb-alien:c-string))
(define-native ("tomoe_present_apply" %present-apply) sb-alien:c-string (server (* t)))
(define-native ("tomoe_present_stack" %present-stack) sb-alien:int
  (server (* t)) (id sb-alien:unsigned-int))
(define-native ("tomoe_present_abort" %present-abort) sb-alien:void (server (* t)))

(defun full-workareas (outputs)
  (loop for output in outputs collect
    (list :name (getf output :name) :x (getf output :x) :y (getf output :y)
          :width (getf output :width) :height (getf output :height))))

(defun preview-native-layers (backend outputs layers overrides)
  "Resolve layer placement and usable output areas without sending configures."
  (unless (= 1 (%layers-begin backend)) (error "Cannot allocate layer preview."))
  (dolist (output outputs)
    (unless (= 1 (%layers-output backend (getf output :name)
                                (getf output :x) (getf output :y)
                                (getf output :width) (getf output :height)
                                (getf output :scale-120 120)))
      (error "Cannot stage layer output ~A." (getf output :name))))
  (dolist (layer layers)
    (let ((override (assoc (getf layer :id) overrides)))
      (unless (= 1 (%layers-surface
                    backend (getf layer :id)
                    (ecase (second override)
                      (:background 0) (:bottom 1) (:top 2) (:overlay 3) ((nil) -1))
                    (or (third override) -1)
                    (ecase (fourth override)
                      (:none 0) (:exclusive 1) (:on-demand 2) ((nil) -1))
                    (if override (if (fifth override) 1 0) -1)))
        (error "Cannot stage layer ~D." (getf layer :id)))))
  (let ((text (%layers-preview backend)))
    (unless text (error "Cannot serialize layer preview."))
    (let ((preview (read-data text)))
      (values (getf preview :layers) (getf preview :workareas)))))

(defun stage-native-outputs (backend outputs)
  (unless (= (%outputs-begin backend) 1) (error "Cannot allocate output configuration."))
  (dolist (output outputs)
    (if (getf output :hold)
        (unless (= 1 (%output-hold backend (getf output :name)))
          (error "Cannot retain current output state for ~A." (getf output :name)))
        (progn
          (unless (= 1 (%output backend (getf output :name)
                               (ecase (getf output :mode) (:preferred 0) (:max 1) (:exact 2))
                               (getf output :width) (getf output :height) (getf output :refresh-mhz)
                               (getf output :scale-120) (getf output :x) (getf output :y)
                               (if (getf output :positioned) 1 0)))
            (error 'output-configuration-error :output (getf output :name)
                   :format-control "Cannot stage output mode for ~A."
                   :format-arguments (list (getf output :name))))
          (unless (= 1 (%output-options backend (getf output :name)
                                       (if (getf output :disabled) 0 1)
                                       (or (getf output :mirror) "") (if (getf output :vrr) 1 0)))
            (error "Cannot stage output options for ~A." (getf output :name)))))))

(defun pending-native-outputs-p (backend)
  (and backend (= 1 (%outputs-pending backend))))

(defun preview-native-outputs (backend outputs)
  "Resolve candidate active outputs and connected ports without native writes."
  (stage-native-outputs backend outputs)
  (let ((text (%outputs-preview backend)))
    (unless text (error "Cannot allocate output configuration preview."))
    (read-data text)))

(defun current-native-outputs (backend)
  "Read currently committed output facts without changing native state."
  (let ((text (%outputs-current backend)))
    (unless text (error "Cannot serialize current native outputs."))
    (let ((snapshot (read-data text)))
      (unless (and (listp snapshot)
                   (integerp (getf snapshot :revision))
                   (<= 0 (getf snapshot :revision))
                   (listp (getf snapshot :outputs)))
        (error "Native output snapshot is invalid: ~S" snapshot))
      snapshot)))

(defun configure-native-outputs (backend outputs)
  (stage-native-outputs backend outputs)
  (let ((message (%outputs-apply backend)))
    (when message (error "~A" message))))

(defun stage-native-ui (backend plans)
  (flet ((require-ui (status)
           (unless (= 1 status) (error "Cannot prepare retained shell rendering."))))
    (dolist (plan plans)
      (let* ((signature (with-standard-io-syntax (write-to-string plan :readably t :pretty nil)))
             (status (%present-ui-surface backend (getf plan :owner) (getf plan :source-id)
                                          (string-downcase (getf plan :name)) (getf plan :output) signature
                                          (getf plan :x) (getf plan :y) (getf plan :width) (getf plan :height)
                                          (ecase (getf plan :layer) (:background 0) (:bottom 1) (:top 2) (:overlay 3)))))
        (unless (member status '(1 2)) (error "Cannot prepare shell surface ~S." (getf plan :name)))
        (when (= status 1)
          (dolist (id (getf plan :assets))
            (require-ui (%present-ui-asset-ref backend id)))
          (dolist (operation (getf plan :draw))
            (require-ui (apply #'%present-ui-clip backend (car (last operation))))
            (require-ui
             (ecase (first operation)
               (:asset
                (destructuring-bind (kind id x y width height tint rgba clip) operation
                  (declare (ignore kind clip))
                  (%present-ui-asset backend id (%double-float x) (%double-float y)
                                     (%double-float width) (%double-float height)
                                     (if tint 1 0) rgba)))
               (:rect
                (destructuring-bind (kind x y width height rgba radius stroke clip) operation
                  (declare (ignore kind clip))
                  (%present-ui-rect backend (%double-float x) (%double-float y)
                                    (%double-float width) (%double-float height) rgba
                                    (%double-float radius) (%double-float stroke))))
               (:text
                (destructuring-bind (kind x y text font size line-height rgba clip) operation
                  (declare (ignore kind clip))
                  (%present-ui-text backend (%double-float x) (%double-float y) text font
                                    (%double-float size) (%double-float line-height) rgba)))
               (:arc
                (destructuring-bind (kind cx cy radius thickness start end rgba clip) operation
                  (declare (ignore kind clip))
                  (%present-ui-arc backend (%double-float cx) (%double-float cy)
                                   (%double-float radius) (%double-float thickness)
                                   (%double-float start) (%double-float end) rgba))))))
          (dolist (hit (getf plan :hits))
            (require-ui (%present-ui-hit backend (getf hit :key) (getf hit :command)
                                         (or (getf hit :hover) "") (getf hit :x) (getf hit :y) (getf hit :width) (getf hit :height))))
          (require-ui (%present-ui-end backend)))))))

(defun stage-native-settings (backend settings)
  "Stage the complete resolved settings as flat native keys; groups prefix them."
  (unless (= 1 (%present-settings backend)) (error "Cannot stage compositor settings."))
  (labels ((number (name value)
             (unless (= 1 (%present-setting backend name (%double-float value)))
               (error "Cannot stage setting ~A." name)))
           (text (name value)
             (unless (= 1 (%present-setting-text backend name value))
               (error "Cannot stage setting ~A." name)))
           (device (plist prefix)
             (loop for (key value) on plist by #'cddr
                   for name = (format nil "~A-~(~A~)" prefix key) do
               (cond ((keywordp value) (text name (string-downcase value)))
                     ((member value '(t nil)) (number name (if value 1 0)))
                     (t (number name value)))))
           (stage (plist table prefix)
             (loop for (key type) in table
                   for value = (getf plist key)
                   for name = (format nil "~@[~A-~]~(~A~)" prefix key) do
               (ecase (if (consp type) (first type) type)
                 (:boolean (number name (if value 1 0)))
                 ((:integer :real) (number name value))
                 (:color (number name (%ui-color value)))
                 (:member (text name (string-downcase value)))
                 (:strings (dolist (item value) (text name item)))
                 (:device (device value name))
                 (:animations
                  (loop for (property spec) on value by #'cddr
                        for prefix = (format nil "~A-~(~A~)" name property) do
                    (number (format nil "~A-kind" prefix) (ecase (getf spec :kind) (:off 0) (:spring 1) (:ease 2)))
                    (loop for key in '(:damping-ratio :stiffness :epsilon :duration-ms)
                          when (getf spec key) do (number (format nil "~A-~(~A~)" prefix key) (getf spec key)))
                    (let ((curve (getf spec :curve)))
                      (when curve
                        (number (format nil "~A-curve" prefix)
                                (if (listp curve) 4
                                    (position curve '(:linear :ease-out-quad :ease-out-cubic :ease-out-expo))))
                        (when (listp curve)
                          (loop for point in curve for axis in '("x1" "y1" "x2" "y2")
                                do (number (format nil "~A-~A" prefix axis) point)))))))
                 (:devices (loop for (device-name . plist) in value
                                 do (text name device-name) (device plist "device")))
                 (:group (stage value (rest type) name))))))
    (stage settings +settings+ nil)))

(defun configure-native-presentation (backend outputs context overrides restack
                                      outputs-changed bindings-changed grab
                                      &optional keyboard-changed settings-changed)
  "Prepare all owned native state, then publish the accepted presentation once."
  (unwind-protect
       (progn
         (when outputs-changed (stage-native-outputs backend outputs))
         (preview-native-layers backend (getf context :outputs)
                                (getf context :layers) overrides)
         (let ((view (getf context :view)))
           (unless (= 1 (%present-begin backend (getf view :x) (getf view :y)
                                       (getf view :zoom) (or (getf context :focus) 0)
                                       (if restack 1 0) (if outputs-changed 1 0)
                                       (if bindings-changed 1 0) (or (first grab) 0)
                                       (ecase (second grab) ((nil) 0) (:move 1) (:resize 2) (:pointer 3))))
             (error "Cannot allocate candidate presentation.")))
         (when keyboard-changed
           (let ((config (getf context :keyboard)))
             (unless (= 1 (%present-keyboard backend (getf config :rules) (getf config :model)
                                            (getf config :layout) (getf config :variant) (getf config :options)
                                            (getf config :repeat-rate) (getf config :repeat-delay)))
               (error "Cannot prepare keyboard configuration (invalid XKB names or allocation failure)."))))
         (when settings-changed (stage-native-settings backend (getf context :settings)))
         (let ((grab (getf context :keyboard-grab)))
           (unless (= 1 (%present-keyboard-grab backend (or (getf grab :owner) "") (or (getf grab :source-id) 0)
                                               (or (getf grab :otherwise) "")))
             (error "Cannot stage the keyboard grab.")))
         (when bindings-changed
           (dolist (binding (getf context :bindings))
             (unless (= 1 (%present-bind backend (getf binding :modifiers)
                                        (getf binding :code) (getf binding :owner)
                                        (getf binding :command) (getf binding :release)
                                        (getf binding :source-id)))
               (error "Cannot allocate candidate binding."))))
         (dolist (window (getf context :layout))
           (unless (= 1 (%present-window backend (getf window :id)
                                        (getf window :x) (getf window :y)
                                        (getf window :width) (getf window :height)
                                        (if (getf window :visible) 1 0)
                                        (if (getf window :fullscreen) 1 0)
                                        (if (getf window :maximize) 1 0)))
             (error "Cannot stage window ~D." (getf window :id)))
           (let* ((properties (getf window :properties)) (border (getf properties :border)))
             (flet ((tri (key) (let ((entry (member key properties))) (if entry (if (second entry) 1 0) -1)))
                    (color (key) (let ((value (getf border key))) (if value (%ui-color value) -1))))
               (unless (= 1 (%present-window-style backend (getf window :id)
                                                   (getf properties :radius -1) (tri :blur) (tri :tearing)
                                                   (color :focused) (color :unfocused)))
                 (error "Cannot stage window ~D properties." (getf window :id))))))
         (dolist (id (or (getf context :stacking) '(0)))
           (unless (= 1 (%present-stack backend id))
             (error "Cannot stage window stacking.")))
         (stage-native-ui backend (getf context :surface-plans))
         (let ((message (%present-apply backend)))
           (when message
             (if outputs-changed
                 (error 'output-configuration-error :format-control "~A"
                        :format-arguments (list message))
                 (error "~A" message)))))
    (%present-abort backend)))

(defun open-backend (socket)
  (let ((library (sb-ext:posix-getenv "TOMOE_BACKEND_LIB")))
    (unless library (error "TOMOE_BACKEND_LIB must name libtomoe-backend.so."))
    (sb-alien:load-shared-object library))
  (unless (= (%abi) +native-abi-version+)
    (error "Native ABI mismatch, expected ~D." +native-abi-version+))
  (let ((server (%create socket)))
    (when (sb-alien:null-alien server) (error "Cannot start wlroots. See native error above."))
    server))
