(defun full-workareas (outputs)
  (loop for output in outputs collect
    (list :name (getf output :name) :x (getf output :x) :y (getf output :y)
          :width (getf output :width) :height (getf output :height))))

(defun preview-native-layers (outputs layers overrides pending)
  "Resolve layer placement and usable output areas without sending configures, on the
staged outputs when PENDING and on the committed ones otherwise."
  (unless (= 1 (%layers-begin pending)) (error "Cannot allocate layer preview."))
  (dolist (output outputs)
    (unless (= 1 (%layers-output (getf output :name)
                                 (getf output :x) (getf output :y)
                                 (getf output :width) (getf output :height)
                                 (getf output :scale-120 120)))
      (error "Cannot stage layer output ~A." (getf output :name))))
  (dolist (layer layers)
    (let ((override (assoc (getf layer :id) overrides)))
      (unless (= 1 (%layers-surface
                    (getf layer :id)
                    (if override
                        (ecase (second override)
                          (:background 0) (:bottom 1) (:top 2) (:overlay 3) ((nil) -1))
                        -1)
                    (or (third override) -1)
                    (if override
                        (ecase (fourth override) (:none 0) (:exclusive 1) ((nil) -1))
                        -1)
                    (if override (if (fifth override) 1 0) -1)))
        (error "Cannot stage layer ~D." (getf layer :id)))))
  (let ((preview (%layers-preview)))
    (unless preview (error "Cannot serialize layer preview."))
    (values (getf preview :layers) (getf preview :workareas))))

(defun stage-native-outputs (outputs)
  (unless (= (%outputs-begin) 1) (error "Cannot allocate output configuration."))
  (dolist (output outputs)
    (if (getf output :hold)
        (unless (= 1 (%output-hold (getf output :name)))
          (error "Cannot retain current output state for ~A." (getf output :name)))
        (progn
          (unless (= 1 (%output (getf output :name)
                                (ecase (getf output :mode) (:preferred 0) (:max 1) (:exact 2))
                                (getf output :width) (getf output :height) (getf output :refresh-mhz)
                                (getf output :scale-120) (getf output :x) (getf output :y)
                                (getf output :positioned) (or (getf output :transform) -1)))
            (setf *output-error-name* (getf output :name))
            (error "Cannot stage output mode for ~A." (getf output :name)))
          (unless (= 1 (%output-options (getf output :name)
                                        (if (getf output :disabled) nil t)
                                        (or (getf output :mirror) "") (getf output :vrr)
                                        (getf output :tearing)))
            (error "Cannot stage output options for ~A." (getf output :name)))
          (unless (= 1 (%output-icc (getf output :name) (or (getf output :icc) "")))
            (error "Cannot stage the ICC profile for ~A." (getf output :name)))))))

(defun pending-native-outputs-p ()
  (= 1 (%outputs-pending)))

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

