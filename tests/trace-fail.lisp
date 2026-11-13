(in-package #:tomoe-user)

;;; A reducer that fails when the event carries :FAIL. The whole candidate
;;; transaction must be discarded, and the next event must commit normally.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

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
