(in-package #:tomoe-user)

(defparameter *scenario-log* (sb-ext:posix-getenv "TOMOE_SCENARIO_LOG"))

(defun scenario-note (line)
  (when *scenario-log*
    (with-open-file (stream *scenario-log* :direction :output :if-exists :append
                                 :if-does-not-exist :create)
      (write-line line stream))))
