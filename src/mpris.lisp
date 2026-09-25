(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_mpris_abi" %mpris-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_mpris_open" %mpris-open) (* t)
  (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_mpris_poll" %mpris-poll) sb-alien:int
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_mpris_snapshot" %mpris-snapshot) sb-alien:c-string
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_mpris_timeout" %mpris-timeout) sb-alien:int
  (producer (* t)) (maximum sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_mpris_close" %mpris-close) sb-alien:void
  (producer (* t)))

(defvar *mpris-support-loaded* nil)

(defun stop-mpris (runtime)
  (let ((native (runtime-mpris-producer runtime)))
    (when native
      (setf (runtime-mpris-producer runtime) nil)
      (%mpris-close native))))

(defun start-mpris (runtime)
  "Start the session observer. An absent bus leaves the default facts."
  (handler-case
      (progn
        (unless *mpris-support-loaded*
          (sb-alien:load-shared-object
           (or (sb-ext:posix-getenv "TOMOE_MPRIS_LIB") "build/libtomoe-mpris.so"))
          (unless (= 1 (%mpris-abi)) (error "Incompatible MPRIS helper ABI."))
          (setf *mpris-support-loaded* t))
        (sb-alien:with-alien ((error-code sb-alien:int))
          (let ((native (%mpris-open (sb-alien:addr error-code))))
            (unless (sb-alien:null-alien native)
              (setf (runtime-mpris-producer runtime) native
                    (getf (runtime-services runtime) :mpris)
                    (read-data (or (%mpris-snapshot native)
                                   (error "Cannot allocate MPRIS snapshot."))))))))
    (serious-condition (condition)
      (stop-mpris runtime)
      (format *error-output* "tomoe: MPRIS unavailable: ~A~%" condition))))

(defun mpris-wait-milliseconds (runtime maximum)
  (let ((native (runtime-mpris-producer runtime)))
    (if native (%mpris-timeout native maximum) maximum)))

(defun poll-mpris-snapshot (runtime)
  "Return changed facts and T, or NIL/NIL when the producer is unchanged."
  (handler-case
      (let* ((native (runtime-mpris-producer runtime))
             (status (%mpris-poll native)))
        (cond ((minusp status)
               (stop-mpris runtime)
               (values (default-mpris-state) t))
              ((plusp status)
               (values (read-data (or (%mpris-snapshot native)
                                     (error "Cannot allocate MPRIS snapshot."))) t))))
    (serious-condition (condition)
      (stop-mpris runtime)
      (format *error-output* "tomoe: MPRIS producer stopped: ~A~%" condition)
      (values (default-mpris-state) t))))

(defun service-mpris (runtime)
  (reconcile-backend-observations runtime)
  (when (and (runtime-running runtime) (not *stop-requested*)
             (runtime-mpris-producer runtime))
    (multiple-value-bind (snapshot changed) (poll-mpris-snapshot runtime)
      (when changed
        (let ((services (copy-data (runtime-services runtime))))
          (setf (getf services :mpris) snapshot)
          (dispatch-event runtime (list :type :services :services services)))))))
