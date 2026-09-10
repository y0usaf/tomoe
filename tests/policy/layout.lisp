(in-package #:tomoe-user)

;; Integration fixture: place every window at a fixed offset inside the first
;; output, so unmounting this extension has an observable effect. It also owns
;; one Alt binding: the modifier mask the native backend reports for Alt was
;; rejected for a while, which refused every extension that bound it. Not
;; shipped policy; the integration check mounts it by path.
(define-extension "test-layout" (:reads (:windows :outputs :key) :state nil)
    (snapshot state event)
  (declare (ignore event))
  (let ((output (first (context snapshot :outputs))))
    (values nil
            (append
             (list (bind-key '(:alt :shift) "j" :layout-down))
             (when output
               (loop for window in (context snapshot :windows)
                     for index from 0
                     collect (place (getf window :id)
                                    (+ (getf output :x) 20 (* index 40))
                                    (+ (getf output :y) 30 (* index 40))
                                    200 120))))
            nil)))

