(in-package #:tomoe-user)

(defun phase-mode (name)
  (if (and (>= (length name) 8) (string= "HEADLESS" (subseq name 0 8)))
      '(1280 720 240)
      '(1920 1080 60)))

(defun phase-slot (index)
  (list (+ 40 (* 340 index)) 100 320 240))

(defparameter +phase-shader+ (tomoe::%getenv "TOMOE_PHASE_SHADER"))
(defparameter +phase-blur+ (tomoe::%getenv "TOMOE_PHASE_BLUR"))

(define-extension "phase" (:reads (:windows :outputs) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((windows (context snapshot :windows)))
    (values nil
            (append (when +phase-blur+ (list (settings :blur (list :enabled t))))
                    (when +phase-shader+
                      (list (shell-surface :phase-wallpaper (ui :stack)
                                           :anchors '(:top :right :bottom :left) :layer :background
                                           :background (list :shader +phase-shader+ :fps 60))))
                    (loop for output in (context snapshot :outputs)
                          collect (configure-output (getf output :name)
                                                    :mode (phase-mode (getf output :name))))
                    (loop for window in windows for index from 0
                          collect (apply #'place (getf window :id) (phase-slot index)))
                    (when +phase-blur+
                      (loop for window in windows
                            collect (window-properties (getf window :id) :blur t)))
                    nil))))
