(in-package #:tomoe-user)

(defparameter *scenario-log* (tomoe::%getenv "TOMOE_SCENARIO_LOG"))

(defun scenario-note (line)
  (when *scenario-log*
    (tomoe::%write-append *scenario-log* (format nil "~A~%" line))))
