(in-package #:tomoe-user)

;;; Dependency chain: ALPHA publishes, BETA derives from it, GAMMA derives from
;;; BETA. Each reducer appends one line per invocation, so the trace records the
;;; order the transaction engine dispatched them in and how many rounds it took
;;; to settle.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(define-extension "chain-alpha" (:reads (:button :data :windows) :state '(:count 0))
    (snapshot state event)
  (let* ((count (1+ (getf state :count)))
         (commands (when (eq (getf event :type) :button)
                     (list (media-control :play-pause)))))
    (trace--record "chain-alpha ~A count=~D emits=~D"
                   (trace--event event) count (length commands))
    (values (list :count count)
            (list (publish-state :chain-alpha (length (context snapshot :windows))))
            commands)))

(define-extension "chain-beta" (:reads (:data) :state nil) (snapshot state event)
  (let* ((alpha (state-value snapshot :chain-alpha 0))
         (beta (1+ alpha))
         (commands (when (eq (getf event :type) :button)
                     (list (media-control :next)))))
    (trace--record "chain-beta ~A alpha=~D emits=~D"
                   (trace--event event) alpha (length commands))
    (values (list :beta beta) (list (publish-state :chain-beta beta)) commands)))

(define-extension "chain-gamma" (:reads (:data) :state nil) (snapshot state event)
  (let* ((beta (state-value snapshot :chain-beta 0))
         (gamma (* 2 beta)))
    (trace--record "chain-gamma ~A beta=~D" (trace--event event) beta)
    (values (list :gamma gamma) (list (publish-state :chain-gamma gamma)) nil)))

(define-extension "chain-delta" (:reads (:button :data :windows) :state nil :admission t)
    (snapshot state event)
  (declare (ignore state))
  (let* ((windows (context snapshot :windows))
         (ids (mapcar (lambda (window) (getf window :id)) windows)))
    (trace--record "chain-delta ~A windows=~A" (trace--event event) ids)
    (values (list :ids ids)
            (when ids (list (focus (car (last ids)))))
            nil)))
