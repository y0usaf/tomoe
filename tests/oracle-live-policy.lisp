(in-package #:tomoe-user)

;; Two reducers that must never be evaluated ahead of their event: one is not
;; declared at all, and one declares itself but reads the clock, which the
;; purity walker refuses. Both keep the synchronous Lisp path, and while either
;; is mounted no command binding in the session can be precomputed.

(defun oracle-live-slot (index)
  (+ 100 (* 340 index)))

(defun oracle-live-row (windows at)
  (let ((index 0))
    (append
     (mapcar (lambda (window)
               (prog1 (place (getf window :id) (oracle-live-slot index) 100 320 240)
                 (incf index)))
             windows)
     (let ((focused (nth (mod at (max 1 (length windows))) windows)))
       (when focused (list (focus (getf focused :id) :raise t)))))))

(defun oracle-live-bindings ()
  (list (bind-key nil "d" "oracle-live-next")))

(define-extension "oracle-live" (:reads (:windows :command) :state '(:at 0))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (count (max 1 (length windows)))
         (command (and (eq (getf event :type) :command)
                       (equal (getf event :owner) "oracle-live")
                       (getf event :name)))
         (at (if command (mod (+ (getf state :at) 1) count) (getf state :at))))
    (values (list :at at)
            (append (list (define-command "oracle-live-next"))
                    (oracle-live-bindings)
                    (oracle-live-row windows at))
            nil)))

(define-extension "oracle-clock" (:reads (:command) :state '(:ticks 0) :precompute t)
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((command (and (eq (getf event :type) :command)
                      (equal (getf event :owner) "oracle-clock")
                      (getf event :name))))
    (values (list :ticks (1+ (getf state :ticks)))
            (list (define-command "oracle-clock-tick")
                  (bind-key nil "f" "oracle-clock-tick"))
            (when command (list (announce :oracle-clock-tick (clock-text "%S")))))))

(defparameter *oracle-global-hits* 0)

(define-extension "oracle-global" (:reads (:command) :state '(:ticks 0) :precompute t)
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((command (and (eq (getf event :type) :command)
                      (equal (getf event :owner) "oracle-global")
                      (getf event :name))))
    (when command (setf *oracle-global-hits* (1+ *oracle-global-hits*)))
    (values (list :ticks (1+ (getf state :ticks)))
            (list (define-command "oracle-global-tick")
                  (bind-key nil "g" "oracle-global-tick"))
            nil)))

(define-extension "oracle-io" (:reads (:command) :state '(:ticks 0) :precompute t)
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((command (and (eq (getf event :type) :command)
                      (equal (getf event :owner) "oracle-io")
                      (getf event :name))))
    (values (list :ticks (1+ (getf state :ticks)))
            (list (define-command "oracle-io-tick")
                  (bind-key nil "h" "oracle-io-tick"))
            (when command (list (announce :oracle-io-tick (length (directory "/run"))))))))
