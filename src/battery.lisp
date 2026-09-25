(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_battery_abi" %battery-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_battery_open" %battery-open) (* t)
  (sysfs-root sb-alien:c-string) (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_battery_poll" %battery-poll) sb-alien:int
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_battery_snapshot" %battery-snapshot) sb-alien:c-string
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_battery_timeout" %battery-timeout) sb-alien:int
  (producer (* t)) (maximum sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_battery_close" %battery-close) sb-alien:void
  (producer (* t)))

(defvar *battery-support-loaded* nil)

(defun stop-battery (runtime)
  (let ((native (runtime-battery-producer runtime)))
    (when native
      (setf (runtime-battery-producer runtime) nil)
      (%battery-close native))))

(defun start-battery (runtime)
  "Start the session producer. Missing UPower and battery hardware are normal."
  (handler-case
      (progn
        (unless *battery-support-loaded*
          (sb-alien:load-shared-object
           (or (sb-ext:posix-getenv "TOMOE_BATTERY_LIB") "build/libtomoe-battery.so"))
          (unless (= 1 (%battery-abi)) (error "Incompatible battery helper ABI."))
          (setf *battery-support-loaded* t))
        (sb-alien:with-alien ((error-code sb-alien:int))
          (let ((native (%battery-open
                         (or (sb-ext:posix-getenv "TOMOE_POWER_SUPPLY_ROOT")
                             "/sys/class/power_supply")
                         (sb-alien:addr error-code))))
            (unless (sb-alien:null-alien native)
              (setf (runtime-battery-producer runtime) native
                    (getf (runtime-services runtime) :battery)
                    (read-data (or (%battery-snapshot native)
                                   (error "Cannot allocate battery snapshot."))))))))
    (serious-condition (condition)
      (stop-battery runtime)
      (format *error-output* "tomoe: battery unavailable: ~A~%" condition))))

(defun battery-wait-milliseconds (runtime maximum)
  (let ((native (runtime-battery-producer runtime)))
    (if native (%battery-timeout native maximum) maximum)))

(defun poll-battery-snapshot (runtime)
  "Return changed facts and T, or NIL/NIL when the producer is unchanged."
  (handler-case
      (let* ((native (runtime-battery-producer runtime))
             (status (%battery-poll native)))
        (cond ((minusp status)
               (stop-battery runtime)
               (values (default-battery-state) t))
              ((plusp status)
               (values (read-data (or (%battery-snapshot native)
                                     (error "Cannot allocate battery snapshot."))) t))))
    (serious-condition (condition)
      (stop-battery runtime)
      (format *error-output* "tomoe: battery producer stopped: ~A~%" condition)
      (values (default-battery-state) t))))

(defun service-battery (runtime)
  (reconcile-backend-observations runtime)
  (when (and (runtime-running runtime) (not *stop-requested*)
             (runtime-battery-producer runtime))
    (multiple-value-bind (snapshot changed) (poll-battery-snapshot runtime)
      (when changed
        (let ((services (copy-data (runtime-services runtime))))
          (setf (getf services :battery) snapshot)
          (dispatch-event runtime (list :type :services :services services)))))))
