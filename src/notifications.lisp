(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_notifications_abi" %notifications-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_notifications_open" %notifications-open) (* t)
  (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_notifications_poll" %notifications-poll) sb-alien:int
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_notifications_snapshot" %notifications-snapshot) sb-alien:c-string
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_notifications_timeout" %notifications-timeout) sb-alien:int
  (producer (* t)) (maximum sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_notifications_close" %notifications-close) sb-alien:void
  (producer (* t)))

(defvar *notification-support-loaded* nil)

(defun stop-notifications (runtime)
  (let ((native (runtime-notification-producer runtime)))
    (when native
      (setf (runtime-notification-producer runtime) nil)
      (%notifications-close native))))

(defun start-notifications (runtime)
  "Acquire the session service once. An absent bus or another daemon is normal."
  (handler-case
      (progn
        (unless *notification-support-loaded*
          (sb-alien:load-shared-object
           (or (sb-ext:posix-getenv "TOMOE_NOTIFICATION_LIB")
               "build/libtomoe-notifications.so"))
          (unless (= 1 (%notifications-abi))
            (error "Incompatible notification helper ABI."))
          (setf *notification-support-loaded* t))
        (sb-alien:with-alien ((error-code sb-alien:int))
          (let ((native (%notifications-open (sb-alien:addr error-code))))
            (unless (sb-alien:null-alien native)
              (setf (runtime-notification-producer runtime) native
                    (getf (runtime-services runtime) :notifications)
                    (read-data (%notifications-snapshot native)))))))
    (serious-condition (condition)
      (stop-notifications runtime)
      (format *error-output* "tomoe: notifications unavailable: ~A~%" condition))))

(defun notification-wait-milliseconds (runtime maximum)
  (let ((native (runtime-notification-producer runtime)))
    (if native (%notifications-timeout native maximum) maximum)))

(defun poll-notification-snapshot (runtime)
  "Return changed facts and T, or NIL/NIL when the producer is unchanged."
  (handler-case
      (let* ((native (runtime-notification-producer runtime))
             (status (%notifications-poll native)))
        (cond ((minusp status)
               (stop-notifications runtime)
               (values (list :available nil :notifications nil) t))
              ((plusp status)
               (values (read-data (or (%notifications-snapshot native)
                                     (error "Cannot allocate notification snapshot."))) t))))
    (serious-condition (condition)
      (stop-notifications runtime)
      (format *error-output* "tomoe: notification producer stopped: ~A~%" condition)
      (values (list :available nil :notifications nil) t))))

(defun service-notifications (runtime)
  (reconcile-backend-observations runtime)
  (when (and (runtime-running runtime) (not *stop-requested*)
             (runtime-notification-producer runtime))
    (multiple-value-bind (snapshot changed) (poll-notification-snapshot runtime)
      (when changed
        (let ((services (copy-data (runtime-services runtime))))
          (setf (getf services :notifications) snapshot)
          (dispatch-event runtime (list :type :services :services services)))))))
