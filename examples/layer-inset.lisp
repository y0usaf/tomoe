(in-package #:tomoe-user)


(defparameter +inset-margin+ 12)
(defparameter +inset-gap+ 8)

(defun inset--workarea (output workareas)
  "OUTPUT's native usable area, or its full box while detached."
  (or (find (getf output :name) workareas :test #'equal
                                           :key (lambda (area) (getf area :name)))
      output))

(defun inset--area (workarea)
  "WORKAREA reduced by this policy's own margin, as (X Y WIDTH HEIGHT)."
  (list (+ (getf workarea :x) +inset-margin+)
        (+ (getf workarea :y) +inset-margin+)
        (max 1 (- (getf workarea :width) (* 2 +inset-margin+)))
        (max 1 (- (getf workarea :height) (* 2 +inset-margin+)))))

(defun inset--output (outputs layout window)
  "The tiling region holding WINDOW's world centre, or the first output.
The camera projects this world layout independently."
  (let* ((entry (find (getf window :id) layout :key (lambda (item) (getf item :id))))
         (x (if entry (+ (getf entry :x) (floor (getf entry :width) 2)) 0))
         (y (if entry (+ (getf entry :y) (floor (getf entry :height) 2)) 0)))
    (or (find-if (lambda (output)
                   (and (<= (getf output :x) x)
                        (< x (+ (getf output :x) (getf output :width)))
                        (<= (getf output :y) y)
                        (< y (+ (getf output :y) (getf output :height)))))
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
    (:reads (:windows :outputs :workareas :layout) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((outputs (context snapshot :outputs))
         (windows (context snapshot :windows))
         (layout (context snapshot :layout))
         (workareas (context snapshot :workareas)))
    (values nil
            (loop for output in outputs
                  for group = (remove-if-not
                               (lambda (window)
                                 (eq output (inset--output outputs layout window)))
                               windows)
                  append (when group
                           (inset--grid (inset--area (inset--workarea output workareas)) group)))
            nil)))
