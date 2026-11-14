(in-package #:tomoe)

(defstruct owned-timer owner arguments milliseconds ticks period clock deadline fired)

(defun prepare-timers (runtime mounts)
  "Build a candidate registry without arming, cancelling, or mutating live timers."
  (let ((previous (make-hash-table :test #'equal)) (count 0) (timers nil))
    (dolist (timer (runtime-timers runtime))
      (setf (gethash (list (owned-timer-owner timer)
                          (first (owned-timer-arguments timer))) previous) timer))
    (dolist (mounted mounts)
      (let ((owner (mounted-spec mounted)))
        (dolist (effect (mounted-effects mounted))
          (when (eq (effect-kind effect) :timer)
            (when (> (incf count) 4096) (error "More than 4096 owned timers."))
            (let* ((args (effect-arguments effect))
                   (old (gethash (list owner (first args)) previous))
                   ;; :REFRESH resolves to the display's own period, so a timer
                   ;; that follows the monitor is rebuilt when the rate changes
                   ;; even though its declaration did not.
                   (milliseconds (%timer-milliseconds (second args)))
                   ;; :SECOND and :MINUTE arm on the next wall-clock boundary
                   ;; rather than on whenever the timer was installed.
                   (clock (%timer-clock-step (second args)))
                   (first-ticks (if clock (clock-boundary-ms clock) milliseconds)))
              (push (if (and old (equal args (owned-timer-arguments old))
                            (eql milliseconds (owned-timer-milliseconds old)))
                        old
                        (make-owned-timer
                         :owner owner :arguments args :milliseconds milliseconds
                         :clock clock
                         :ticks (ceiling (* first-ticks internal-time-units-per-second)
                                         1000)
                         :period (when (third args)
                                   (ceiling (* (max 1 milliseconds)
                                               internal-time-units-per-second) 1000))))
                    timers))))))
    (nreverse timers)))

(defun install-timers (runtime timers)
  "Arm prepared resources only after policy/native acceptance. Removal cancels."
  (let ((now (get-internal-real-time)))
    (dolist (timer timers)
      (unless (owned-timer-deadline timer)
        (setf (owned-timer-deadline timer) (+ now (owned-timer-ticks timer))))))
  (setf (runtime-timers runtime) timers))

(defun timer-wait-milliseconds (runtime)
  "How long the backend may sleep before the earliest unfired timer is due."
  (let ((now (get-internal-real-time)) (wait nil))
    (dolist (timer (runtime-timers runtime) wait)
      (unless (owned-timer-fired timer)
        (setf wait (least-wait wait (wait-until now (owned-timer-deadline timer))))))))

(defun service-timers (runtime)
  "Deliver at most 64 due events on the compositor thread, once per source/pass.
Consume before invoking: failure never retries a one-shot or an elapsed tick.
The retained fired record prevents an unchanged one-shot effect from rearming."
  (reconcile-backend-observations runtime)
  (let* ((now (get-internal-real-time)) (delivered 0)
         (stop-at (+ now (ceiling (* 8 internal-time-units-per-second) 1000)))
         (due (stable-sort
               (loop for timer in (runtime-timers runtime)
                     when (and (not (owned-timer-fired timer))
                               (<= (owned-timer-deadline timer) now))
                       collect timer)
               #'< :key #'owned-timer-deadline)))
    (dolist (timer due)
      (reconcile-backend-observations runtime)
      (unless (and (runtime-running runtime) (not *stop-requested*) (< delivered 64)
                   (or (zerop delivered) (< (get-internal-real-time) stop-at)))
        (return))
      (when (and (member timer (runtime-timers runtime) :test #'eq)
                 (not (owned-timer-fired timer))
                 (<= (owned-timer-deadline timer) now))
        (incf delivered)
        (setf (owned-timer-fired timer) t)
        (unwind-protect
             (handler-case
                 (transact runtime (runtime-mounts runtime)
                           (list :type :timer :name (first (owned-timer-arguments timer)))
                           nil (list (spec-name (owned-timer-owner timer))))
               (serious-condition (condition) (record-error runtime condition)))
          (when (and (owned-timer-period timer)
                     (member timer (runtime-timers runtime) :test #'eq))
            ;; A clock timer re-arms on the next boundary, so a display that
            ;; reads the time changes exactly when the time changes instead of
            ;; drifting by however late its last tick ran.
            (let ((ticks (if (owned-timer-clock timer)
                             (ceiling (* (clock-boundary-ms (owned-timer-clock timer))
                                         internal-time-units-per-second)
                                      1000)
                             (owned-timer-period timer))))
              (setf (owned-timer-deadline timer) (+ (get-internal-real-time) ticks)
                    (owned-timer-fired timer) nil))))))))

(defun describe-timers (runtime)
  (loop for timer in (runtime-timers runtime)
        for (name milliseconds repeat) = (owned-timer-arguments timer)
        collect (list :owner (spec-name (owned-timer-owner timer)) :name name
                      :kind (if repeat :interval :once)
                      :milliseconds (or (owned-timer-milliseconds timer) milliseconds)
                      :status (if (owned-timer-fired timer) :fired :pending))))
