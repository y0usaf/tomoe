(in-package #:tomoe-user)

;;; Second, later owner of the shared key: its mount wins, its unmount exposes
;;; the first owner's value again.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "trace-mount-b" (:reads (:button) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (trace--record "trace-mount-b ~A" (trace--event event))
  (values state
          (list (publish-state :trace-shared (list :owner "b"))
                (interval :trace-mount-tick 600000))
          nil))
