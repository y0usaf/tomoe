(in-package #:tomoe-user)

;;; A rule declaration replaced in place, and a rule instance whose apply
;;; reducer therefore sees a deferred delivery: the instance is not re-run in
;;; the round that replaced its declaration, but in the round after it.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "rule-owner" (:reads (:button :data) :state '(:declarations 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((declarations (getf state :declarations)))
    (trace--record "rule-owner ~A declarations=~D" (trace--event event) declarations)
    (values (list :declarations (1+ declarations))
            (list (window-rule :trace
                               :app-id "trace-window"
                               :properties (list (cons :trace declarations))
                               :reads '(:windows)
                               :state (list :seen 0)
                               :apply (lambda (window snapshot state event)
                                        (declare (ignore snapshot))
                                        (trace--record "rule-apply window=~A seen=~D ~A"
                                                       (getf window :id) (getf state :seen)
                                                       (trace--event event))
                                        (values (list :seen (1+ (getf state :seen)))
                                                (list (place (getf window :id) 16 16 320 240))
                                                nil))))
            nil)))
