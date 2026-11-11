(in-package #:tomoe-user)

(defparameter +cursor-trail+ '(:shader "shaders/trail-pixels.glsl" :fps 60 :idle 0.5))

(define-extension "cursor-trail" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (shell-surface :cursor-trail (ui :stack)
                               :anchors '(:top :right :bottom :left)
                               :layer :overlay
                               :click-through t
                               :background +cursor-trail+))
          nil))
