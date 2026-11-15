(in-package #:tomoe-user)

;;; Second, later owner of the shared key: its mount wins, its unmount exposes
;;; the first owner's value again.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

(define-extension "trace-mount-b" (:reads (:button) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (trace--record "trace-mount-b ~A" (trace--event event))
  (values state
          (list (publish-state :trace-shared (list :owner "b"))
                (interval :trace-mount-tick 600000))
          nil))
