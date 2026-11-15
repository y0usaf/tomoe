(in-package #:tomoe-user)

;; A tiling row whose commands move the focus. It declares its transactions safe
;; to evaluate ahead of the key that runs them, so the loop may compute the next
;; transition before the key arrives and the native side may apply it at once.

(defun oracle-slot (index)
  (+ 100 (* 340 index)))

(defun oracle-row (windows at)
  "Place every window on its slot and raise the one AT."
  (let ((index 0))
    (append
     (mapcar (lambda (window)
               (prog1 (place (getf window :id) (oracle-slot index) 100 320 240)
                 (incf index)))
             windows)
     (let ((focused (nth (mod at (max 1 (length windows))) windows)))
       (when focused (list (focus (getf focused :id) :raise t)))))))

(defun oracle-bindings ()
  "The keys this policy owns. Pressing one runs a command of this extension."
  (list (bind-key nil "a" "oracle-next")
        (bind-key nil "s" "oracle-back")))

(define-extension "oracle-wm" (:reads (:windows :command) :state '(:at 0) :precompute t)
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (count (max 1 (length windows)))
         (command (and (eq (getf event :type) :command)
                       (equal (getf event :owner) "oracle-wm")
                       (getf event :name)))
         (at (if command (mod (+ (getf state :at) (if (equal command "oracle-back") -1 1)) count)
                 (getf state :at))))
    (values (list :at at)
            (append (list (define-command "oracle-next") (define-command "oracle-back"))
                    (oracle-bindings)
                    (oracle-row windows at))
            nil)))
