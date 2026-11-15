(in-package #:tomoe-user)

;;; A rule declaration replaced in place, and a rule instance whose apply
;;; reducer therefore sees a deferred delivery: the instance is not re-run in
;;; the round that replaced its declaration, but in the round after it.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

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
