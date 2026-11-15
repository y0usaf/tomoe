(in-package #:tomoe)

(defun %ui-unique (values)
  (or (null values)
      (and (not (member (first values) (rest values)))
           (%ui-unique (rest values)))))

(defun %ui-backdrop (value)
  "Normalize a shell background: a color, (:image PATH [:fit FIT]),
(:shader PATH [:fps FPS] [:idle SECONDS]) or (:trail [:cell PIXELS] [:life SECONDS]
[:steps N] [:color RGBA] [:fps FPS] [:idle SECONDS])."
  (%ui-list value 16 "Shell background")
  (let ((kind (first value)))
    (if (eq kind :trail)
        (let ((options (rest value)))
          (%ui-properties options '(:cell :life :steps :color :fps :idle))
          (let ((cell (%ui-length (getf options :cell 9)))
                (life (%ui-length (getf options :life 0.35d0)))
                (steps (getf options :steps 4))
                (fps (%double-float (getf options :fps 0)))
                (idle (%double-float (getf options :idle 0.5))))
            (unless (and (integerp steps) (<= 1 steps 8))
              (error "Shell trail :steps must be an integer from 1 to 8."))
            (unless (<= 1d0 cell 256d0) (error "Shell trail :cell must be from 1 to 256 pixels."))
            (unless (<= 0d0 life 4d0) (error "Shell trail :life must be from 0 to 4 seconds."))
            (unless (<= 0d0 fps 1000d0)
              (error "Shell trail :fps must be from 0, following the display, to 1000."))
            (unless (<= 0d0 idle 4d0) (error "Shell trail :idle must be from 0 to 4 seconds."))
            (list :trail :cell cell :life life :steps steps
                  :color (%ui-color (getf options :color #xb8e0ffff))
                  :fps fps :idle idle)))
        (let ((options (cddr value)))
          (unless (member kind '(:image :shader))
            (error "Shell background must be a color, (:image PATH [:fit FIT]), (:shader PATH [:fps FPS] [:idle SECONDS]), or (:trail [:cell PIXELS] [:life SECONDS] [:steps N] [:color RGBA] [:fps FPS] [:idle SECONDS])."))
          (%ui-properties options (if (eq kind :image) '(:fit) '(:fps :idle)))
          (list* kind (%ui-string (second value) 65536 nil)
                 (if (eq kind :image)
                     (list :fit (%ui-choice (getf options :fit :cover) '(:cover :contain :fill)))
                     (let ((fps (%double-float (getf options :fps 0)))
                           (idle (%double-float (getf options :idle 0))))
                       (unless (<= 0d0 fps 1000d0)
                         (error "Shell shader :fps must be from 0, following the display, to 1000."))
                       (unless (<= 0d0 idle 4d0) (error "Shell shader :idle must be from 0 to 4 seconds."))
                       (list :fps fps :idle idle))))))))

(defun canonical-shell-surface (arguments)
  (%ui-properties arguments '(:name :tree :width :height :anchors :margin :layer
                             :exclusive-zone :visible :output :background :color :font-size :stack
                             :click-through :slide :blur :placement :attach :attach-axes))
  (let ((anchors (getf arguments :anchors '(:top :left :right)))
        (visible (getf arguments :visible t))
        (click-through (getf arguments :click-through))
        (slide (getf arguments :slide))
        (attach (getf arguments :attach))
        (attach-axes (getf arguments :attach-axes '(:x :y)))
        (blur (getf arguments :blur)))
    (%ui-list anchors 4 "Shell anchors")
    (dolist (anchor anchors) (%ui-choice anchor '(:top :right :bottom :left)))
    (unless (%ui-unique anchors) (error "Duplicate shell anchor."))
    (unless (member visible '(t nil)) (error "Shell visibility must be T or NIL."))
    (unless (member click-through '(t nil)) (error "Shell click-through must be T or NIL."))
    (unless (member slide '(t nil)) (error "Shell slide must be T or NIL."))
    (unless (or (null attach) (and (integerp attach) (<= 0 attach 4294967295)))
      (error "Shell attach must be NIL or a window id."))
    (%ui-list attach-axes 2 "Shell attach axes")
    (dolist (axis attach-axes) (%ui-choice axis '(:x :y)))
    (unless (%ui-unique attach-axes) (error "Duplicate shell attach axis."))
    (unless (or (member blur '(t nil)) (realp blur)) (error "Shell blur must be T, NIL or a corner radius."))
    (list :name (%ui-key (getf arguments :name))
          :tree (canonical-ui (getf arguments :tree))
          :width (%ui-length (getf arguments :width 0))
          :height (%ui-length (getf arguments :height 0))
          :anchors (loop for edge in '(:top :right :bottom :left)
                         when (member edge anchors) collect edge)
          :margin (%ui-edges (getf arguments :margin 0) t)
          :placement (%ui-choice (getf arguments :placement :world) '(:world :view :follow))
          :layer (%ui-choice (getf arguments :layer :top) '(:background :bottom :top :overlay))
          :exclusive-zone (%ui-length (getf arguments :exclusive-zone 0) nil t)
          :visible visible
          :output (let ((output (getf arguments :output)))
                    (when output (%ui-string output 65536 nil)))
          :background (let ((background (getf arguments :background #x1e1e2eff)))
                        (if (consp background) (%ui-backdrop background) (%ui-color background)))
          :color (%ui-color (getf arguments :color #xcdd6f4ff))
          :font-size (%ui-length (getf arguments :font-size 13))
          :stack (%ui-key (getf arguments :stack) t)
          :click-through click-through
          :slide slide
          :attach attach
          :attach-axes (loop for axis in '(:x :y) when (member axis attach-axes) collect axis)
          :blur (if (eq blur t) 0d0 (%ui-length blur t)))))

(defun shell-surface (name tree &rest options)
  "Own a retained shell surface and its click commands in this extension.
:PLACEMENT :WORLD anchors to the output canvas, :VIEW anchors to the visible
viewport (a published :focal-frame, otherwise the output), and :FOLLOW keeps
its canvas position until the viewport leaves it, then glides in.
:ATTACH ID glues the surface to the moving window ID: every frame adds that
window's in-flight placement offset to the drawn position, on :ATTACH-AXES
(a subset of (:x :y), both by default). An attached axis never slides, so the
window's own motion carries the surface and the two cannot drift apart; an
unknown or unmapped ID attaches to nothing. :SLIDE animates the surface's own
declared position move, and a surface may use both on different axes."
  (%effect :surface (canonical-shell-surface (list* :name name :tree tree options))))

(%define-effect :surface
  (lambda (args effect)
    (%effect :surface (canonical-shell-surface args)))
  (lambda (args) (list :surface (getf args :name)))
  (lambda (m args mounted)
    (setf (getf m :surfaces)
          (cons (append (list :owner (spec-name (mounted-spec mounted))
                              :source-id (spec-id (mounted-spec mounted))
                              :directory (spec-directory (mounted-spec mounted)))
                        (copy-data args))
                (getf m :surfaces)))))

(defun shell-viewport (outputs data)
  "The visible rectangle a policy publishes as :focal-frame, in output pixels."
  (%ui-shell-viewport outputs data))

(defun resolve-shell-surfaces (declarations outputs workareas &key asset-loader viewport)
  "Return complete draw/hit plans and sequential reservations, without mutation.
VIEWPORT is the visible rectangle published for one output, if any. The layout,
measurement and draw plan of every surface are computed by the compositor."
  (let ((result (%ui-resolve declarations outputs workareas viewport)))
    (values (first result) (second result))))

(defun stage-native-ui (plans)
  "Hand every resolved surface plan to the compositor's retained shell scene."
  (%ui-stage plans)
  nil)
