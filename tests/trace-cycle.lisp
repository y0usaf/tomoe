(in-package #:tomoe-user)

;;; Two reducers that keep replacing each other's published value once the event
;;; asks them to couple. The candidate transaction can never settle, so the engine
;;; must give up after its bounded number of rounds, leave the committed context
;;; untouched, and stay usable.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "trace-cycle-a" (:reads (:button :data) :state nil)
    (snapshot state event)
  (let* ((coupled (or (eq (state-value snapshot :trace-cycle-coupling) t)
                      (and (getf event :cycle) t)))
         (b (state-value snapshot :trace-cycle-b 0)))
    (trace--record "trace-cycle-a ~A coupled=~A b=~A" (trace--event event) coupled b)
    (values (list :coupled coupled)
            (when coupled
              (list (publish-state :trace-cycle-coupling t)
                    (publish-state :trace-cycle-a (1+ b))))
            nil)))

(define-extension "trace-cycle-b" (:reads (:data) :state nil)
    (snapshot state event)
  (let* ((coupled (eq (state-value snapshot :trace-cycle-coupling) t))
         (a (state-value snapshot :trace-cycle-a 0)))
    (trace--record "trace-cycle-b ~A coupled=~A a=~A" (trace--event event) coupled a)
    (values (list :coupled coupled)
            (when coupled (list (publish-state :trace-cycle-b a)))
            nil)))
