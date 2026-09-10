(in-package #:tomoe-user)

;; Integration fixture: place every window at a fixed offset inside the first
;; output, so unmounting this extension has an observable effect. Not shipped
;; policy; the integration check mounts it by path.
(define-extension "test-layout" (:reads (:windows :outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (when output
              (loop for window in (context snapshot :windows)
                    for index from 0
                    collect (place (getf window :id)
                                   (+ (getf output :x) 20 (* index 40))
                                   (+ (getf output :y) 30 (* index 40))
                                   200 120)))
            nil)))
