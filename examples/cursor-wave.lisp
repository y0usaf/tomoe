(in-package #:tomoe-user)

;;;; A cursor behaviour of your own.
;;;;
;;;; Mount this next to the shipped ones: the cursor leans a little further with
;;;; the pointer's speed, on top of the tilt the default policy already asks for,
;;;; because cursor behaviours stack in :ORDER. Every behaviour is an ordinary
;;;; registration through CURSOR-EFFECT, so removing this file's extension takes
;;;; the lean away and leaves the rest of the cursor exactly as it was.

(defparameter +wave+ 20 "Degrees of lean at +wave-speed+ and beyond.")
(defparameter +wave-speed+ 4000 "Pointer speed, in pixels per second, that leans fully.")

(defun cursor-wave (motion)
  "Lean the cursor with the pointer's speed, in either direction."
  (let* ((vx (coerce (getf motion :vx) 'double-float))
         (vy (coerce (getf motion :vy) 'double-float))
         (speed (sqrt (+ (* vx vx) (* vy vy)))))
    (when (> speed 4)
      (list :rotation (* +wave+ (min 1d0 (/ speed +wave-speed+)))
            :settle 0.07))))

(define-extension "cursor-wave" (:reads (:pointer) :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "wave" 'cursor-wave :order 15)) nil))
