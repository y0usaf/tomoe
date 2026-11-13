(in-package #:tomoe-user)

(defun frameclock-mode (name)
  (if (and (>= (length name) 8) (string= "HEADLESS" (subseq name 0 8)))
      '(1280 720 240)
      '(1920 1080 60)))

(defun frameclock-place (window index)
  (place (getf window :id)
         (+ 40 (* 340 (mod index 4)))
         (+ 40 (* 300 (floor index 4)))
         320 240))

(define-extension "frameclock" (:reads (:windows :outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (values nil
          (append (loop for output in (context snapshot :outputs)
                        collect (configure-output (getf output :name)
                                                  :mode (frameclock-mode (getf output :name))))
                  (loop for window in (context snapshot :windows) for index from 0
                        collect (frameclock-place window index)))
          nil))
