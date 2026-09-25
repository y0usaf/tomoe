(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_exec_abi" %exec-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_exec_supported" %exec-supported) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_exec_start" %exec-start) (* t)
  (command sb-alien:c-string) (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_exec_read" %exec-read) sb-alien:long
  (job (* t)) (which sb-alien:int) (buffer (* sb-alien:unsigned-char)) (size sb-alien:unsigned-long))
(sb-alien:define-alien-routine ("tomoe_exec_poll" %exec-poll) sb-alien:int (job (* t)))
(sb-alien:define-alien-routine ("tomoe_exec_code" %exec-code) sb-alien:int (job (* t)))
(sb-alien:define-alien-routine ("tomoe_exec_stop" %exec-stop) sb-alien:int (job (* t)))
(sb-alien:define-alien-routine ("tomoe_exec_release" %exec-release) sb-alien:int (job (* t)))

(defvar *execution-support-loaded* nil)
(defun open-execution-support ()
  (unless *execution-support-loaded*
    (sb-alien:load-shared-object
     (or (sb-ext:posix-getenv "TOMOE_EXEC_LIB") "build/libtomoe-executions.so"))
    (unless (= 1 (%exec-abi)) (error "Incompatible execution helper ABI."))
    (unless (= 1 (%exec-supported))
      (error "Owned execution requires Linux 6.9 process-group pidfd signals."))
    (setf *execution-support-loaded* t)))

(defstruct process-lease process stopped stop-error)
(defstruct (owned-execution (:include process-lease))
  owner identity arguments stdout stderr (out-count 0) (err-count 0)
  out-eof err-eof deadline (status :pending) reason code error delivered (turn 0))
(defstruct execution-plan active retired)

