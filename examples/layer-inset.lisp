(in-package #:tomoe-user)

;; A bar-aware tiling layout. Exclusive zones claimed by layer surfaces inset
;; each output's usable area, and windows are gridded inside what remains, so a
;; mapped bar shrinks the tiling area and unmapping it restores the full output.
;; Mounted after the shipped policy this replaces its tile layout.

(defparameter +inset-margin+ 12)
(defparameter +inset-gap+ 8)

(defun inset--edge (anchors)
  "The edge wlr_layer_surface_v1_get_exclusive_edge picks for ANCHORS, or NIL."
  (flet ((exactly (&rest edges)
           (and (= (length anchors) (length edges))
                (every (lambda (edge) (member edge anchors)) edges))))
    (cond ((or (exactly :top) (exactly :top :left :right)) :top)
          ((or (exactly :bottom) (exactly :bottom :left :right)) :bottom)
          ((or (exactly :left) (exactly :left :top :bottom)) :left)
          ((or (exactly :right) (exactly :right :top :bottom)) :right))))

(defun inset--edges (layers)
  "Space reserved on each edge by mapped layer surfaces, as (TOP RIGHT BOTTOM LEFT).
A surface reserves its exclusive zone plus that edge's margin, exactly what the
compositor subtracts from the output's usable area."
  (let ((top 0) (right 0) (bottom 0) (left 0))
    (dolist (surface layers (list top right bottom left))
      (let ((zone (getf surface :exclusive-zone)))
        (when (and (integerp zone) (plusp zone) (getf surface :visible))
          (destructuring-bind (top-margin right-margin bottom-margin left-margin)
              (or (getf surface :margin) '(0 0 0 0))
            (case (inset--edge (getf surface :anchors))
              (:top (incf top (+ zone top-margin)))
              (:bottom (incf bottom (+ zone bottom-margin)))
              (:left (incf left (+ zone left-margin)))
              (:right (incf right (+ zone right-margin))))))))))

(defun inset--area (output edges)
  "OUTPUT's box reduced by EDGES and the margin, as (X Y WIDTH HEIGHT)."
  (destructuring-bind (top right bottom left) edges
    (list (+ (getf output :x) left +inset-margin+)
          (+ (getf output :y) top +inset-margin+)
          (max 1 (- (getf output :width) left right (* 2 +inset-margin+)))
          (max 1 (- (getf output :height) top bottom (* 2 +inset-margin+))))))

(defun inset--output (outputs layout window)
  "The output holding WINDOW's laid-out centre, or the first output."
  (let* ((entry (find (getf window :id) layout :key (lambda (item) (getf item :id))))
         (x (if entry (+ (getf entry :x) (floor (getf entry :width) 2)) 0))
         (y (if entry (+ (getf entry :y) (floor (getf entry :height) 2)) 0)))
    (or (find-if (lambda (output)
                   (and (<= (getf output :x) x (+ (getf output :x) (getf output :width)))
                        (<= (getf output :y) y (+ (getf output :y) (getf output :height)))))
                 outputs)
        (first outputs))))

(defun inset--grid (area windows)
  "Fill AREA with equal cells, left to right and top to bottom."
  (destructuring-bind (x y width height) area
    (let* ((count (length windows))
           (columns (loop for size from 1 when (>= (* size size) count) return size))
           (rows (ceiling count columns))
           (cell-width (max 1 (floor (- width (* +inset-gap+ (1- columns))) columns)))
           (cell-height (max 1 (floor (- height (* +inset-gap+ (1- rows))) rows))))
      (loop for window in windows
            for index from 0
            for column = (mod index columns)
            for row = (floor index columns)
            collect (place (getf window :id)
                           (+ x (* column (+ cell-width +inset-gap+)))
                           (+ y (* row (+ cell-height +inset-gap+)))
                           cell-width cell-height)))))

(define-extension "layer-inset"
    (:reads (:windows :outputs :layout :layers) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((outputs (context snapshot :outputs))
         (windows (context snapshot :windows))
         (layout (context snapshot :layout))
         (edges (inset--edges (context snapshot :layers))))
    (values nil
            (loop for output in outputs
                  for group = (remove-if-not
                               (lambda (window)
                                 (eq output (inset--output outputs layout window)))
                               windows)
                  append (when group (inset--grid (inset--area output edges) group)))
            nil)))
