(in-package #:tomoe-user)

;; The host does not recognize these names. Each unit can be unmounted or
;; replaced with the same API available to a config file.

;;; ---------------------------------------------------------------- utilities

(defun %layer-exclusive-edge (anchors)
  "The one edge a layer surface reserves. Layer-shell honours an exclusive zone
only for a surface anchored to a single edge, or to that edge plus both
perpendicular ones; a corner, both edges of an axis, or no anchor reserves
nothing."
  (flet ((exactly (&rest edges)
           (and (= (length anchors) (length edges))
                (every (lambda (edge) (member edge anchors)) edges))))
    (cond ((or (exactly :top) (exactly :top :left :right)) :top)
          ((or (exactly :bottom) (exactly :bottom :left :right)) :bottom)
          ((or (exactly :left) (exactly :left :top :bottom)) :left)
          ((or (exactly :right) (exactly :right :top :bottom)) :right))))

(defun %layer-insets (layers)
  "Exclusive zones the visible layer surfaces take from the tiling area, as a
plist of :top :right :bottom :left. Each surface reserves its zone plus that
edge's margin on the single edge its anchors name; a hidden surface, a
non-positive zone, or an anchor set with no exclusive edge reserves nothing."
  (let ((top 0) (right 0) (bottom 0) (left 0))
    (dolist (layer layers)
      (let ((zone (getf layer :exclusive-zone))
            (margin (getf layer :margin)))
        (when (and (getf layer :visible) (integerp zone) (plusp zone))
          (let* ((edge (%layer-exclusive-edge (getf layer :anchors)))
                 (offset (case edge (:top (first margin)) (:right (second margin))
                                 (:bottom (third margin)) (:left (fourth margin))))
                 (inset (+ zone (or offset 0))))
            (when (plusp inset)
              (case edge
                (:top (incf top inset))
                (:right (incf right inset))
                (:bottom (incf bottom inset))
                (:left (incf left inset))))))))
    (list :top top :right right :bottom bottom :left left)))

(defun %state-flag (state id key)
  "The boolean STATE records for ID's KEY, or NIL for an untracked window."
  (let ((entry (assoc id state)))
    (and entry (cdr (assoc key (cdr entry))))))

(defun %state-put (state id key value)
  "STATE maps a window id to (:fullscreen . B) (:maximize . B). Return STATE
with ID's KEY set to VALUE; entries stay plain data."
  (let ((entry (assoc id state)))
    (flet ((flag (name) (if (eq name key) value (and entry (cdr (assoc name (cdr entry)))))))
      (cons (list id (cons :fullscreen (flag :fullscreen)) (cons :maximize (flag :maximize)))
            (remove id state :key #'car)))))

(defun %drag-box (state dx dy)
  "The dragged box for a pointer delta from the press point: a move shifts the
press box, a resize changes its size. Sizes never shrink below 64 units and stay
inside PLACE's bounds."
  (labels ((bound (value limit) (min limit (max (- limit) value)))
           (size (value) (min 16384 (max 64 value))))
    (if (eq (getf state :mode) :move)
        (list (bound (+ (getf state :base-x) dx) 1048576)
              (bound (+ (getf state :base-y) dy) 1048576)
              (getf state :base-width) (getf state :base-height))
        (list (getf state :base-x) (getf state :base-y)
              (size (+ (getf state :base-width) dx))
              (size (+ (getf state :base-height) dy))))))

;;; ------------------------------------------------------------------- policy

(define-extension "tiles" (:reads (:windows :outputs :layers) :state 8) (snapshot gap event)
  (declare (ignore event))
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (count (length windows)))
    (values gap
            (when (and output windows)
              (let* ((insets (%layer-insets (context snapshot :layers)))
                     (x (+ (getf output :x) (getf insets :left)))
                     (y (+ (getf output :y) (getf insets :top)))
                     (width (max 1 (- (getf output :width)
                                      (getf insets :left) (getf insets :right))))
                     (height (max 1 (- (getf output :height)
                                       (getf insets :top) (getf insets :bottom))))
                     (gap (min gap (floor width (1+ (* 2 count))) (floor height 3)))
                     (available (- width (* gap (1+ count)))))
                (loop for window in windows for i from 0
                      for left = (floor (* available i) count)
                      for right = (floor (* available (1+ i)) count)
                      collect (place (getf window :id)
                                     (+ x gap (* i gap) left)
                                     (+ y gap)
                                     (max 1 (- right left)) (max 1 (- height (* gap 2)))))))
            nil)))

(define-extension "focus" (:reads (:windows :button :key) :state nil) (snapshot focused event)
  (let ((ids (mapcar (lambda (w) (getf w :id)) (context snapshot :windows))))
    (unless (member focused ids) (setf focused (first ids)))
    (case (getf event :type)
      (:map (setf focused (getf event :id)))
      (:button (setf focused (let ((id (getf event :id))) (when (member id ids) id))))
      (:key
       (when (and (equal (getf event :owner) "focus") (equal (getf event :command) "next"))
         (setf focused (or (second (member focused ids)) (first ids))))))
    (values focused (list (focus focused) (bind-key '(:super) "Tab" :next)) nil)))

(define-extension "window-state" (:reads (:windows :outputs :layout :focus :key) :state nil)
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (layout (context snapshot :layout))
         (output (first (context snapshot :outputs)))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (type (getf event :type)))
    ;; A window that is gone cannot hold state. A flag the resolved layout no
    ;; longer takes from this unit was overridden by a later owner (drag), and
    ;; re-asserting it would fight that owner forever.
    (setf state (remove-if-not (lambda (entry) (member (car entry) ids)) state))
    (dolist (entry state)
      (dolist (key '(:fullscreen :maximize))
        (when (and (%state-flag state (car entry) key)
                   (not (getf (find (car entry) layout :key (lambda (window) (getf window :id))) key)))
          (setf state (%state-put state (car entry) key nil)))))
    (let ((change (cond ((and (eq type :key) (equal (getf event :owner) "window-state")
                              (member (context snapshot :focus) ids))
                         (let ((command (getf event :command))
                               (focused (context snapshot :focus)))
                           (cond ((equal command "fullscreen") (cons focused :fullscreen))
                                 ((equal command "maximize") (cons focused :maximize)))))
                        ((and (eq type :metadata) (member (getf event :request) '(:fullscreen :maximize))
                              (member (getf event :id) ids))
                         (cons (getf event :id) (getf event :request))))))
      (when change
        (setf state (if (eq type :metadata)
                        (%state-put state (car change) (cdr change) t)
                        (%state-put state (car change) (cdr change)
                                    (not (%state-flag state (car change) (cdr change)))))))
      (values state
              (append (list (bind-key '(:super) "f" :fullscreen)
                            (bind-key '(:super) "m" :maximize))
                      (loop for entry in state
                            for id = (car entry)
                            for fullscreen = (not (null (%state-flag state id :fullscreen)))
                            for maximize = (not (null (%state-flag state id :maximize)))
                            append (append (list (fullscreen id fullscreen) (maximize id maximize))
                                           (when (and fullscreen output)
                                             (list (place id (getf output :x) (getf output :y)
                                                          (max 1 (getf output :width))
                                                          (max 1 (getf output :height)) t))))))
              nil))))

(define-extension "drag" (:reads (:windows :layout :button :grab) :state nil) (snapshot state event)
  (let* ((layout (context snapshot :layout))
         (ids (mapcar (lambda (window) (getf window :id)) (context snapshot :windows)))
         (type (getf event :type))
         (id (getf state :id))
         (modifiers (getf event :modifiers))
         (target (find (getf event :id) layout :key (lambda (window) (getf window :id)))))
    (cond
      ;; A grabbed window that disappears cannot keep the grab.
      ((and state (not (member id ids))) (values nil nil nil))
      ;; Super plus the left button moves a window, the right button resizes it;
      ;; both start from the box the layout currently resolves for it.
      ((and (eq type :button) (eq (getf event :state) :pressed) target
            (member (getf event :id) ids) (member (getf event :button) '(272 273))
            (integerp modifiers) (logtest 64 modifiers))
       (let ((window-id (getf event :id))
             (mode (if (eql (getf event :button) 272) :move :resize))
             (box (list (getf target :x) (getf target :y)
                        (getf target :width) (getf target :height))))
         (values (list :id window-id :mode mode
                       :origin-x (getf event :x) :origin-y (getf event :y)
                       :base-x (first box) :base-y (second box)
                       :base-width (third box) :base-height (fourth box)
                       :x (first box) :y (second box) :width (third box) :height (fourth box))
                 (list (grab window-id mode) (apply #'place window-id box)
                       (fullscreen window-id nil))
                 nil)))
      ((and (eq type :button) (eq (getf event :state) :released) state (eql (getf event :id) id))
       (values nil nil nil))
      ((and (eq type :grab) state (eql (getf event :id) id))
       (let* ((dx (- (getf event :x) (getf state :origin-x)))
              (dy (- (getf event :y) (getf state :origin-y)))
              (box (%drag-box state dx dy)))
         (values (list :id id :mode (getf state :mode)
                       :origin-x (getf state :origin-x) :origin-y (getf state :origin-y)
                       :base-x (getf state :base-x) :base-y (getf state :base-y)
                       :base-width (getf state :base-width) :base-height (getf state :base-height)
                       :x (first box) :y (second box) :width (third box) :height (fourth box))
                 (list (grab id (getf state :mode)) (apply #'place id box)
                       (fullscreen id nil))
                 nil)))
      ;; Any other event while a drag is active re-asserts the grab and the box.
      (state
       (values state
               (list (grab id (getf state :mode))
                     (place id (getf state :x) (getf state :y)
                            (getf state :width) (getf state :height) t)
                     (fullscreen id nil))
               nil))
      (t (values nil nil nil)))))

(define-extension "commands" (:reads (:key :focus) :state nil) (snapshot state event)
  (declare (ignore state))
  (let ((focused (context snapshot :focus))
        (command (when (and (eq (getf event :type) :key) (equal (getf event :owner) "commands"))
                   (getf event :command))))
    (values nil
            (list (bind-key '(:super) "Return" :terminal)
                  (bind-key '(:super) "q" :close)
                  (bind-key '(:super :shift) "r" :reload)
                  (bind-key '(:super :shift) "Escape" :quit))
            (cond ((equal command "terminal") (list (launch "foot")))
                  ((and (equal command "close") focused) (list (close-window focused)))
                  ((equal command "reload") (list (reload)))
                  ((equal command "quit") (list (quit)))))))
