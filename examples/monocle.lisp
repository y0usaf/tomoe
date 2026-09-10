(in-package #:tomoe-user)

;; A layout replacement. Only the focused window is visible; the focus and
;; command extensions keep working without knowing which layout is mounted.
(define-extension "monocle" (:reads (:windows :outputs :focus) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (focused (or (context snapshot :focus) (getf (first windows) :id))))
    (values nil
            (when output
              (loop for window in windows
                    collect (place (getf window :id) (getf output :x) (getf output :y)
                                   (getf output :width) (getf output :height)
                                   (eql (getf window :id) focused))))
            nil)))
