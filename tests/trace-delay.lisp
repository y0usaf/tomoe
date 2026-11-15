(in-package #:tomoe-user)

;;; One delayed event: the extension asks for an interval once and withdraws it
;;; when it fires, so the transaction that a timer starts is recorded exactly once
;;; and the layout it settles on is the layout the following steps inherit.

(defun trace--record (control &rest arguments)
  (tomoe::%write-append "/run/trace.log"
                        (format nil "~A~%" (apply #'format nil control arguments))))

(defun trace--event (event)
  (let ((keys (getf event :keys)) (id (getf event :id)) (fail (getf event :fail)))
    (concatenate 'string (format nil "~A" (getf event :type))
                 (if keys (format nil " keys=~S" keys) "")
                 (if id (format nil " id=~A" id) "")
                 (if fail (format nil " fail=~A" fail) ""))))

(define-extension "trace-delay" (:reads (:windows :button) :state '(:fired nil))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (fired (or (getf state :fired) (eq (getf event :type) :timer))))
    (trace--record "trace-delay ~A windows=~A fired=~A"
                   (trace--event event) (length windows) fired)
    (values (list :fired fired)
            (append (when (getf (first windows) :id)
                      (let ((window (first windows)))
                        (list (place (getf window :id) (if fired 40 80) 60
                                     (getf window :width) (getf window :height)))))
                    (unless fired (list (interval :trace-delay-tick 150))))
            nil)))
