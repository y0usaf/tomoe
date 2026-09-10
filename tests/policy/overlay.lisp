(in-package #:tomoe-user)

;; Integration fixture: own a layer override for every layer surface until an
;; overlay-off command arrives, so the resolved layer state and the revert are
;; both observable. The state is deliberately a cons so the check also covers
;; control framing of dotted extension state. Not shipped policy.
(define-extension "test-overlay" (:reads (:layers :key) :state (list (cons :enabled t)))
    (snapshot state event)
  (when (eq (getf event :type) :key)
    (cond ((equal (getf event :command) "overlay-off") (setf (cdr (assoc :enabled state)) nil))
          ((equal (getf event :command) "overlay-on") (setf (cdr (assoc :enabled state)) t))))
  (values state
          (when (cdr (assoc :enabled state))
            (loop for entry in (context snapshot :layers)
                  collect (layer (getf entry :id) :layer :overlay :exclusive-zone 48
                                 :keyboard :exclusive :visible nil)))
          nil))
