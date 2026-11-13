(in-package #:tomoe-user)

(defparameter +close-fade+ '(:ease (:duration-ms 200 :curve :ease-out-cubic))
  "niri's close: the window keeps its last frame, shrinking and fading out.")

(define-extension "close-fade" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot event))
  (values nil (list (settings :animations (list :window-close +close-fade+))) nil))