(defun configure-virtual-outputs (old new)
  (dolist (output old)
    (unless (find (getf output :name) new :test #'equal :key (lambda (o) (getf o :name)))
      (%virtual-output (getf output :name) 0 0 0)))
  (dolist (output new)
    (unless (member output old :test #'equal)
      (%virtual-output (getf output :name) (getf output :width) (getf output :height)
                       (getf output :refresh-mhz)))))

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
  (unless (= 1 (%present-settings)) (error "Cannot stage compositor settings."))
  (labels ((number (name value)
             (unless (= 1 (%present-setting name (%double-float value)))
               (error "Cannot stage setting ~A." name)))
           (text (name value)
             (unless (= 1 (%present-setting-text name value))
               (error "Cannot stage setting ~A." name)))
           (stage (flat)
             (loop for (key value) on flat by #'cddr
                   do (if (stringp value) (text key value) (number key value)))))
    (stage (%stage-settings settings +settings+ nil nil))))

(defun stage-native-motion (motion)
  "Stage the pointer gate: nothing on the fast path unless MOTION says so."
  (when motion
    (unless (= 1 (%motion (if (getf motion :fast) 1 0)
                          (if (getf motion :hover) 1 0)
                          (if (getf motion :regions) 1 0)
                          (if (getf motion :edges) 1 0)
                          (getf motion :distance 0)))
      (error "Cannot stage the motion gate."))
    (dolist (region (getf motion :regions))
      (destructuring-bind (name x y width height) region
        (unless (= 1 (%motion-region name x y width height))
          (error "Cannot stage motion region ~A." name))))
    (dolist (edge (getf motion :edges))
      (destructuring-bind (side thickness) edge
        (unless (= 1 (%motion-edge (ecase side (:top 0) (:bottom 1) (:left 2) (:right 3))
                                   thickness))
          (error "Cannot stage motion edge ~A." (%lower side)))))
    (let ((drag (getf motion :drag)))
      (when drag
        (unless (= 1 (%motion-drag (getf drag :id)
                                   (ecase (getf drag :mode) (:move 1) (:resize 2) (:pointer 3))
                                   (getf drag :bound) (getf drag :min-size) (getf drag :max-size)))
          (error "Cannot stage the native drag."))))))

(defun %stage-border (properties)
  (let ((border (getf properties :border)))
    (list (if (getf border :focused) (%ui-color (getf border :focused)) -1)
          (if (getf border :unfocused) (%ui-color (getf border :unfocused)) -1))))

(defun %stage-tri (properties key)
  (let ((entry (member key properties)))
    (if entry (if (second entry) 1 0) -1)))

(defun %stage-window (window)
  "Stage one window: geometry, style, visual, shader and mesh."
  (let* ((id (getf window :id))
         (properties (getf window :properties))
         (border (%stage-border properties))
         (translate (getf properties :translate))
         (grid (getf properties :shader-grid))
         (mesh-grid (getf properties :mesh-grid)))
    (unless (= 1 (%present-window id (getf window :x) (getf window :y)
                                  (getf window :width) (getf window :height)
                                  (if (getf window :visible) 1 0)
                                  (if (getf window :fullscreen) 1 0)
                                  (if (getf window :maximize) 1 0)))
      (error "Cannot stage window ~D." id))
    (unless (= 1 (%present-window-style id (getf properties :radius -1)
                                        (%stage-tri properties :blur)
                                        (%stage-tri properties :tearing)
                                        (first border) (second border)))
      (error "Cannot stage window ~D properties." id))
    (unless (= 1 (%present-window-visual id
                                         (%double-float (getf properties :alpha 1))
                                         (if (getf properties :tint)
                                             (%ui-color (getf properties :tint)) 0)
                                         (%double-float (getf properties :scale 1))
                                         (%double-float (getf properties :rotation 0))
                                         (getf translate :x 0) (getf translate :y 0)))
      (error "Cannot stage window ~D visuals." id))
    (unless (= 1 (%present-window-shader id (or (getf properties :shader) "")
                                         (or (car grid) 1) (or (cdr grid) 1)
                                         (or (getf properties :shader-margin) 0)
                                         (%double-float (or (getf properties :shader-settle) 0))
                                         (or (getf properties :shader-entry) "mainImage")))
      (error "Cannot stage window ~D shader." id))
    (unless (= 1 (%present-window-mesh id (or (car mesh-grid) 0) (or (cdr mesh-grid) 0)
                                       (getf properties :mesh)))
      (error "Cannot stage window ~D mesh." id))))

(defun stage-native-sounds (sounds)
  (unless (= 1 (%present-sounds)) (error "Cannot allocate candidate sounds."))
  (loop for event in +sound-events+ for code from 0
        for entry = (getf sounds event) do
    (dolist (file (getf entry :files))
      (let ((message (%present-sound code file (getf entry :gain))))
        (when message (error "Cannot load sound ~A: ~A" file message))))))

(defun configure-native-presentation (context overrides restack outputs-changed bindings-changed
                                      grab motion keyboard-changed settings-changed sounds-changed)
  "Prepare all owned native state, then publish the accepted presentation once."
  (let ((failure nil))
    (%catch
     (lambda ()
       (when outputs-changed (stage-native-outputs (resolved-native-output-config context)))
       (preview-native-layers (getf context :outputs) (getf context :layers) overrides outputs-changed)
       (let ((view (getf context :view)))
         (unless (= 1 (%present-begin (getf view :x) (getf view :y) (getf view :zoom)
                                      (or (getf context :focus) 0)
                                      (if restack 1 0) (if outputs-changed 1 0)
                                      (if bindings-changed 1 0) (or (first grab) 0)
                                      (ecase (second grab) ((nil) 0) (:move 1) (:resize 2) (:pointer 3))))
           (error "Cannot allocate candidate presentation.")))
       (stage-native-motion motion)
       (when keyboard-changed
         (let ((config (getf context :keyboard)))
           (unless (= 1 (%present-keyboard (getf config :rules) (getf config :model)
                                           (getf config :layout) (getf config :variant)
                                           (getf config :options)
                                           (getf config :repeat-rate) (getf config :repeat-delay)))
             (error "Cannot prepare keyboard configuration (invalid XKB names or allocation failure)."))))
       (when settings-changed (stage-native-settings (getf context :settings)))
       (when sounds-changed (stage-native-sounds (getf context :sounds)))
       (let ((keyboard-grab (getf context :keyboard-grab)))
         (unless (= 1 (%present-keyboard-grab (or (getf keyboard-grab :owner) "")
                                              (or (getf keyboard-grab :source-id) 0)
                                              (or (getf keyboard-grab :otherwise) "")))
           (error "Cannot stage the keyboard grab.")))
       (when bindings-changed
         (dolist (binding (getf context :bindings))
           (unless (= 1 (%present-bind (getf binding :modifiers)
                                       (getf binding :code) (getf binding :owner)
                                       (getf binding :command) (getf binding :release)
                                       (getf binding :source-id)))
             (error "Cannot allocate candidate binding."))))
       (dolist (window (getf context :layout))
         (%stage-window window))
       (dolist (id (or (getf context :stacking) '(0)))
         (unless (= 1 (%present-stack id))
           (error "Cannot stage window stacking.")))
       (stage-native-ui (getf context :surface-plans))
       (let ((message (%present-apply)))
         (when message (error "~A" message))))
     (lambda (text) (setf failure text)))
    (%present-abort)
    (when failure
      (error "~A" failure))))
