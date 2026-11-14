(in-package #:tomoe-user)

;;; First owner of the shared key. Mounted before its partner, so its value is
;;; exposed again once the partner is unmounted.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "trace-mount-a" (:reads (:button) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (trace--record "trace-mount-a ~A" (trace--event event))
  (values state (list (publish-state :trace-shared (list :owner "a"))) nil))
