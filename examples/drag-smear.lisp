(in-package #:tomoe-user)

(defparameter +drag-smear+
  '(:shader "shaders/drag-smear.glsl" :shader-margin 512 :shader-settle 0.45d0))

(define-extension "drag-smear" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore event))
  (values nil
          (loop for window in (context snapshot :windows)
                collect (apply #'window-properties (getf window :id) +drag-smear+))
          nil))
