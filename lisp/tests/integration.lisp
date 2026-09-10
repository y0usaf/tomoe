;;; End-to-end check for the packaged compositor.
;;;
;;; Starts $TOMOE_LISP_BIN on the headless backend, mounts the fixture policies
;;; through the control protocol, runs the compiled Wayland client against it,
;;; and asserts what the compositor reports. The control client here speaks the
;;; wire format itself with sb-bsd-sockets; it never loads compositor internals.
;;; run-integration.sh prepares XDG_RUNTIME_DIR and TOMOE_TEST_CLIENT.
(require :sb-posix)
(require :sb-bsd-sockets)

(defpackage #:tomoe-integration
  (:use #:cl)
  (:export #:main))

(in-package #:tomoe-integration)

(defparameter *tests-directory* (make-pathname :name nil :type nil :defaults *load-truename*))
(defparameter *policy-directory* (merge-pathnames "policy/" *tests-directory*))
(defparameter *binary* (sb-ext:posix-getenv "TOMOE_LISP_BIN"))
(defparameter *client-binary* (sb-ext:posix-getenv "TOMOE_TEST_CLIENT"))
(defparameter *runtime-directory* (sb-ext:posix-getenv "XDG_RUNTIME_DIR"))
(defparameter *socket-name* (format nil "tomoe-test-~D" (sb-posix:getpid)))
(defparameter *control-path*
  (and *runtime-directory*
       (format nil "~A/~A.ctl" (string-right-trim "/" *runtime-directory*) *socket-name*)))
(defparameter *wayland-socket-path*
  (and *runtime-directory*
       (format nil "~A/~A" (string-right-trim "/" *runtime-directory*) *socket-name*)))
(defparameter *log-directory*
  (and *runtime-directory*
       (merge-pathnames (format nil "~A.logs/" *socket-name*)
                        (pathname (concatenate 'string (string-right-trim "/" *runtime-directory*) "/")))))

;; Passed through to children; everything else is replaced so a run cannot
;; depend on the caller's session.
(defparameter *inherited-environment* '("PATH" "HOME" "LD_LIBRARY_PATH" "TOMOE_LISP_BACKEND"
                                        "TOMOE_LISP_BUILTINS"))

(defvar *compositor* nil)
(defvar *clients* nil)
(defvar *logs* nil)
(defvar *passed* 0)
(defvar *failed* 0)

(defun report (label value &optional detail)
  (if value
      (progn (incf *passed*) (format t "ok   ~A~%" label))
      (progn (incf *failed*) (format t "FAIL ~A~@[ — ~A~]~%" label detail)))
  (finish-output)
  (not (null value)))

(defun check (label value &optional detail)
  (report label (not (null value)) (or detail (format nil "got ~S" value))))

(defun check-equal (label expected actual &key (test #'equal))
  (let ((matches (funcall test expected actual)))
    (report label matches (unless matches (format nil "expected ~S, got ~S" expected actual)))))

(defun child-environment (&rest overrides)
  (let ((environment (loop for key in *inherited-environment*
                           for value = (sb-ext:posix-getenv key)
                           when value collect (format nil "~A=~A" key value))))
    (dolist (override overrides environment)
      (let ((key (car override)))
        (setf environment
              (cons (format nil "~A=~A" key (cdr override))
                    (remove key environment
                            :key (lambda (entry) (subseq entry 0 (position #\= entry)))
                            :test #'equal)))))))

(defun start-process (program arguments environment label)
  (let* ((log (merge-pathnames (format nil "~A.log" label) *log-directory*))
         (stream (open log :direction :output :if-exists :supersede :if-does-not-exist :create)))
    (unwind-protect
         (prog1 (sb-ext:run-program program arguments :wait nil :search nil :input nil
                                    :output stream :error stream :environment environment)
           (push (cons label log) *logs*))
      (close stream))))

(defun stop-process (process)
  (when (eq (sb-ext:process-status process) :running)
    (ignore-errors (sb-ext:process-kill process sb-posix:sigterm)))
  (loop repeat 100 while (eq (sb-ext:process-status process) :running) do (sleep 0.05))
  (when (eq (sb-ext:process-status process) :running)
    (ignore-errors (sb-ext:process-kill process sb-posix:sigkill))
    (ignore-errors (sb-ext:process-wait process)))
  (ignore-errors (sb-ext:process-close process)))

(defun stop-children ()
  (dolist (process (cons *compositor* *clients*))
    (when process (stop-process process))))

(defun read-log (path)
  (when (and path (probe-file path))
    (with-open-file (stream path :if-does-not-exist nil)
      (when stream
        (let ((text (make-string (file-length stream))))
          (subseq text 0 (read-sequence text stream)))))))

(defun log-contains (label text)
  (let ((entry (assoc label *logs* :test #'equal)))
    (and entry (let ((content (read-log (cdr entry))))
                 (and content (search text content))))))

(defun dump-logs ()
  (dolist (entry (reverse *logs*))
    (format t "--- ~A (~A)~%~A~%" (car entry) (cdr entry) (or (read-log (cdr entry)) "(no output)")))
  (finish-output))

(defun check-compositor ()
  (when (and *compositor* (member (sb-ext:process-status *compositor*) '(:exited :signaled)))
    (error "The compositor exited with status ~S." (sb-ext:process-exit-code *compositor*))))

;;; Control protocol: a one-request connection per call, framed as
;;; "<character count>\n<characters>".

(defun send-frame (stream form)
  (let* ((*package* (find-package :keyword))
         (*print-pretty* nil) (*print-readably* nil) (*print-escape* t) (*print-base* 10)
         (*print-radix* nil) (*print-circle* nil) (*print-case* :upcase)
         (text (write-to-string form)))
    (format stream "~D~%~A" (length text) text)
    (finish-output stream)))

(defun receive-frame (stream)
  (let ((digits (loop for character = (read-char stream nil nil)
                      until (or (null character) (char= character #\Newline))
                      collect character into collected
                      finally (return (coerce collected 'string)))))
    (let ((length (parse-integer digits :junk-allowed t)))
      (unless (and length (<= 1 length 1048576)) (error "Bad control frame length ~S." digits))
      (let ((text (make-string length)))
        (unless (= (read-sequence text stream) length) (error "Truncated control frame."))
        (let ((*read-eval* nil) (*package* (find-package :cl-user)))
          (read-from-string text))))))

(defun control-request (request)
  "Send one control request and return its payload, or signal on an error reply."
  (check-compositor)
  (let ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream)))
    (unwind-protect
         (progn
           (sb-bsd-sockets:socket-connect socket *control-path*)
           (let ((stream (sb-bsd-sockets:socket-make-stream
                          socket :input t :output t :element-type 'character :external-format :utf-8)))
             (send-frame stream request)
             (destructuring-bind (version status result) (receive-frame stream)
               (unless (eql 1 version) (error "The compositor speaks wire version ~S." version))
               (ecase status
                 (:ok result)
                 (:error (error "The compositor refused ~S: ~A" (second request) result))))))
      (sb-bsd-sockets:socket-close socket :abort t))))

(defun inspect-state ()
  (control-request '(1 :inspect)))

(defun wait-for-control (timeout)
  "Poll the control socket until the compositor answers, then return that state."
  (let ((deadline (+ (get-internal-real-time) (round (* timeout internal-time-units-per-second))))
        (failure nil))
    (loop
      (check-compositor)
      (handler-case (return (inspect-state))
        (serious-condition (condition) (setf failure condition)))
      (when (> (get-internal-real-time) deadline)
        (error "The control socket did not answer within ~D seconds: ~A" timeout failure))
      (sleep 0.05))))

(defun wait-until (label timeout predicate)
  "Poll the compositor until PREDICATE holds, or fail with the last state.
Returns the state snapshot that satisfied PREDICATE."
  (let ((deadline (+ (get-internal-real-time) (round (* timeout internal-time-units-per-second))))
        (state nil))
    (loop
      (setf state (inspect-state))
      (when (funcall predicate state) (return state))
      (when (> (get-internal-real-time) deadline)
        (error "~A did not happen within ~D seconds; last state ~S" label timeout (state-summary state)))
      (sleep 0.05))))

(defun state-summary (state)
  (list :last-error (getf state :last-error)
        :windows (mapcar (lambda (window)
                           (list (getf window :id) (getf window :app-id)
                                 (getf window :width) (getf window :height)))
                         (getf state :windows))
        :layers (mapcar (lambda (layer)
                          (list (getf layer :id) (getf layer :namespace) (getf layer :layer)
                                (getf layer :exclusive-zone) (getf layer :keyboard) (getf layer :visible)))
                        (getf state :layers))))

(defun window-by-app-id (state app-id)
  (find app-id (getf state :windows) :key (lambda (window) (getf window :app-id)) :test #'equal))

(defun layer-by-namespace (state namespace)
  (find namespace (getf state :layers) :key (lambda (layer) (getf layer :namespace)) :test #'equal))

(defun extension-state (state name)
  (getf (find name (getf state :extensions)
              :key (lambda (extension) (getf extension :name)) :test #'equal)
        :state))

(defun layout-entry (state id)
  (find id (getf state :layout) :key (lambda (window) (getf window :id))))

(defun extension-names (state)
  (sort (mapcar (lambda (extension) (getf extension :name)) (getf state :extensions)) #'string<))

(defun anchors (layer)
  (sort (mapcar #'symbol-name (getf layer :anchors)) #'string<))

(defun layout-offset (state id)
  "Where the layout puts window ID, relative to the first output."
  (let ((output (first (getf state :outputs)))
        (entry (layout-entry state id)))
    (when (and output entry)
      (list (- (getf entry :x) (getf output :x)) (- (getf entry :y) (getf output :y))))))

(defun inside-output-p (state id)
  (let ((output (first (getf state :outputs)))
        (entry (layout-entry state id)))
    (and output entry
         (>= (getf entry :x) (getf output :x))
         (>= (getf entry :y) (getf output :y))
         (<= (+ (getf entry :x) (getf entry :width)) (+ (getf output :x) (getf output :width)))
         (<= (+ (getf entry :y) (getf entry :height)) (+ (getf output :y) (getf output :height))))))

(defun start-client (label arguments)
  (let ((process (start-process *client-binary* arguments
                                (child-environment (cons "XDG_RUNTIME_DIR" *runtime-directory*)
                                                   (cons "WAYLAND_DISPLAY" *socket-name*))
                                label)))
    (push process *clients*)
    process))

(defun wait-for-exit (process timeout)
  (let ((deadline (+ (get-internal-real-time) (round (* timeout internal-time-units-per-second)))))
    (loop while (eq (sb-ext:process-status process) :running)
          do (when (> (get-internal-real-time) deadline)
               (error "The compositor did not exit within ~D seconds." timeout))
             (sleep 0.05))
    (sb-ext:process-exit-code process)))

(defun require-environment ()
  (unless *binary* (error "TOMOE_LISP_BIN is unset; run tests/run-integration.sh."))
  (setf *binary* (namestring (truename *binary*)))
  (unless *client-binary* (error "TOMOE_TEST_CLIENT is unset; run tests/run-integration.sh."))
  (setf *client-binary* (namestring (truename *client-binary*)))
  (unless *runtime-directory* (error "XDG_RUNTIME_DIR is unset; run tests/run-integration.sh."))
  (let ((policies (directory (merge-pathnames "*.lisp" *policy-directory*))))
    (unless (>= (length policies) 2) (error "No fixture policies under ~A." *policy-directory*))
    (ensure-directories-exist *log-directory*)))

(defun run-check ()
  (let ((layout-policy (namestring (merge-pathnames "layout.lisp" *policy-directory*)))
        (overlay-policy (namestring (merge-pathnames "overlay.lisp" *policy-directory*)))
        (state nil))
    ;; 1. Start the compositor with no policy and wait for its control socket.
    (setf *compositor*
          (start-process *binary*
                         (list "--socket" *socket-name* "--backend" "headless" "--bare" "--no-watch")
                         (child-environment (cons "XDG_RUNTIME_DIR" *runtime-directory*)
                                            (cons "WAYLAND_DISPLAY" *socket-name*)
                                            (cons "WLR_RENDERER" "pixman"))
                         "compositor"))
    (setf state (wait-for-control 30))
    (check "the compositor answers on its control socket" (getf state :socket))
    (check "the compositor is watching nothing with --no-watch" (null (getf state :watch)))
    (check "the run starts without a runtime error" (null (getf state :last-error))
           (format nil "last-error: ~S" (getf state :last-error)))
    (check "no policy is mounted by --bare" (null (getf state :extensions)))
    (check "the compositor reports at least one output" (plusp (length (getf state :outputs))))

    ;; 2. Mount the fixture policies.
    (control-request (list 1 :mount layout-policy))
    (control-request (list 1 :mount overlay-policy))
    (setf state (wait-until "the fixture policies to mount" 15
                            (lambda (s) (equal '("test-layout" "test-overlay") (extension-names s)))))
    (check-equal "mounting the fixtures" '("test-layout" "test-overlay") (extension-names state))
    (check "mounting raises no runtime error" (null (getf state :last-error))
           (format nil "last-error: ~S" (getf state :last-error)))
    ;; Extension state is data the control client has to read back; a cons is
    ;; the shape the shipped layouts use for their own bookkeeping.
    (check-equal "extension state containing a cons survives inspect"
                 '((:enabled . t))
                 (extension-state state "test-overlay"))

    ;; 3. Two xdg clients map at their requested size; the fixture layout places
    ;;    both of them at its fixed offset inside the output.
    (start-client "client-one" (list "--mode" "xdg" "--app-id" "tomoe-test-one" "--title" "Tomoe one"
                                     "--size" "320x240" "--color" "cc4444" "--seconds" "60"))
    (setf state (wait-until "the first window to map" 30
                            (lambda (s) (window-by-app-id s "tomoe-test-one"))))
    (check "the first client mapped at its requested size"
           (log-contains "client-one" (format nil "configured 320x240~%mapped")))
    (let ((id (getf (window-by-app-id state "tomoe-test-one") :id)))
      (setf state (wait-until "the layout to place the first window" 15
                              (lambda (s) (equal '(20 30) (layout-offset s id)))))
      (check-equal "the layout places the first window at the fixture offset" '(20 30) (layout-offset state id))
      (check "the layout keeps the first window inside the output" (inside-output-p state id)))

    (start-client "client-two" (list "--mode" "xdg" "--app-id" "tomoe-test-two" "--title" "Tomoe two"
                                     "--size" "200x160" "--color" "4444cc" "--seconds" "60"))
    (setf state (wait-until "the second window to map" 30
                            (lambda (s) (window-by-app-id s "tomoe-test-two"))))
    (check "the second client mapped at its requested size"
           (log-contains "client-two" (format nil "configured 200x160~%mapped")))
    (let ((id (getf (window-by-app-id state "tomoe-test-two") :id)))
      (setf state (wait-until "the layout to place the second window" 15
                              (lambda (s) (equal '(60 70) (layout-offset s id)))))
      (check-equal "the layout places the second window at the fixture offset" '(60 70) (layout-offset state id))
      (check "the layout keeps the second window inside the output" (inside-output-p state id)))

    (let ((first (getf (window-by-app-id state "tomoe-test-one") :id))
          (second (getf (window-by-app-id state "tomoe-test-two") :id)))
      ;; 4. A layer client appears with its own request overridden by the fixture:
      ;;    the resolved layer, zone, keyboard and visibility are the override's.
      (start-client "client-layer" (list "--mode" "layer" "--namespace" "tomoe-test" "--layer" "top"
                                         "--anchor" "top,left,right" "--exclusive-zone" "64"
                                         "--keyboard" "none" "--size" "200x40" "--color" "44cc44"
                                         "--seconds" "60"))
      (setf state (wait-until "the layer surface to appear" 30
                              (lambda (s) (layer-by-namespace s "tomoe-test"))))
      (check "the layer client mapped at its requested size"
             (log-contains "client-layer" (format nil "configured 200x40~%mapped")))
      (let ((layer (layer-by-namespace state "tomoe-test")))
        (check-equal "the layer namespace is the requested one" "tomoe-test" (getf layer :namespace))
        (check-equal "the layer anchors are the requested ones" '("LEFT" "RIGHT" "TOP") (anchors layer))
        (check-equal "the layer keeps its requested height" 40 (getf layer :height))
        (check-equal "the fixture override resolves the layer" :overlay (getf layer :layer))
        (check-equal "the fixture override resolves the exclusive zone" 48 (getf layer :exclusive-zone))
        (check-equal "the fixture override resolves the keyboard interactivity" :exclusive (getf layer :keyboard))
        (check-equal "the fixture override hides the layer" nil (getf layer :visible)))

      ;; 5. An injected key command flips the override off, so the client's own
      ;;    request resolves again.
      (control-request (list 1 :event (format nil "(:type :key :owner ~S :command ~S)"
                                              "test-overlay" "overlay-off")))
      (setf state (wait-until "the override to clear" 15
                              (lambda (s) (let ((layer (layer-by-namespace s "tomoe-test")))
                                            (and layer (eq :top (getf layer :layer)))))))
      (let ((layer (layer-by-namespace state "tomoe-test")))
        (check-equal "the injected command restores the client's layer" :top (getf layer :layer))
        (check-equal "the injected command restores the exclusive zone" 64 (getf layer :exclusive-zone))
        (check-equal "the injected command restores the keyboard interactivity" :none (getf layer :keyboard))
        (check-equal "the injected command shows the layer again" t (getf layer :visible))
        (check-equal "the injected command reaches the owning extension"
                     '((:enabled)) (extension-state state "test-overlay")))

      ;; 6. Unmounting the layout fixture returns both windows to their mapped
      ;;    size at the origin.
      (control-request (list 1 :unmount "test-layout"))
      (setf state (wait-until "the layout to revert" 15
                              (lambda (s)
                                (every (lambda (id)
                                         (let ((entry (layout-entry s id)))
                                           (and entry (eql 0 (getf entry :x)) (eql 0 (getf entry :y))
                                                (eql t (getf entry :visible)))))
                                       (list first second)))))
      (check-equal "unmounting leaves only the overlay fixture" '("test-overlay") (extension-names state))
      (dolist (pair (list (cons "the first" first) (cons "the second" second)))
        (let* ((id (cdr pair))
               (entry (layout-entry state id))
               (window (find id (getf state :windows) :key (lambda (w) (getf w :id)))))
          (check-equal (format nil "~A window returns to the origin" (car pair))
                       '(0 0) (list (getf entry :x) (getf entry :y)))
          (check-equal (format nil "~A window returns to its mapped size" (car pair))
                       (list (getf window :width) (getf window :height))
                       (list (getf entry :width) (getf entry :height)))
          (check-equal (format nil "~A window is visible again" (car pair)) t (getf entry :visible))))

      ;; 7. The whole run stayed clean, and quit removes the sockets.
      (check "the run ended without a runtime error" (null (getf state :last-error))
             (format nil "last-error: ~S" (getf state :last-error)))
      (check "the control socket exists before quitting" (probe-file *control-path*))
      (check "the wayland socket exists before quitting" (probe-file *wayland-socket-path*))
      (control-request '(1 :quit))
      (check-equal "quit exits with status zero" 0 (wait-for-exit *compositor* 15))
      (check "quit removes the control socket" (null (probe-file *control-path*)))
      (check "quit removes the wayland socket" (null (probe-file *wayland-socket-path*))))))

(defun main ()
  (handler-case (require-environment)
    (serious-condition (condition)
      (report (format nil "the check could not start: ~A" condition) nil)))
  (when (zerop *failed*)
    (handler-case (run-check)
      (serious-condition (condition)
        (report (format nil "the check ran to completion: ~A" condition) nil))))
  (stop-children)
  (when (plusp *failed*) (dump-logs))
  (format t "~D passed, ~D failed~%" *passed* *failed*)
  (finish-output)
  (if (zerop *failed*) 0 1))

(sb-ext:exit :code (main))
