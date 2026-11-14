(in-package #:tomoe-user)

(define-extension "memory-refresh" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil (list (configure-output "HEADLESS-1" :mode '(1280 720 240))) nil))
