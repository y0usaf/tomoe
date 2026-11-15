(in-package #:tomoe)

(defun %ui-list (value limit description)
  (let ((tail value) (count 0))
    (loop while (consp tail) do
      (when (> (incf count) limit) (error "~A exceeds its list budget." description))
      (setf tail (cdr tail)))
    (unless (null tail) (error "~A must be a proper list." description)))
  value)

(defun %ui-properties (properties allowed)
  (%ui-list properties 64 "UI properties")
  (unless (evenp (length properties)) (error "UI properties need key/value pairs."))
  (let ((seen nil))
    (loop for key in properties by #'cddr do
      (unless (member key allowed) (error "Unsupported UI property ~S." key))
      (when (member key seen) (error "Duplicate UI property ~S." key))
      (push key seen)))
  properties)

(defun %ui-length (value &optional nullable signed)
  (if (and nullable (null value)) nil
      (let ((number (%double-float value)))
        (unless (<= (if signed -16384d0 0d0) number 16384d0)
          (error "UI length is outside its supported range: ~S." value))
        number)))

(defun %ui-string (value limit &optional (empty t))
  (unless (and (stringp value) (<= (length value) limit)
               (or empty (plusp (length value)))
               (not (find (code-char 0) value)))
    (error "Expected a bounded UI string without NUL characters."))
  value)

(defun %ui-key (value &optional nullable)
  (if (and nullable (null value)) nil
      (progn
        (unless (keywordp value) (error "UI names and commands must be keywords."))
        (%ui-string (symbol-name value) 128 nil)
        value)))

(defun %ui-unique (values)
  (or (null values)
      (and (not (member (first values) (rest values)))
           (%ui-unique (rest values)))))

(defun %ui-color (value &optional nullable)
  (cond ((and nullable (null value)) nil)
        ((and (integerp value) (<= 0 value 4294967295)) value)
        ((and (stringp value) (member (length value) '(4 7 9))
              (char= (char value 0) #\#)
              (every (lambda (c) (digit-char-p c 16)) (subseq value 1)))
         (let* ((number (parse-integer value :start 1 :radix 16))
                (digits (length value)))
           (case digits
             (4 (let* ((red (floor number 256))
                       (green (floor (mod number 256) 16))
                       (blue (mod number 16)))
                  (+ (* 17 red 16777216) (* 17 green 65536) (* 17 blue 256) 255)))
             (7 (+ (* number 256) 255))
             (9 number))))
        (t (error "UI color must be RRGGBBAA or #rgb/#rrggbb/#rrggbbaa."))))

(defun %ui-edges (value &optional signed)
  (if (%finite-real-p value)
      (make-list 4 :initial-element (%ui-length value nil signed))
      (progn
        (%ui-list value 4 "UI edges")
        (unless (= (length value) 4) (error "UI edges need top/right/bottom/left."))
        (mapcar (lambda (edge) (%ui-length edge nil signed)) value))))

(defun %ui-choice (value choices)
  (unless (member value choices) (error "Unsupported UI value ~S; expected one of ~S." value choices))
  value)

(defun %ui-default (kind property)
  (case property
    (:height (when (eq kind :progress) 4d0))
    (:grow (if (eq kind :spacer) 1d0 0d0))
    (:radius (if (eq kind :progress) 2d0 0d0))
    (:border 0d0)
    (:gap (if (eq kind :button) 4d0 0d0))
    (:padding '(0d0 0d0 0d0 0d0))
    (:align (if (member kind '(:row :button)) :center :start))
    (:justify :start)
    (:text "")
    (:font "sans")
    (:rotate 0)
    (:size (cond ((member kind '(:icon :circular-progress)) 16d0)))
    (:orientation :horizontal)
    (:thickness (if (eq kind :circular-progress) 2d0 1d0))
    (:color (case kind (:separator #x45475aff)
                  ((:progress :circular-progress) #x89b4faff)))
    (:track #x313244ff)
    (:value 0d0)))

(defun %ui-property (tree property)
  (getf tree property (%ui-default (getf tree :kind) property)))

(defun canonical-ui (tree)
  "Validate and copy a bounded UI tree. Omitted text styles remain inherited."
  (let ((count 0))
    (labels ((walk (node depth)
               (when (or (> (incf count) 512) (> depth 32))
                 (error "UI tree exceeds 512 nodes or depth 32."))
               (let* ((kind (progn (%ui-list node 64 "UI node") (getf node :kind)))
                      (common '(:kind :width :height :grow :background :border-color
                                :radius :border :key :on-click :on-hover))
                      (extra
                        (case kind
                          ((:row :column :button) '(:children :gap :padding :align :justify))
                          (:stack '(:children))
                          (:text '(:text :size :line-height :color :font :rotate))
                          (:image '(:src))
                          (:icon '(:name :path :size :color))
                          ((:rect :spacer) nil)
                          (:separator '(:orientation :thickness :color))
                          (:progress '(:value :color :track))
                          (:circular-progress '(:size :thickness :value :color :track))
                          (otherwise (error "Unsupported UI kind ~S." kind))))
                      (result (list :kind kind)))
                 (%ui-properties node (append common extra))
                 (dolist (key (append (cdr common) extra))
                   (let* ((default (%ui-default kind key))
                          (value (getf node key default))
                          (normalized
                            (case key
                              ((:width :height :line-height) (%ui-length value t))
                              (:size (%ui-length value (eq kind :text)))
                              ((:grow :radius :border :gap :thickness) (%ui-length value))
                              ((:background :border-color :color) (%ui-color value t))
                              (:track (%ui-color value))
                              ((:key :on-click :on-hover) (%ui-key value t))
                              (:padding (%ui-edges value))
                              (:align (%ui-choice value '(:start :center :end :stretch)))
                              (:justify (%ui-choice value '(:start :center :end)))
                              (:orientation (%ui-choice value '(:horizontal :vertical)))
                              (:rotate (%ui-choice value '(0 90 180 270)))
                              (:text (%ui-string value 65536))
                              (:font (%ui-string value 128 nil))
                              (:src (%ui-string value 65536 nil))
                              (:name (%ui-string value 65536 nil))
                              (:path (if (null value) nil (%ui-string value 65536)))
                              (:value (max 0d0 (min 1d0 (%double-float value))))
                              (:children
                               (%ui-list value 512 "UI children")
                               (mapcar (lambda (child) (walk child (1+ depth))) value)))))
                     (unless (equal normalized default)
                       (setf result (append result (list key normalized))))))
                 result)))
      (walk tree 1))))

(defun ui (kind &rest properties)
  "Construct an immutable-by-ownership element declaration, never a native handle."
  (canonical-ui (cons :kind (cons kind properties))))

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

(define-effect :surface
  :canonical (%effect :surface (canonical-shell-surface args))
  :key (list :surface (getf args :name))
  :reduce (push (append (list :owner (spec-name (mounted-spec mounted))
                              :source-id (spec-id (mounted-spec mounted))
                              :directory (spec-directory (mounted-spec mounted)))
                        (copy-data args))
                (materialization-surfaces m)))

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
