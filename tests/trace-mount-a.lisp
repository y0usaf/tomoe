(in-package #:tomoe-user)

;;; First owner of the shared key. Mounted before its partner, so its value is
;;; exposed again once the partner is unmounted.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

(define-extension "trace-mount-a" (:reads (:button) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (trace--record "trace-mount-a ~A" (trace--event event))
  (values state (list (publish-state :trace-shared (list :owner "a"))) nil))
