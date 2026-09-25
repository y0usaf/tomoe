(in-package #:tomoe)


(sb-alien:define-alien-routine ("tomoe_watch_abi" %watch-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_watch_open" %watch-open) (* t)
  (path sb-alien:c-string) (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_watch_poll" %watch-poll) sb-alien:int
  (watch (* t)))
(sb-alien:define-alien-routine ("tomoe_watch_read" %watch-read) sb-alien:long
  (watch (* t)) (buffer (* sb-alien:unsigned-char)) (limit sb-alien:unsigned-long))
(sb-alien:define-alien-routine ("tomoe_watch_close" %watch-close) sb-alien:void
  (watch (* t)))

(defvar *watch-support-loaded* nil)

(defconstant +watch-changed+ 1)
(defconstant +watch-overflow+ 2)
(defconstant +watch-parent-lost+ 4)
(defconstant +watch-parent-restored+ 8)
(defconstant +watch-retry-milliseconds+ 250)

(defstruct owned-watch owner arguments path absolute-path content-limit native buffer
  (status :ready) error retry-deadline (active t) (turn 0))

(defstruct watch-plan active retired created (adopted nil))

(defun open-watch-support ()
  "Load the independent helper only when a new native watch is needed."
  (unless *watch-support-loaded*
    (sb-alien:load-shared-object
     (or (sb-ext:posix-getenv "TOMOE_WATCH_LIB")
         "build/libtomoe-watches.so"))
    (unless (= 1 (%watch-abi))
      (error "Incompatible file-watch helper ABI."))
    (setf *watch-support-loaded* t)))

(defun %watch-error-text (condition)
  (let* ((text (princ-to-string condition))
         (length (length text)))
    (if (> length 1024) (subseq text 0 1024) text)))

(defun %watch-retry-ticks ()
  (ceiling (* +watch-retry-milliseconds+ internal-time-units-per-second) 1000))

(defun %watch-close-resource (watch)
  (let ((native (owned-watch-native watch)))
    (when native
      (%watch-close native)
      (setf (owned-watch-native watch) nil)))
  (setf (owned-watch-buffer watch) nil
        (owned-watch-active watch) nil
        (owned-watch-retry-deadline watch) nil)
  watch)

(defun %watch-absolute-path (owner declared-path)
  (if (and (plusp (length declared-path))
           (char= (char declared-path 0) #\/))
      (copy-seq declared-path)
      (concatenate 'string (directory-namestring (spec-source owner))
                   declared-path)))

(defun %watch-open-path (path)
  (open-watch-support)
  (sb-alien:with-alien ((error-code sb-alien:int))
    (let ((native (%watch-open path (sb-alien:addr error-code))))
      (if (sb-alien:null-alien native)
          (error "Cannot open file watch ~A: errno ~D." path error-code)
          native))))

(defun %make-owned-watch (owner arguments)
  (destructuring-bind (name declared-path content-limit) arguments
    (declare (ignore name))
    (let ((buffer (make-array (1+ content-limit)
                              :element-type '(unsigned-byte 8)))
          (native nil)
          (watch nil))
      (unwind-protect
           (progn
             (setf native (%watch-open-path
                           (%watch-absolute-path owner declared-path)))
             (setf watch
                   (make-owned-watch
                    :owner owner :arguments (copy-data arguments)
                    :path (copy-seq declared-path)
                    :absolute-path (%watch-absolute-path owner declared-path)
                    :content-limit content-limit :native native :buffer buffer))
             (setf native nil)
             watch)
        (when native (%watch-close native))))))

(defun %watch-same-p (watch owner arguments)
  (and (eq owner (owned-watch-owner watch))
       (equal arguments (owned-watch-arguments watch))))

(defun prepare-watches (runtime mounts)
  "Build a watch candidate without changing the accepted registry."
  (let ((previous (make-hash-table :test #'equal))
        (keys (make-hash-table :test #'equal))
        (active nil) (created nil) (count 0) (success nil))
    (dolist (watch (runtime-watches runtime))
      (setf (gethash (list (owned-watch-owner watch)
                          (first (owned-watch-arguments watch))) previous)
            watch))
    (dolist (mounted mounts)
      (dolist (effect (mounted-effects mounted))
        (when (eq :watch (effect-kind effect))
          (when (> (incf count) 256)
            (error "More than 256 owned file watches.")))))
    (unwind-protect
         (progn
           (dolist (mounted mounts)
             (let ((owner (mounted-spec mounted)))
               (dolist (effect (mounted-effects mounted))
                 (when (eq :watch (effect-kind effect))
                   (let* ((arguments (effect-arguments effect))
                          (name (first arguments))
                          (key (list owner name))
                          (old (gethash key previous)))
                     (when (gethash key keys)
                       (error "Duplicate file watch: ~S." name))
                     (setf (gethash key keys) t)
                     (if (and old (%watch-same-p old owner arguments))
                         (push old active)
                         (let ((watch (%make-owned-watch owner arguments)))
                           (push watch created)
                           (push watch active))))))))
           (setf active (nreverse active)
                 created (nreverse created))
           (when (> (+ (length (runtime-watches runtime)) (length active)) 512)
             (error "More than 512 live or candidate file watches."))
           (let ((plan
                   (make-watch-plan
                    :active active
                    :retired (remove-if (lambda (watch)
                                          (member watch active :test #'eq))
                                        (runtime-watches runtime))
                    :created created)))
             (setf success t)
             plan))
      (unless success
        (dolist (watch created) (%watch-close-resource watch))))))

(defun install-watches (runtime plan)
  "Publish PLAN and close resources retired by its accepted declaration set."
  (setf (watch-plan-adopted plan) t
        (runtime-watches runtime) (watch-plan-active plan))
  (dolist (watch (watch-plan-retired plan))
    (%watch-close-resource watch))
  (setf (watch-plan-created plan) nil
        (watch-plan-retired plan) nil)
  runtime)

(defun discard-watch-plan (plan)
  "Close only resources opened by an unadopted candidate; safe to repeat."
  (when (and plan (not (watch-plan-adopted plan)))
    (dolist (watch (watch-plan-created plan))
      (%watch-close-resource watch))
    (setf (watch-plan-created plan) nil
          (watch-plan-active plan) nil
          (watch-plan-retired plan) nil))
  nil)

(defun stop-watches (runtime)
  (dolist (watch (runtime-watches runtime))
    (%watch-close-resource watch))
  (setf (runtime-watches runtime) nil)
  runtime)

(defun describe-watches (runtime)
  (loop for watch in (runtime-watches runtime)
        for owner = (owned-watch-owner watch)
        collect (list :owner (copy-seq (spec-name owner))
                      :name (first (owned-watch-arguments watch))
                      :path (copy-seq (owned-watch-path watch))
                      :content-limit (owned-watch-content-limit watch)
                      :status (owned-watch-status watch))))

(defun %watch-read-content (watch)
  (let* ((buffer (owned-watch-buffer watch))
         (limit (owned-watch-content-limit watch))
         (count
           (sb-sys:with-pinned-objects (buffer)
              (%watch-read
              (owned-watch-native watch)
              (sb-alien:sap-alien (sb-sys:vector-sap buffer)
                                  (* sb-alien:unsigned-char))
              limit))))
    (cond
      ((minusp count)
       (values "" :read-error
               (%watch-error-text (format nil "Watch read failed: errno ~D." (- count)))))
      ((> count limit)
       (values "" :read-error
               (%watch-error-text
                (format nil "Watch content exceeds the ~D-byte limit." limit))))
      (t
       (handler-case
           (values (sb-ext:octets-to-string buffer :end count :external-format :utf-8)
                   :ok nil)
         (serious-condition (condition)
           (values "" :read-error (%watch-error-text condition))))))))

(defun %watch-deliver (runtime watch status content error)
  (setf (owned-watch-status watch) status
        (owned-watch-error watch) error)
  (let* ((owner (owned-watch-owner watch))
         (arguments (owned-watch-arguments watch))
         (event (list :type :watch
                      :owner (copy-seq (spec-name owner))
                      :source-id (spec-id owner)
                      :name (first arguments)
                      :path (copy-seq (owned-watch-path watch))
                      :content (copy-seq content)
                      :status status
                      :error (and error (copy-seq error)))))
    (handler-case
        (transact runtime (runtime-mounts runtime) event nil
                  (list (spec-name owner)))
      (serious-condition (condition)
        (record-error runtime condition))))
  runtime)

(defun %watch-service-one (runtime watch now)
  (let ((bits (%watch-poll (owned-watch-native watch))))
    (cond
      ((minusp bits)
       (setf (owned-watch-error watch)
             (%watch-error-text (format nil "Watch poll failed: errno ~D." (- bits)))
             (owned-watch-retry-deadline watch) (+ now (%watch-retry-ticks)))
       (record-error runtime
                     (make-condition 'simple-error
                                     :format-control "File watch poll failed: errno ~D."
                                     :format-arguments (list (- bits)))))
      ((zerop bits)
       (setf (owned-watch-retry-deadline watch)
             (unless (owned-watch-active watch)
               (+ now (%watch-retry-ticks)))))
      ((plusp (logand bits +watch-parent-lost+))
       (setf (owned-watch-active watch) nil
             (owned-watch-retry-deadline watch) (+ now (%watch-retry-ticks)))
       (%watch-deliver runtime watch :unavailable "" nil))
      ((plusp (logand bits +watch-parent-restored+))
       (setf (owned-watch-active watch) t
             (owned-watch-retry-deadline watch) nil)
       (multiple-value-bind (content read-status error) (%watch-read-content watch)
         (%watch-deliver runtime watch (if (eq read-status :ok) :restored :read-error)
                         (if (eq read-status :ok) content "") error)))
      ((plusp (logand bits (logior +watch-overflow+ +watch-changed+)))
       (setf (owned-watch-active watch) t
             (owned-watch-retry-deadline watch) nil)
       (multiple-value-bind (content status error) (%watch-read-content watch)
         (%watch-deliver runtime watch
                         (if (eq status :ok)
                             (if (plusp (logand bits +watch-overflow+)) :overflow :changed)
                             :read-error)
                         (if (eq status :ok) content "") error))))))

(defun service-watches (runtime)
  "Poll a fair bounded snapshot of accepted watches on the compositor thread."
  (reconcile-backend-observations runtime)
  (let* ((now (get-internal-real-time))
         (stop-at (+ now (ceiling (* 8 internal-time-units-per-second) 1000)))
         (serviced 0)
         (snapshot
           (stable-sort (copy-list (runtime-watches runtime)) #'<
                        :key #'owned-watch-turn)))
    (dolist (watch snapshot)
      (reconcile-backend-observations runtime)
      (unless (and (runtime-running runtime) (not *stop-requested*) (< serviced 64)
                   (or (zerop serviced) (< (get-internal-real-time) stop-at)))
        (return))
      (when (and (member watch (runtime-watches runtime) :test #'eq)
                 (or (null (owned-watch-retry-deadline watch))
                     (<= (owned-watch-retry-deadline watch) now)))
        (incf serviced)
        (setf (owned-watch-turn watch) (incf (runtime-watch-turn runtime)))
        (%watch-service-one runtime watch now)))
    runtime))
