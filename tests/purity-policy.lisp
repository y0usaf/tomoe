(in-package #:tomoe)

;; A helper the walker can prove harmless. It lives in a package a livepatch can
;; reach, so a patch that redefines it has to withdraw the proof with it.
(defun purity-ids (windows)
  (mapcar (lambda (window) (getf window :id)) windows))

(in-package #:tomoe-user)

;; Mutating reducers. Each is handed private copies, so none of these writes may
;; reach the committed context, the next transaction or a sibling reducer.
(define-extension "purity-snapshot" (:reads (:windows :command) :state '(:runs 0))
    (snapshot state event)
  (let ((windows (context snapshot :windows))
        (command (and (eq (getf event :type) :command) (getf event :name))))
    (when (equal command "purity-mangle-snapshot")
      (mapc 'nreverse windows)
      (mapc 'rplaca windows (make-list (length windows) :initial-element :mangled)))
    (values (list :runs (1+ (getf state :runs)))
            (list (define-command "purity-mangle-snapshot"))
            nil)))

(define-extension "purity-carrier" (:reads (:command) :state '(:order (1 2 3) :runs 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((command (and (eq (getf event :type) :command) (getf event :name))))
    (when (equal command "purity-mangle-state")
      (setf (getf state :order) (cons 99 (cdr (getf state :order)))))
    (values (list :order (getf state :order) :runs (1+ (getf state :runs)))
            (append (list (define-command "purity-mangle-state")
                          (define-command "purity-mangle-event"))
                    (when (equal command "purity-mangle-state")
                      (list (publish-state :purity-state-order (getf state :order)))))
            nil)))

(define-extension "purity-event" (:reads (:command) :state nil)
    (snapshot state event)
  (declare (ignore snapshot))
  (let ((command (and (eq (getf event :type) :command) (getf event :name))))
    (when (equal command "purity-mangle-event")
      (setf (getf event :owner) "mangled"
            (getf event :name) "mangled"))
    (values nil nil nil)))

(define-extension "purity-indirect" (:reads (:windows :command) :state '(:runs 0))
    (snapshot state event)
  (let ((windows (context snapshot :windows))
        (command (and (eq (getf event :type) :command) (getf event :name)))
        (mapper nil))
    (when (equal command "purity-mangle-indirect")
      (setf mapper (if (zerop (getf state :runs))
                       (lambda (window) (rplaca window :mangled))
                       (lambda (window) window)))
      (mapc mapper windows))
    (values (list :runs (1+ (getf state :runs)))
            (list (define-command "purity-mangle-indirect"))
            nil)))

(define-extension "purity-honest" (:reads (:windows :command) :state nil)
    (snapshot state event)
  (declare (ignore state))
  (values nil
          (append (list (publish-state :purity-order (tomoe::purity-ids (context snapshot :windows))))
                  (when (eq (getf event :type) :command)
                    (list (publish-state :purity-owner (list (getf event :owner)))
                          (publish-state :purity-command (list (getf event :name))))))
          nil))
