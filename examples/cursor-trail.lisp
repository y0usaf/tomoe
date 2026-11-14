(in-package #:tomoe-user)

(defparameter +cursor-trail+
  '(:trail :cell 9 :life 0.35 :steps 4 :color #xb8e0ffff :idle 0.5))

(define-extension "cursor-trail" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (shell-surface :cursor-trail (ui :stack)
                               :anchors '(:top :right :bottom :left)
                               :layer :overlay
                               :click-through t
                               :background +cursor-trail+))
          nil))
