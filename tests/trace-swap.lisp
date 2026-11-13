(in-package #:tomoe-user)

;;; Mounted from a writable copy the trace test edits between steps, so a reload
;;; swaps the running declaration for a new one. The first line below is the
;;; value the test rewrites.

(defparameter *trace-swap-x* 30)

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "trace-swap" (:reads (:windows :button) :state '(:runs 0))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (runs (1+ (getf state :runs))))
    (trace--record "trace-swap ~A x=~D runs=~D" (trace--event event) *trace-swap-x* runs)
    (values (list :runs runs)
            (loop for window in windows
                  collect (place (getf window :id)
                                 (+ *trace-swap-x* (getf window :id)) 200
                                 (getf window :width) (getf window :height)))
            nil)))
