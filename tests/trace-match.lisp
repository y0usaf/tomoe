(in-package #:tomoe-user)

;;; A window rule whose :MATCH predicate counts its own calls in a closure: the
;;; engine may not assume a predicate is pure, so it has to call it on every
;;; reconciliation pass it runs. trace.py counts the calls each step makes.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(let ((calls 0))
  (defun trace-match--call () (setf calls (1+ calls))))

(define-extension "match-owner" (:reads (:button) :state '(:presses 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((presses (+ (getf state :presses) (if (eq (getf event :type) :button) 1 0))))
    (trace--record "match-owner ~A presses=~D" (getf event :type) presses)
    (values (list :presses presses)
            (list (window-rule :stateful
                               :app-id "trace-window"
                               :match (lambda (window snapshot)
                                        (declare (ignore snapshot))
                                        (trace--record "match call=~D window=~A"
                                                       (trace-match--call) (getf window :id))
                                        t)
                               :properties '((:trace-match . t))))
            nil)))
