(in-package #:tomoe-user)

;;; A reducer that fails when the event carries :FAIL. It keeps its last answer
;;; while the other reducer's answer commits, and the next event commits normally.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

(define-extension "trace-fail-a" (:reads (:button :data) :state '(:runs 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((runs (1+ (getf state :runs))))
    (trace--record "trace-fail-a ~A runs=~D" (trace--event event) runs)
    (values (list :runs runs) (list (publish-state :trace-fail-a :a)) nil)))

(define-extension "trace-fail-b" (:reads (:button :data) :state nil)
    (snapshot state event)
  (declare (ignore snapshot))
  (trace--record "trace-fail-b ~A" (trace--event event))
  (when (getf event :fail)
    (error "trace failure on demand"))
  (values state (list (publish-state :trace-fail-b :ok)) nil))
