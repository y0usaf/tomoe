(defun full-workareas (outputs)
  (loop for output in outputs collect
    (list :name (getf output :name) :x (getf output :x) :y (getf output :y)
          :width (getf output :width) :height (getf output :height))))

(defun stage-native-output-config (output)
  (list :name (getf output :name)
        :mode (getf output :mode) :width (getf output :width) :height (getf output :height)
        :refresh-mhz (getf output :refresh-mhz) :scale-120 (getf output :scale-120)
        :x (getf output :x) :y (getf output :y) :positioned (getf output :positioned)
        :disabled (getf output :disabled) :mirror (getf output :mirror)
        :vrr (getf output :vrr) :icc (getf output :icc)
        :transform (or (getf output :transform) -1) :tearing (getf output :tearing)
        :hold (getf output :hold)))

(defun stage-native-outputs (outputs)
  (%plan-outputs (mapcar #'stage-native-output-config outputs)))

(defun preview-native-outputs (outputs)
  "Resolve candidate active outputs and connected ports without native writes."
  (stage-native-outputs outputs)
  (let ((preview (%outputs-preview)))
    (unless preview (error "Cannot allocate output configuration preview."))
    preview))

(defun current-native-outputs ()
  "Read currently committed output facts without changing native state."
  (let ((snapshot (%outputs-current)))
    (unless (and (listp snapshot)
                 (integerp (getf snapshot :revision))
                 (<= 0 (getf snapshot :revision))
                 (listp (getf snapshot :outputs)))
      (error "Native output snapshot is invalid: ~S" snapshot))
    snapshot))

(defun preview-native-layers (outputs layers overrides)
  "Resolve layer placement and usable output areas without sending configures."
  (%plan-layers (list :outputs outputs :layers layers :overrides overrides))
  (let ((preview (%layers-preview)))
    (unless preview (error "Cannot serialize layer preview."))
    (values (getf preview :layers) (getf preview :workareas))))

(defun configure-virtual-outputs (old new)
  (declare (ignore old))
  (%plan-virtual-outputs new))

(defun %stage-setting-flat (name value flat)
  (cond ((keywordp value) (append flat (list name (string-downcase value))))
        ((member value '(t nil)) (append flat (list name (if value 1 0))))
        (t (append flat (list name value)))))

(defun %stage-device (plist prefix flat)
  (loop for (key value) on plist by #'cddr
        for name = (format nil "~A-~A" prefix (%lower key))
        do (setf flat (%stage-setting-flat name value flat))
        finally (return flat)))

(defun %stage-settings (plist table prefix flat)
  (loop for (key type) in table
        for value = (getf plist key)
        for name = (format nil "~A~A" (if prefix (concatenate 'string prefix "-") "")
                           (%lower key))
        do (setf flat
                 (ecase (if (consp type) (first type) type)
                   (:boolean (%stage-setting-flat name (if value 1 0) flat))
                   ((:integer :real) (%stage-setting-flat name value flat))
                   (:color (%stage-setting-flat name (%ui-color value) flat))
                   (:member (%stage-setting-flat name (string-downcase value) flat))
                   (:strings (dolist (item value) (setf flat (append flat (list name item)))) flat)
                   (:device (%stage-device value name flat))
                   (:animations
                    (let ((flat flat))
                      (loop for (property spec) on value by #'cddr
                            for prefix = (format nil "~A-~A" name (%lower property)) do
                        (setf flat (%stage-setting-flat
                                    (concatenate 'string prefix "-kind")
                                    (ecase (getf spec :kind) (:off 0) (:spring 1) (:ease 2)) flat))
                        (loop for field in '(:damping-ratio :stiffness :epsilon :launch :duration-ms)
                              when (getf spec field)
                                do (setf flat (%stage-setting-flat
                                               (format nil "~A-~A" prefix (%lower field))
                                               (getf spec field) flat)))
                        (let ((curve (getf spec :curve)))
                          (when curve
                            (setf flat (%stage-setting-flat
                                        (concatenate 'string prefix "-curve")
                                        (if (listp curve) 4
                                            (position curve '(:linear :ease-out-quad :ease-out-cubic :ease-out-expo)))
                                        flat))
                            (when (listp curve)
                              (loop for point in curve for axis in '("x1" "y1" "x2" "y2")
                                    do (setf flat (%stage-setting-flat
                                                   (format nil "~A-~A" prefix axis) point flat)))))))
                      flat))
                   (:devices
                    (loop for (device-name . plist) in value
                          do (setf flat (append flat (list name device-name)))
                             (setf flat (%stage-device plist "device" flat)))
                    flat)
                   (:group (%stage-settings value (rest type) name flat))))
        finally (return flat)))

(defun stage-native-settings (settings)
  "Stage the complete resolved settings as flat native keys; groups prefix them."
  (%plan-settings (%stage-settings settings +settings+ nil nil)))

(defun stage-native-motion (motion)
  "Stage the pointer gate: nothing on the fast path unless MOTION says so."
  (when motion (%plan-motion motion)))

(defun configure-native-presentation (context overrides restack outputs-changed bindings-changed
                                      grab motion keyboard-changed settings-changed sounds-changed)
  "Prepare all owned native state, then publish the accepted presentation once."
  (let ((failure nil))
    (%catch
     (lambda ()
       (let ((view (getf context :view)))
         (%plan-begin (getf view :x) (getf view :y) (getf view :zoom)
                      (or (getf context :focus) 0)
                      (or (first grab) 0)
                      (ecase (second grab) ((nil) 0) (:move 1) (:resize 2) (:pointer 3)))
         (when outputs-changed
           (stage-native-outputs (getf context :native-output-config)))
         (preview-native-layers (getf context :outputs) (getf context :layers) overrides)
         (stage-native-motion motion)
         (when keyboard-changed
           (let ((config (getf context :keyboard)))
             (%plan-keyboard (getf config :rules) (getf config :model)
                             (getf config :layout) (getf config :variant) (getf config :options)
                             (getf config :repeat-rate) (getf config :repeat-delay))))
         (when settings-changed (stage-native-settings (getf context :settings)))
         (when sounds-changed (%plan-sounds (getf context :sounds)))
         (let ((keyboard-grab (getf context :keyboard-grab)))
           (%plan-keyboard-grab (or (getf keyboard-grab :owner) "")
                                (or (getf keyboard-grab :source-id) 0)
                                (or (getf keyboard-grab :otherwise) "")))
         (when bindings-changed
           (%plan-bindings
            (loop for binding in (getf context :bindings)
                  collect (list (getf binding :modifiers) (getf binding :code)
                                (getf binding :owner) (getf binding :command)
                                (getf binding :release) (getf binding :source-id)))))
         (dolist (window (getf context :layout))
           (%plan-window-window window))
         (%plan-stack (getf context :stacking))
         (%plan-surfaces (getf context :surface-plans))
         (let ((message (%plan-commit)))
           (when message (error "~A" message)))))
     (lambda (text) (setf failure text)))
    (when failure
      (%plan-abort)
      (error "~A" failure))))

(defun %stage-border (properties)
  (let ((border (getf properties :border)))
    (list :focused (if (getf border :focused) (%ui-color (getf border :focused)) -1)
          :unfocused (if (getf border :unfocused) (%ui-color (getf border :unfocused)) -1))))

(defun %stage-tri (properties key)
  (let ((entry (member key properties)))
    (if entry (if (second entry) 1 0) -1)))

(defun %plan-window-window (window)
  "Stage one window's geometry, style, visual, shader and mesh."
  (let* ((properties (getf window :properties))
         (translate (getf properties :translate))
         (grid (getf properties :shader-grid))
         (mesh-grid (getf properties :mesh-grid)))
    (%plan-window (getf window :id) (getf window :x) (getf window :y)
                  (getf window :width) (getf window :height)
                  (list :visible (if (getf window :visible) 1 0)
                        :fullscreen (if (getf window :fullscreen) 1 0)
                        :maximize (if (getf window :maximize) 1 0))
                  (list :radius (getf properties :radius -1)
                        :blur (%stage-tri properties :blur)
                        :tearing (%stage-tri properties :tearing)
                        :border (%stage-border properties))
                  (list :alpha (%double-float (getf properties :alpha 1))
                        :tint (if (getf properties :tint) (%ui-color (getf properties :tint)) 0)
                        :scale (%double-float (getf properties :scale 1))
                        :rotation (%double-float (getf properties :rotation 0))
                        :translate-x (getf translate :x 0)
                        :translate-y (getf translate :y 0))
                  (list :path (or (getf properties :shader) "")
                        :grid-x (or (car grid) 1) :grid-y (or (cdr grid) 1)
                        :margin (or (getf properties :shader-margin) 0)
                        :settle (%double-float (or (getf properties :shader-settle) 0))
                        :entry (or (getf properties :shader-entry) "mainImage"))
                  (list :grid-x (or (car mesh-grid) 0) :grid-y (or (cdr mesh-grid) 0)
                        :positions (getf properties :mesh)))))
