(in-package #:tomoe-user)

;;; One delayed event: the extension asks for an interval once and withdraws it
;;; when it fires, so the transaction that a timer starts is recorded exactly once
;;; and the layout it settles on is the layout the following steps inherit.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

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
