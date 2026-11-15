(in-package #:tomoe-user)

(defparameter +wave+ 20 "Degrees of lean at +wave-speed+ and beyond.")
(defparameter +wave-speed+ 4000 "Pointer speed, in pixels per second, that leans fully.")

(defparameter +wave-spec+
  (list :rotation (list :speed (/ +wave+ +wave-speed+) :above 4 :below +wave-speed+)
        :settle 0.07)
  "Lean the cursor with the pointer's speed, in either direction.")

(define-extension "cursor-wave" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (cursor-effect "wave" +wave-spec+ :order 15)) nil))
