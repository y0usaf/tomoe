(in-package #:tomoe-user)

(defparameter +power-commands+
  (list (define-command "power" :description "Blank or restore every output.")))

(define-extension "power-check" (:reads (:command) :state nil) (snapshot state event)
  (declare (ignore snapshot state))
  (values nil +power-commands+
          (when (and (eq (getf event :type) :command) (equal (getf event :name) "power"))
            (list (output-power :toggle)))))
