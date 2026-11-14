(in-package #:tomoe-user)

(define-extension "memory-handback" (:reads (:command)) (snapshot state event)
  "Cons just over the 4 MB the idle handback waits for on CHURN, then leave the scene
quiet, so a check can watch the handback that follows inside a fixed window. The list is
small enough that collecting it costs no more than an ordinary handback, and the state
counts it, so the conses cannot be optimised out of a command nothing reads."
  (declare (ignore snapshot))
  (let ((churn (and (eq (getf event :type) :command)
                    (equal (getf event :name) "churn"))))
    (values (when churn (list :cells (count-if #'null (make-list 300000))))
            (list (define-command "churn" :description "Cons what the idle handback waits for."))
            nil)))