(defun prepare-executions (runtime mounts)
  "Prepare ownership and bounded capture storage without starting any command."
  (let ((previous (make-hash-table :test #'equal)) (count 0) (active nil))
    (dolist (job (runtime-executions runtime))
      (setf (gethash (list (owned-execution-owner job)
                          (first (owned-execution-arguments job))) previous) job))
    (dolist (mounted mounts)
      (let ((owner (mounted-spec mounted)))
        (dolist (effect (mounted-effects mounted))
          (when (eq (effect-kind effect) :exec)
            (when (> (incf count) 64) (error "More than 64 owned executions."))
            (let* ((args (effect-arguments effect))
                   (old (gethash (list owner (first args)) previous)))
              (push (if (and old (equal args (owned-execution-arguments old))) old
                        (make-owned-execution
                         :owner owner :arguments args
                         :identity (list (spec-source owner) (spec-name owner) (first args))
                         :stdout (make-array (1+ (fourth args)) :element-type '(unsigned-byte 8))
                         :stderr (make-array (1+ (fourth args)) :element-type '(unsigned-byte 8))))
                    active))))))
    (let ((retired (append (runtime-retired-executions runtime)
                           (remove-if (lambda (job)
                                        (or (member job active :test #'eq)
                                            (null (owned-execution-process job))))
                                      (runtime-executions runtime)))))
      (when (> (+ count (length retired)) 128)
        (error "More than 128 active or retiring executions."))
      (when (plusp count) (open-execution-support))
      (make-execution-plan :active (nreverse active) :retired retired))))

(defun execution-error-text (condition)
  (let ((text (princ-to-string condition))) (subseq text 0 (min 1024 (length text)))))

(defun kill-execution (runtime job)
  (when (process-lease-process job)
    (when (process-lease-stopped job) (return-from kill-execution t))
    (let ((result (%exec-stop (process-lease-process job))))
      (cond
        ((= result 1)
         (setf (process-lease-stopped job) t (process-lease-stop-error job) nil)
         t)
        (t
         (unless (eql result (process-lease-stop-error job))
           (setf (process-lease-stop-error job) result)
           (record-error runtime
                         (make-condition 'simple-error :format-control "Execution cancellation failed: errno ~D."
                                         :format-arguments (list (- result)))))
         nil)))))

(defun install-executions (runtime plan)
  "Publish prepared ownership. Starting a command is deferred until service."
  (setf (runtime-executions runtime) (execution-plan-active plan)
        (runtime-retired-executions runtime) (execution-plan-retired plan))
  (dolist (job (runtime-retired-executions runtime))
    (unless (eq (owned-execution-status job) :cancelled)
      (setf (owned-execution-status job) :cancelled)
      (kill-execution runtime job)
      (setf (owned-execution-owner job) nil (owned-execution-arguments job) nil
            (owned-execution-stdout job) nil (owned-execution-stderr job) nil))))

(defun release-execution (job)
  (let ((result (%exec-release (process-lease-process job))))
    (unless (= 1 result) (error "Execution release failed: errno ~D." (- result)))
    (setf (process-lease-process job) nil)))

(defun reap-executions (runtime)
  (setf (runtime-retired-executions runtime)
        (delete-if (lambda (job)
                     (kill-execution runtime job)
                     (let ((result (%exec-poll (owned-execution-process job))))
                       (when (minusp result) (error "Cannot reap retired command: errno ~D." (- result)))
                       (when (and (plusp result) (owned-execution-stopped job))
                         (release-execution job) t)))
                   (runtime-retired-executions runtime))))

(defun start-execution (runtime job)
  (handler-case
      (sb-alien:with-alien ((error-code sb-alien:int))
        (let ((process (%exec-start (second (owned-execution-arguments job))
                                    (sb-alien:addr error-code))))
          (when (sb-alien:null-alien process)
            (error "Cannot start async command: errno ~D." error-code))
          (setf (owned-execution-process job) process
                (owned-execution-status job) :running
                (owned-execution-deadline job)
                (+ (get-internal-real-time)
                   (ceiling (* (third (owned-execution-arguments job)) internal-time-units-per-second)
                            1000)))))
    (serious-condition (condition)
      (setf (owned-execution-error job) (execution-error-text condition))
      (if (owned-execution-process job)
          (progn
            (setf (owned-execution-status job) :terminating (owned-execution-reason job) :io-error)
            (kill-execution runtime job))
          (setf (owned-execution-status job) :spawn-error)))))

(defun drain-execution (job which)
  "Read at most 4096 bytes from one pipe without blocking. Return overflow."
  (loop with budget = 4096 repeat 16 while (plusp budget) do
    (let* ((buffer (if (zerop which) (owned-execution-stdout job) (owned-execution-stderr job)))
           (offset (if (zerop which) (owned-execution-out-count job) (owned-execution-err-count job)))
           (remaining (- (fourth (owned-execution-arguments job))
                         (owned-execution-out-count job) (owned-execution-err-count job)))
           (count (sb-sys:with-pinned-objects (buffer)
                    (%exec-read (owned-execution-process job) which
                                (sb-alien:sap-alien (sb-sys:sap+ (sb-sys:vector-sap buffer) offset)
                                                    (* sb-alien:unsigned-char))
                                (min budget (1+ remaining))))))
      (cond
        ((zerop count)
         (if (zerop which) (setf (owned-execution-out-eof job) t)
             (setf (owned-execution-err-eof job) t))
         (return nil))
        ((plusp count)
         (if (zerop which) (incf (owned-execution-out-count job) (min count remaining))
             (incf (owned-execution-err-count job) (min count remaining)))
         (when (> count remaining) (return t))
         (decf budget count))
        ((or (= (- count) sb-posix:eagain) (= (- count) sb-posix:eintr)) (return nil))
        (t (error "Cannot read async output: errno ~D." (- count)))))))

(defun poll-execution (runtime job)
  (when (eq (owned-execution-status job) :pending)
    (when (find (owned-execution-identity job) (runtime-retired-executions runtime)
                :test #'equal :key #'owned-execution-identity)
      (return-from poll-execution))
    (start-execution runtime job))
  (when (owned-execution-process job)
    (handler-case
        (when (eq (owned-execution-status job) :running)
          (when (or (drain-execution job 0) (drain-execution job 1))
            (setf (owned-execution-reason job) :output-limit
                  (owned-execution-error job) "Combined output exceeded the byte limit."))
          (when (owned-execution-reason job)
            (setf (owned-execution-status job) :terminating)
            (kill-execution runtime job)))
      (serious-condition (condition)
        (setf (owned-execution-status job) :terminating
              (owned-execution-reason job) :io-error
              (owned-execution-error job) (execution-error-text condition))
        (kill-execution runtime job)))
    (when (eq (owned-execution-status job) :terminating) (kill-execution runtime job))
    (let ((status (%exec-poll (owned-execution-process job))))
      (when (minusp status) (error "Cannot reap async command: errno ~D." (- status)))
      (when (and (eq (owned-execution-status job) :running)
                 (not (and (plusp status) (owned-execution-out-eof job) (owned-execution-err-eof job)))
                 (>= (get-internal-real-time) (owned-execution-deadline job)))
        (setf (owned-execution-status job) :terminating
              (owned-execution-reason job) :timeout
              (owned-execution-error job) "Execution exceeded its timeout.")
        (kill-execution runtime job))
      (when (and (plusp status)
                 (or (not (eq (owned-execution-status job) :running))
                     (and (owned-execution-out-eof job) (owned-execution-err-eof job))))
        (setf (owned-execution-status job)
              (or (owned-execution-reason job) (if (= 1 status) :exited :signaled)))
        (setf (owned-execution-code job) (%exec-code (owned-execution-process job)))
        (when (kill-execution runtime job) (release-execution job))))))

(defun deliver-execution (runtime job)
  (let ((event (list :type :exec :name (first (owned-execution-arguments job))
                     :status (owned-execution-status job) :code (owned-execution-code job)
                     :stdout (execution-output-text (owned-execution-stdout job) (owned-execution-out-count job))
                     :stderr (execution-output-text (owned-execution-stderr job) (owned-execution-err-count job))
                     :error (owned-execution-error job)))
        (owner (spec-name (owned-execution-owner job))))
    (setf (owned-execution-delivered job) t
          (owned-execution-stdout job) nil (owned-execution-stderr job) nil)
    (handler-case (transact runtime (runtime-mounts runtime) event nil (list owner))
      (serious-condition (condition) (record-error runtime condition)))))

(defun service-executions (runtime)
  (reconcile-backend-observations runtime)
  (reap-executions runtime)
  (let ((stop-at (+ (get-internal-real-time) (ceiling (* 8 internal-time-units-per-second) 1000)))
        (serviced 0)
        (jobs (stable-sort (remove-if #'owned-execution-delivered (copy-list (runtime-executions runtime)))
                           #'< :key #'owned-execution-turn)))
    (dolist (job jobs)
      (reconcile-backend-observations runtime)
      (unless (and (runtime-running runtime) (not *stop-requested*) (< serviced 64)
                   (or (zerop serviced) (< (get-internal-real-time) stop-at))) (return))
      (when (member job (runtime-executions runtime) :test #'eq)
        (incf serviced)
        (setf (owned-execution-turn job) (incf (runtime-execution-turn runtime)))
        (poll-execution runtime job)
        (when (and (execution-terminal-p job) (null (owned-execution-process job)))
          (deliver-execution runtime job))))))

(defun stop-executions (runtime)
  (install-executions runtime
                      (make-execution-plan :active nil
                                           :retired (append (runtime-retired-executions runtime)
                                                            (remove-if-not #'owned-execution-process
                                                                           (runtime-executions runtime)))))
  (loop repeat 100 while (runtime-retired-executions runtime) do
    (reap-executions runtime)
    (when (runtime-retired-executions runtime) (sleep 0.01)))
  (when (runtime-retired-executions runtime)
    (error "Killed async commands have not yet become reapable.")))

(defun execution-output-text (octets count)
  (let* ((text (if octets
                   (sb-ext:octets-to-string
                    octets :end count :external-format '(:utf-8 :replacement #\REPLACEMENT_CHARACTER))
                   ""))
         (start (position-if-not #'sb-unicode:whitespace-p text))
         (end (position-if-not #'sb-unicode:whitespace-p text :from-end t)))
    (if start (subseq text start (1+ end)) "")))

(defun execution-terminal-p (job)
  (not (member (owned-execution-status job) '(:pending :running :terminating))))

(defun describe-executions (runtime)
  (loop for job in (runtime-executions runtime)
        for (name command timeout output-limit) = (owned-execution-arguments job)
        collect (list :owner (spec-name (owned-execution-owner job)) :name name
                      :status (owned-execution-status job)
                      :delivered (owned-execution-delivered job)
                      :timeout timeout :output-limit output-limit
                      :code (owned-execution-code job))))
