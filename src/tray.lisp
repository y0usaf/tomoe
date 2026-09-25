(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_tray_abi" %tray-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_tray_open" %tray-open) (* t)
  (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_tray_poll" %tray-poll) sb-alien:int
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_tray_snapshot" %tray-snapshot) sb-alien:c-string
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_tray_timeout" %tray-timeout) sb-alien:int
  (producer (* t)) (maximum sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_tray_close" %tray-close) sb-alien:void
  (producer (* t)))

(defvar *tray-support-loaded* nil)

(defun stop-tray (runtime)
  (let ((native (runtime-tray-producer runtime)))
    (when native
      (setf (runtime-tray-producer runtime) nil)
      (%tray-close native))))

(defun start-tray (runtime)
  "Acquire the session watcher once. An absent bus or another watcher is normal."
  (handler-case
      (progn
        (unless *tray-support-loaded*
          (sb-alien:load-shared-object
           (or (sb-ext:posix-getenv "TOMOE_TRAY_LIB") "build/libtomoe-tray.so"))
          (unless (= 1 (%tray-abi)) (error "Incompatible tray helper ABI."))
          (setf *tray-support-loaded* t))
        (sb-alien:with-alien ((error-code sb-alien:int))
          (let ((native (%tray-open (sb-alien:addr error-code))))
            (unless (sb-alien:null-alien native)
              (setf (runtime-tray-producer runtime) native
                    (getf (runtime-services runtime) :tray)
                    (read-data (or (%tray-snapshot native)
                                   (error "Cannot allocate tray snapshot."))))))))
    (serious-condition (condition)
      (stop-tray runtime)
      (format *error-output* "tomoe: tray unavailable: ~A~%" condition))))

(defun tray-wait-milliseconds (runtime maximum)
  (let ((native (runtime-tray-producer runtime)))
    (if native (%tray-timeout native maximum) maximum)))

(defun poll-tray-snapshot (runtime)
  "Return changed facts and T, or NIL/NIL when the producer is unchanged."
  (handler-case
      (let* ((native (runtime-tray-producer runtime))
             (status (%tray-poll native)))
        (cond ((minusp status)
               (stop-tray runtime)
               (values (default-tray-state) t))
              ((plusp status)
               (values (read-data (or (%tray-snapshot native)
                                     (error "Cannot allocate tray snapshot."))) t))))
    (serious-condition (condition)
      (stop-tray runtime)
      (format *error-output* "tomoe: tray producer stopped: ~A~%" condition)
      (values (default-tray-state) t))))

(defun service-tray (runtime)
  (reconcile-backend-observations runtime)
  (when (and (runtime-running runtime) (not *stop-requested*)
             (runtime-tray-producer runtime))
    (multiple-value-bind (snapshot changed) (poll-tray-snapshot runtime)
      (when changed
        (let ((services (copy-data (runtime-services runtime))))
          (setf (getf services :tray) snapshot)
          (dispatch-event runtime (list :type :services :services services)))))))
