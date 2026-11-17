(in-package #:tomoe-user)

(define-extension "livecore-check" (:reads (:windows :outputs) :state 0) (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (steps (if (and (getf event :type) (eql (getf event :type) :command))
                    (1+ (or state 0))
                    (or state 0))))
    (values steps
            (append (loop for window in windows
                          for index from 0
                          collect (place (getf window :id) (+ 8 (* 24 index)) (+ 8 (* 24 index))
                                         400 300))
                    (when windows (list (focus (getf (car (last windows)) :id))))
                    (list (define-command "livecore-step")
                          (bind-key nil "a" "livecore-step")
                          (publish-state :livecore-steps steps)))
            nil)))
