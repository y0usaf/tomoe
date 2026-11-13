(in-package #:tomoe-user)

(defparameter +horosphere-sides+ 5)
(defparameter +horosphere-grid+ 14)
(defparameter +horosphere-reach+ 3d0)
(defparameter +horosphere-margin+ 16)
(defparameter +horosphere-limit+ 0.92d0)

(defun horosphere--point (re im)
  (cons (coerce re 'double-float) (coerce im 'double-float)))
(defun horosphere--add (a b)
  (cons (+ (car a) (car b)) (+ (cdr a) (cdr b))))
(defun horosphere--sub (a b)
  (cons (- (car a) (car b)) (- (cdr a) (cdr b))))
(defun horosphere--mul (a b)
  (cons (- (* (car a) (car b)) (* (cdr a) (cdr b)))
        (+ (* (car a) (cdr b)) (* (cdr a) (car b)))))
(defun horosphere--div (a b)
  (let ((square (horosphere--size b)))
    (cons (/ (+ (* (car a) (car b)) (* (cdr a) (cdr b))) square)
          (/ (- (* (cdr a) (car b)) (* (car a) (cdr b))) square))))
(defun horosphere--conj (a)
  (cons (car a) (- (cdr a))))
(defun horosphere--size (a)
  (+ (* (car a) (car a)) (* (cdr a) (cdr a))))
(defun horosphere--length (a)
  (sqrt (horosphere--size a)))
(defun horosphere--unit (angle)
  (cons (cos angle) (sin angle)))
(defun horosphere--scale (a factor)
  (cons (* (car a) factor) (* (cdr a) factor)))

(defun horosphere--apply (map point)
  "The disk point MAP sends POINT to. Maps are 2x2 complex matrices acting on
z or on its conjugate; any nonzero complex scale acts the same."
  (destructuring-bind (a b c d flipped) map
    (let ((local (if flipped (horosphere--conj point) point)))
      (horosphere--div (horosphere--add (horosphere--mul a local) b)
                       (horosphere--add (horosphere--mul c local) d)))))

(defun horosphere--normalize (a b c d flipped)
  "The same map at unit determinant, so long products keep a workable scale."
  (let* ((det (horosphere--sub (horosphere--mul a d) (horosphere--mul b c)))
         (size (sqrt (abs (horosphere--size det)))))
    (if (or (zerop size) (not (realp size)))
        (list a b c d flipped)
        (let ((factor (/ 1d0 size)))
          (list (horosphere--scale a factor) (horosphere--scale b factor)
                (horosphere--scale c factor) (horosphere--scale d factor) flipped)))))

(defun horosphere--compose (after before)
  "The map that applies BEFORE and then AFTER."
  (destructuring-bind (a1 b1 c1 d1 flipped1) after
    (destructuring-bind (a2 b2 c2 d2 flipped2) before
      (let ((a a2) (b b2) (c c2) (d d2))
        (when flipped1
          (setf a (horosphere--conj a2) b (horosphere--conj b2)
                c (horosphere--conj c2) d (horosphere--conj d2)))
        (horosphere--normalize (horosphere--add (horosphere--mul a1 a)
                                               (horosphere--mul b1 c))
                               (horosphere--add (horosphere--mul a1 b)
                                               (horosphere--mul b1 d))
                               (horosphere--add (horosphere--mul c1 a)
                                               (horosphere--mul d1 c))
                               (horosphere--add (horosphere--mul c1 b)
                                               (horosphere--mul d1 d))
                               (not (eq flipped1 flipped2)))))))

(defun horosphere--geodesic (first second)
  "Centre of the circle through FIRST and SECOND that is orthogonal to the unit
circle: the geodesic the two points share."
  (let* ((px (car first)) (py (cdr first)) (qx (car second)) (qy (cdr second))
         (det (- (* px qy) (* qx py)))
         (first-offset (/ (+ (horosphere--size first) 1d0) 2d0))
         (second-offset (/ (+ (horosphere--size second) 1d0) 2d0)))
    (horosphere--point (/ (- (* first-offset qy) (* second-offset py)) det)
                       (/ (- (* px second-offset) (* qx first-offset)) det))))

(defun horosphere--reflection (first second)
  "Reflection in the geodesic through FIRST and SECOND: inversion in the circle
that crosses the unit circle at right angles."
  (let ((centre (horosphere--geodesic first second)))
    (list centre (horosphere--point -1d0 0d0)
          (horosphere--point 1d0 0d0) (cons (- (car centre)) (- (cdr centre))) t)))

(defun horosphere--fundamental (sides)
  "The unit-disk corners of a regular {4 SIDES} square at the origin, and the
square's corner offset in the Klein model, where its sides are straight."
  (let* ((apothem (acosh (/ (cos (/ pi sides)) (sin (/ pi 4)))))
         (half-side (acosh (/ (cos (/ pi 4)) (sin (/ pi sides)))))
         (radius (acosh (* (cosh apothem) (cosh half-side))))
         (poincare (tanh (/ radius 2)))
         (klein (/ (tanh radius) (sqrt 2d0))))
    (values (loop for corner below 4 collect
                  (horosphere--scale (horosphere--unit (+ (/ pi 4)
                                                          (/ (* 2 pi corner) 4)))
                                     poincare))
            klein)))

(defun horosphere--key (point)
  (cons (round (* (car point) 1000000d0)) (round (* (cdr point) 1000000d0))))

(defun horosphere--tiling (sides reach)
  "Every tile of the {4 SIDES} tiling within REACH hyperbolic units of the
centre, as maps from the fundamental square, nearest first."
  (multiple-value-bind (corners square) (horosphere--fundamental sides)
    (let ((reflections (loop for corner below 4 collect
                             (horosphere--reflection (nth corner corners)
                                                    (nth (mod (1+ corner) 4) corners))))
          (identity (list (horosphere--point 1d0 0d0) (horosphere--point 0d0 0d0)
                          (horosphere--point 0d0 0d0) (horosphere--point 1d0 0d0) nil))
          (limit (tanh (/ reach 2)))
          (seen (make-hash-table :test #'equal))
          (queue nil)
          (tiles nil))
      (setf (gethash (horosphere--key (horosphere--point 0d0 0d0)) seen) t)
      (push identity queue)
      (loop while queue
            for map = (pop queue)
            for centre = (horosphere--apply map (horosphere--point 0d0 0d0))
            for radius = (horosphere--length centre)
            do (push (list :map map :centre centre :radius radius) tiles)
               (when (<= radius limit)
                 (dolist (reflection reflections)
                   (let* ((tile (horosphere--compose map reflection))
                          (next (horosphere--apply tile (horosphere--point 0d0 0d0)))
                          (key (horosphere--key next)))
                     (unless (gethash key seen)
                       (setf (gethash key seen) t)
                       (push tile queue))))))
      (values (stable-sort tiles #'< :key (lambda (tile) (getf tile :radius)))
              square))))

(multiple-value-bind (tiles square) (horosphere--tiling +horosphere-sides+ +horosphere-reach+)
  (defparameter +horosphere-tiles+ tiles)
  (defparameter +horosphere-square+ square))

(defparameter +horosphere-identity+
  (list (horosphere--point 1d0 0d0) (horosphere--point 0d0 0d0)
        (horosphere--point 0d0 0d0) (horosphere--point 1d0 0d0) nil))

(defun horosphere--translation (point)
  "The disk isometry that carries the origin to POINT."
  (list (horosphere--point 1d0 0d0) point (horosphere--conj point)
        (horosphere--point 1d0 0d0) nil))

(defun horosphere--clamped (pan)
  "PAN, pulled back inside the disk so a long drag cannot push every tile off
the rim and leave nothing to look at."
  (let* ((centre (horosphere--apply pan (horosphere--point 0d0 0d0)))
         (radius (horosphere--length centre)))
    (if (<= radius +horosphere-limit+)
        pan
        (horosphere--translation
         (horosphere--scale centre (/ +horosphere-limit+ radius))))))

(defun horosphere--displacement (pixels radius)
  "The disk displacement a drag of PIXELS across RADIUS asks for, with the screen's
downward y turned into the disk's upward one."
  (let* ((pixels (horosphere--point (car pixels) (- (cdr pixels))))
         (length (horosphere--length pixels)))
    (if (zerop length)
        (horosphere--point 0d0 0d0)
        (horosphere--scale pixels (min (/ 1d0 radius) (/ +horosphere-limit+ length))))))

(defun horosphere--tile-points (map pan centre radius square grid)
  "The tile's absolute screen points, row 0 at the window's top edge."
  (let* ((placed (horosphere--compose pan map))
         (flipped (fifth placed)))
    (loop for row from 0 to grid
          append (loop for column from 0 to grid
                       for along = (if flipped
                                       (- 1d0 (/ (* 2d0 column) grid))
                                       (- (/ (* 2d0 column) grid) 1d0))
                       for down = (- 1d0 (/ (* 2d0 row) grid))
                       for flat = (horosphere--point (* square along) (* square down))
                       for inside = (sqrt (- 1d0 (horosphere--size flat)))
                       for curved = (horosphere--scale flat (/ 1d0 (+ 1d0 inside)))
                       for reached = (horosphere--apply placed curved)
                       collect (round (+ (car centre) (* radius (car reached))))
                       collect (round (- (cdr centre) (* radius (cdr reached))))))))

(defun horosphere--bounds (points)
  (loop for (x y) on points by #'cddr
        minimize x into left maximize x into right
        minimize y into top maximize y into bottom
        finally (return (list left top right bottom))))

(define-extension "horosphere"
    (:reads (:windows :outputs :command :button :grab)
     :state (list :pan +horosphere-identity+ :drag nil))
    (snapshot state event)
  (let* ((grid +horosphere-grid+)
         (square +horosphere-square+)
         (tiles +horosphere-tiles+)
         (output (first (context snapshot :outputs)))
         (windows (stable-sort (remove-if-not (lambda (window) (getf window :id))
                                              (context snapshot :windows))
                               #'< :key (lambda (window) (getf window :id))))
         (pan (or (getf state :pan) +horosphere-identity+))
         (drag (getf state :drag))
         (type (getf event :type)))
    (let ((command (cond ((eq type :command) (getf event :name))
                         ((eq type :button) (getf event :command)))))
      (cond ((equal command "pan")
             (setf drag (list :origin (horosphere--point (getf event :x) (getf event :y))
                              :applied (horosphere--point 0d0 0d0))))
            ((equal command "pan-end")
             (setf drag nil))))
    (let* ((centre (horosphere--point (+ (getf output :x 0) (/ (getf output :width 1280) 2))
                                      (+ (getf output :y 0) (/ (getf output :height 720) 2))))
           (radius (max 64 (- (/ (min (getf output :width 1280)
                                      (getf output :height 720))
                                 2)
                              +horosphere-margin+))))
      (when (and (eq type :grab) drag)
        (let* ((pixels (horosphere--sub (horosphere--point (getf event :x) (getf event :y))
                                        (getf drag :origin)))
               (step (horosphere--sub pixels (getf drag :applied))))
          (setf (getf drag :applied) pixels)
          (unless (and (zerop (car step)) (zerop (cdr step)))
            (setf pan (horosphere--clamped
                       (horosphere--compose
                        (horosphere--translation (horosphere--displacement step radius))
                        pan))))))
      (values (list :pan pan :drag drag)
              (append
               (loop for window in windows
                     for index from 0
                     for tile = (nth (mod index (length tiles)) tiles)
                     for points = (horosphere--tile-points (getf tile :map) pan centre
                                                          radius square grid)
                     for bounds = (horosphere--bounds points)
                     for left = (first bounds)
                     for top = (second bounds)
                     append (list (place (getf window :id) left top
                                         (- (third bounds) left) (- (fourth bounds) top) t)
                                  (window-properties (getf window :id)
                                                     :mesh-grid (cons grid grid)
                                                     :mesh (loop for (x y) on points
                                                                 by #'cddr
                                                                 collect (- x left)
                                                                 collect (- y top)))))
               (loop for window in (reverse windows)
                     collect (raise-window (getf window :id)))
               (when drag (list (grab nil :pointer)))
               (list (define-command "pan" :description "Start dragging the sphere.")
                     (define-command "pan-end" :description "Stop dragging the sphere.")
                     (bind-button nil :middle "pan" :release "pan-end")))))))
