(in-package #:tomoe)

(defun %cursor-options (options allowed)
  (unless (evenp (length options)) (error "Cursor properties need key/value pairs."))
  (let ((tail options) (seen nil))
    (loop while (consp tail) do
      (let ((key (first tail)))
        (unless (member key allowed) (error "Unsupported cursor property ~S." key))
        (when (member key seen) (error "Duplicate cursor property ~S." key))
        (push key seen))
      (setf tail (cddr tail))))
  options)

(defun %cursor-clause (clause)
  (cond
    ((realp clause) (%double-float clause))
    ((not (and (consp clause) (keywordp (first clause))))
     (error "Cursor term clause must be a real or a (FIELD SCALE ...) list."))
    (t
     (let ((field (first clause)) (scale (second clause)) (options (cddr clause)))
       (unless (member field '(:speed :angle :acceleration :shake :vx :vy))
         (error "Unsupported cursor term field ~S." field))
       (unless (realp scale)
         (error "Cursor term ~S needs a real scale." field))
       (%cursor-options options '(:above :below))
       (dolist (key '(:above :below))
         (when (getf options key) (%double-float (getf options key))))
       (when (member field '(:angle :vx :vy))
         (when (getf options :above)
           (error "Cursor term ~S takes only a :BELOW cap." field)))
       (list* field (%double-float scale) options)))))

(defun %cursor-term (value)
  "Validate a cursor term: a real number, a single field clause
(FIELD SCALE [:above N] [:below N]), or a list of clauses and real constants."
  (cond ((null value) nil)
        ((realp value) (%double-float value))
        ((not (listp value))
         (error "Cursor term must be a real number or a list of clauses."))
        ((keywordp (first value)) (%cursor-term (list value)))
        (t (loop for clause in value collect (%cursor-clause clause)))))

(defun %cursor-trail (trail)
  (%cursor-options trail '(:cell :life :steps :color))
  (let ((cell (%double-float (getf trail :cell 9)))
        (life (%double-float (getf trail :life 0.35d0)))
        (steps (getf trail :steps 4))
        (color (%ui-color (getf trail :color #xb8e0ffff))))
    (unless (<= 1d0 cell 256d0) (error "Cursor trail :cell must be from 1 to 256 pixels."))
    (unless (<= 0d0 life 4d0) (error "Cursor trail :life must be from 0 to 4 seconds."))
    (unless (and (integerp steps) (<= 1 steps 8))
      (error "Cursor trail :steps must be an integer from 1 to 8."))
    (list :cell cell :life life :steps steps :color color)))

(defun %cursor-spec (spec)
  (%cursor-options spec '(:rotation :scale :stretch :stretch-angle
                         :offset-x :offset-y :settle :trail))
  (let ((settle (getf spec :settle))
        (trail (getf spec :trail)))
    (when settle
      (unless (<= 0d0 (%double-float settle) 4d0)
        (error "Cursor :settle must be from 0 to 4 seconds.")))
    (list :rotation (%cursor-term (getf spec :rotation))
          :scale (%cursor-term (getf spec :scale))
          :stretch (%cursor-term (getf spec :stretch))
          :stretch-angle (%cursor-term (getf spec :stretch-angle))
          :offset-x (%cursor-term (getf spec :offset-x))
          :offset-y (%cursor-term (getf spec :offset-y))
          :settle (when settle (%double-float settle))
          :trail (when trail (%cursor-trail trail)))))

(defun cursor-effect (name spec &key (order 0))
  "Own a cursor behaviour as declarative data the compositor evaluates every
frame, so a mounted behaviour never wakes the configuration.

SPEC is a plist of transform terms and tails:

  :ROTATION       degrees clockwise
  :SCALE          a multiplier (2 doubles the size)
  :STRETCH        a fraction stretched along :STRETCH-ANGLE degrees
  :STRETCH-ANGLE  degrees
  :OFFSET-X       pixels away from the hotspot
  :OFFSET-Y       pixels away from the hotspot
  :SETTLE         seconds the answer eases over (default 0.08)
  :TRAIL          (:CELL :LIFE :STEPS :COLOR) for the fading pixel trail

A term is a real number, one clause, or a list of clauses and real constants,
and its value is their sum. A clause is (:SPEED SCALE [:ABOVE N] [:BELOW N]) or
the same for :ACCELERATION and :SHAKE (magnitudes in pixels per second, pixels
per second squared, and the shake ratio; a magnitude at or below :ABOVE
contributes nothing and :BELOW caps it), (:ANGLE SCALE [:BELOW N]) for the
motion direction in degrees, or (:VX SCALE [:BELOW N])/(:VY SCALE [:BELOW N])
for the signed velocity, where :BELOW caps the magnitude.

Behaviours compose in ORDER, then mount order: rotations add, scales multiply,
offsets add, the last :TRAIL wins and the largest :SETTLE is used. An extension
is woken for pointer motion when it declares :POINTER in its :READS; dropping
the CURSOR-EFFECT from its result removes its contribution entirely."
  (unless (stringp name) (error "Cursor effect name must be a string."))
  (unless (<= 1 (length name) 256) (error "Cursor effect names need 1 to 256 characters."))
  (unless (integerp order) (error "Cursor effect order must be an integer."))
  (%effect :cursor-effect (list name (%cursor-spec spec) order)))

(%define-effect :cursor-effect
  (lambda (args effect)
    (destructuring-bind (name spec order) args
      (%effect :cursor-effect (list name (%cursor-spec spec) order))))
  (lambda (args) '(:cursor-effect))
  (lambda (m args mounted)
    (destructuring-bind (name spec order) args
      (setf (getf m :cursors)
            (append (getf m :cursors)
                    (list (list :owner (spec-name (mounted-spec mounted))
                                :order order :spec spec)))))))

(defun stage-cursor-effects (context)
  "Hand the ordered cursor behaviours of CONTEXT to the compositor, which
evaluates them itself on every frame the cursor is drawn."
  (%cursor-effects (getf context :cursors))
  nil)
