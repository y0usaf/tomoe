(in-package #:tomoe-user)

;;; Mounted from a writable copy the trace test edits between steps, so a reload
;;; swaps the running declaration for a new one. The first line below is the
;;; value the test rewrites.

(defparameter *trace-swap-x* 30)

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

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
