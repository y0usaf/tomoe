(in-package #:tomoe)

(defun drain-backend-events (runtime)
  "Yield between complete policy transactions after 64 events or four ms.
Queued window/layer observations form a FIFO fence: consume that prefix before
private callbacks can use their scopes. Output proposals can wait for the next
turn; materialization already refreshes their authoritative native snapshot."
  (let ((backend (runtime-backend runtime))
        (delivered 0)
        (deadline (+ (get-internal-real-time)
                     (ceiling (* 4 internal-time-units-per-second) 1000))))
    (loop while (and (runtime-running runtime) (not *stop-requested*)) do
      (when (and (plusp delivered)
                 (or (>= delivered 64) (>= (get-internal-real-time) deadline))
                 (zerop (%event-barrier backend)))
        (return))
      (let ((text (%event backend)))
        (unless text (return))
        (dispatch-event runtime (read-data text))
        (incf delivered)))
    delivered))

(defparameter +operations+
  '(("inspect" . :inspect) ("reload" . :reload) ("mount" . :mount)
    ("unmount" . :unmount) ("command" . :command) ("event" . :event)
    ("hit-test" . :hit-test) ("quit" . :quit)))

(defun usage ()
  (write-line "Usage: tomoe [--socket NAME] [--backend auto|nested|headless|drm] [--bare]
       [--config FILE] [--watch|--no-watch] [--drm_device PATH]
       tomoe [--socket NAME] inspect|reload|mount FILE|unmount NAME|command OWNER NAME|event PLIST|hit-test X Y|quit
       tomoe [--socket NAME] msg METHOD [JSON]

Default socket: tomoe-0. Default backend: auto. winit and tty mean nested and drm.
Auto nests in an existing Wayland display, or uses DRM when none is found.
Nested mode discovers live wayland-N sockets when WAYLAND_DISPLAY is unset.
Extensions are trusted Common Lisp programs. --bare omits all shipped policy.
Loads $XDG_CONFIG_HOME/tomoe/init.lisp or ~/.config/tomoe/init.lisp when present.
--config overrides that file; --bare skips it.
Extension sources are watched and reloaded when edited; --no-watch disables that.
event sends one data plist to a live instance as an injected input event.
hit-test reads one screen-space point from a live instance without changing it.
X11 clients connect through DISPLAY; the xwayland extension starts xwayland-satellite on the first connection.
Control replies are versioned Lisp data. Mutating commands are silent on success."))

(defun json-socket-path (name)
  (socket-path name)
  (format nil "~A/tomoe.~A.sock" (runtime-directory) name))

(defun json-client-path (explicit-name)
  (or (and explicit-name (json-socket-path explicit-name))
      (let ((path (sb-ext:posix-getenv "TOMOE_SOCKET")))
        (when (and path (plusp (length path))) path))
      (let ((display (sb-ext:posix-getenv "WAYLAND_DISPLAY")))
        (when (and display (plusp (length display))) (json-socket-path display)))
      (error "Set TOMOE_SOCKET or WAYLAND_DISPLAY, or pass --socket NAME.")))

(defun runtime-directory ()
  (let ((path (sb-ext:posix-getenv "XDG_RUNTIME_DIR")))
    (unless path (error "XDG_RUNTIME_DIR is required."))
    (let ((stat (sb-posix:stat path)))
      (unless (and (sb-posix:s-isdir (sb-posix:stat-mode stat))
                   (= (sb-posix:stat-uid stat) (sb-posix:getuid))
                   (zerop (logand (sb-posix:stat-mode stat) #o077)))
        (error "XDG_RUNTIME_DIR must be an owned directory with mode 0700.")))
    (string-right-trim "/" path)))

(defun socket-path (name)
  (unless (and (<= 1 (length name) 60) (not (member name '("." "..") :test #'equal))
               (every (lambda (c) (find c "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.")) name))
    (error "Invalid socket name: ~S" name))
  (format nil "~A/~A.ctl" (runtime-directory) name))

(defun parent-wayland-display ()
  (let ((display (sb-ext:posix-getenv "WAYLAND_DISPLAY")))
    (or (and display (plusp (length display)) display)
        (loop for path in (sort (directory (format nil "~A/wayland-*" (runtime-directory))
                                           :resolve-symlinks nil)
                                 #'string< :key #'namestring)
              for name = (file-namestring path)
              when (and (> (length name) 8)
                        (every #'digit-char-p (subseq name 8))
                        (socket-answering-p (namestring path)))
                return name))))

(defun watch-sources (runtime stamps)
  "Reload a source once its stamp has held still for two consecutive samples.
The baseline for a source is the stamp recorded when the runtime loaded it, so
an edit made before the watcher's first look is not mistaken for the loaded
content. A source that vanished or cannot be read keeps the mounted policy and
is reported once. Every observed stamp is recorded, so a broken file is retried
only after it changes again."
  (dolist (path (runtime-sources runtime))
    (reconcile-backend-observations runtime)
    (unless (and (runtime-running runtime) (not *stop-requested*)) (return))
    (let ((current (source-stamp path)) (entry (gethash path stamps)))
      (cond
        ((null current)
         (unless (eq (getf entry :loaded) :missing)
           (setf (gethash path stamps) (list :stamp nil :loaded :missing :stable 0))
           (record-error runtime (make-condition 'simple-error
                                                 :format-control "Cannot read extension source ~A."
                                                 :format-arguments (list path)))))
        ((null entry)
         (let ((loaded (cdr (assoc path (runtime-source-stamps runtime) :test #'equal))))
           (setf (gethash path stamps)
                 (list :stamp current :loaded (or loaded current) :stable 0))))
        ((equal current (getf entry :loaded))
         (setf (getf entry :stamp) current (getf entry :stable) 0))
        (t
         (setf (getf entry :stable)
               (if (equal current (getf entry :stamp)) (1+ (getf entry :stable)) 1)
               (getf entry :stamp) current)
         (when (>= (getf entry :stable) 2)
           (setf (getf entry :loaded) current (getf entry :stable) 0)
           (handler-case (configure runtime (runtime-sources runtime) path)
             (serious-condition (condition) (report-config-error runtime condition nil)))))))))

(defun free-x-display ()
  "The first X display name whose lock file is absent or names a dead process."
  (loop for n below 64
        for lock = (format nil "/tmp/.X~D-lock" n)
        for pid = (ignore-errors
                   (with-open-file (in lock) (parse-integer (read-line in) :junk-allowed t)))
        unless (and (probe-file lock) (or (null pid) (probe-file (format nil "/proc/~D" pid))))
          return (format nil ":~D" n)
        finally (error "No free X display below :64.")))

(defun primary-drm-devices (path)
  "TOMOE_DRM_DEVICES naming PATH's card first, then every other card."
  (let* ((name (file-namestring path))
         (card (if (and (>= (length name) 7) (string= "renderD" name :end2 7))
                   (let ((match (first (directory (format nil "/sys/class/drm/~A/device/drm/card*" name)))))
                     (unless match (error "No card node for ~A." path))
                     (format nil "/dev/dri/~A" (car (last (pathname-directory match)))))
                   (namestring (truename path))))
         (others (remove card (mapcar #'namestring (directory "/dev/dri/card*")) :test #'equal)))
    (format nil "~{~A~^:~}" (cons card others))))

(defparameter +session-variables+
  '("WAYLAND_DISPLAY" "DISPLAY" "XDG_CURRENT_DESKTOP" "TOMOE_PORTAL_CHOOSER" "TOMOE_SOCKET")
  "Variables pushed into the systemd user and D-Bus activation environments.")

(defun session-shell (script &optional (wait t))
  (ignore-errors (sb-ext:run-program (or (sb-ext:posix-getenv "TOMOE_SHELL") "/bin/sh") (list "-c" script) :wait wait :output nil :error nil)))

(defun start-session ()
  "Publish the session environment and bring tomoe-session.target up."
  (let ((variables (format nil "~{~A~^ ~}" (remove-if-not #'sb-ext:posix-getenv +session-variables+))))
    (session-shell (format nil "hash systemctl 2>/dev/null && systemctl --user import-environment ~A; hash dbus-update-activation-environment 2>/dev/null && dbus-update-activation-environment ~:*~A; exit 0" variables)))
  (session-shell "hash systemctl 2>/dev/null || exit 0; systemctl --user start tomoe-session.target; timeout 10 systemctl --user start xdg-desktop-portal-gtk.service; systemctl --user try-restart xdg-desktop-portal.service" nil))

(defun stop-session ()
  (session-shell (format nil "hash systemctl 2>/dev/null || exit 0; systemctl --user stop tomoe-session.target; systemctl --user unset-environment ~{~A~^ ~}" +session-variables+)))

(defun run-compositor (name backend sources watch &optional drm-device)
  (when (member backend '("auto" "nested") :test #'equal)
    (let ((display (parent-wayland-display)))
      (cond
        (display
         (sb-posix:setenv "WAYLAND_DISPLAY" display 1)
         (setf backend "nested"))
        ((equal backend "auto") (setf backend "drm"))
        (t (error "No live Wayland display found in ~A. Set WAYLAND_DISPLAY for a custom socket, or use --backend headless or drm."
                  (runtime-directory))))))
  (cond
    ((equal backend "nested") (sb-posix:setenv "TOMOE_BACKEND" "nested" 1))
    ((equal backend "headless") (sb-posix:setenv "TOMOE_BACKEND" "headless" 1))
    ((equal backend "drm") (sb-posix:setenv "TOMOE_BACKEND" "drm" 1))
    (t (error "Unknown backend: ~A" backend)))
  (when drm-device
    (sb-posix:setenv "TOMOE_DRM_DEVICES" (primary-drm-devices drm-device) 1))
  (setf *stop-requested* nil)
  (flet ((stop (signal info context)
           (declare (ignore signal info context)) (setf *stop-requested* t)))
    (sb-sys:enable-interrupt sb-posix:sigterm #'stop)
    (sb-sys:enable-interrupt sb-posix:sigint #'stop))
  (let ((control (open-control (socket-path name))) (json-server nil) (native nil) (runtime nil)
        (stamps (make-hash-table :test #'equal)) (next-watch 0))
    (unwind-protect
         (progn
           (setf native (open-backend name)
                 runtime (make-runtime :backend native :socket name))
           (setf json-server (open-json-control (json-socket-path name))
                 (runtime-json-server runtime) json-server)
           (setf (runtime-watch runtime) watch)
           (sb-posix:setenv "WAYLAND_DISPLAY" name 1)
           (sb-posix:setenv "TOMOE_SOCKET" (json-server-path json-server) 1)
           (sb-posix:setenv "DISPLAY" (free-x-display) 1)
           (sb-posix:setenv "XDG_CURRENT_DESKTOP" "tomoe" 1)
           (when (equal backend "drm") (start-session))
           (start-notifications runtime)
           (start-mpris runtime)
           (start-battery runtime)
           (start-network runtime)
           (start-tray runtime)
           (if (rest sources)
               (handler-case (configure runtime sources)
                 (serious-condition (condition)
                   (configure runtime (butlast sources))
                   (report-config-error runtime condition t)))
               (configure runtime sources))
           (loop while (and (runtime-running runtime) (not *stop-requested*)) do
             (let ((status (%step native (if (plusp (%event-count native)) 0
                                            (tray-wait-milliseconds
                                             runtime (network-wait-milliseconds
                                                      runtime (battery-wait-milliseconds
                                                               runtime (mpris-wait-milliseconds
                                                                        runtime (notification-wait-milliseconds
                                                                                 runtime (timer-wait-milliseconds runtime 8))))))))))
               (when (< status 0) (error "Native event loop failed."))
               (when (> status 0) (return)))
             (drain-backend-events runtime)
             (unless (and (runtime-running runtime) (not *stop-requested*)) (return))
             (service-notifications runtime)
             (service-mpris runtime)
             (service-battery runtime)
             (service-network runtime)
             (service-tray runtime)
             (serve-control runtime control)
             (serve-json-control runtime json-server)
             (service-timers runtime)
             (service-watches runtime)
             (service-executions runtime)
             (service-managed-processes runtime)
             (poll-json-focus runtime)
             (when (and (runtime-watch runtime) (>= (get-internal-real-time) next-watch))
               (setf next-watch (+ (get-internal-real-time)
                                   (floor internal-time-units-per-second 4)))
               (watch-sources runtime stamps)))
           (loop with deadline = (+ (get-internal-real-time) (ceiling internal-time-units-per-second 10))
                 while (and (json-control-pending-p json-server)
                            (< (get-internal-real-time) deadline))
                 do (drain-json-control json-server) (sleep 0.001))
           0)
      (unwind-protect (when runtime
                        (setf (runtime-timers runtime) nil)
                        (unwind-protect (stop-tray runtime)
                          (unwind-protect (stop-network runtime)
                            (unwind-protect (stop-battery runtime)
                              (unwind-protect (stop-mpris runtime)
                                (unwind-protect (stop-notifications runtime)
                                  (unwind-protect (stop-watches runtime)
                                    (unwind-protect (stop-executions runtime)
                                      (stop-managed-processes runtime)))))))))
        (unwind-protect (when (and runtime (equal backend "drm")) (stop-session))
          (unwind-protect (when native (%destroy native))
            (unwind-protect (close-json-control json-server) (close-control control))))))))

(defun default-config-file ()
  (let* ((root (sb-ext:posix-getenv "XDG_CONFIG_HOME"))
         (directory (if (and root (plusp (length root)))
                        (format nil "~A/" (string-right-trim "/" root))
                        (merge-pathnames ".config/" (user-homedir-pathname))))
         (path (merge-pathnames "tomoe/init.lisp" directory)))
    (when (probe-file path) (namestring (truename path)))))

(defun run-cli (arguments)
  (let ((name "tomoe-0") (explicit-name nil) (backend "auto") (bare nil) (config nil) (watch t)
        (drm-device nil))
    (labels ((argument (option)
               (or (pop arguments) (error "~A requires a value." option))))
      (loop while arguments for option = (pop arguments) do
        (cond
          ((member option '("-h" "--help") :test #'equal) (usage) (return-from run-cli 0))
          ((member option '("-V" "--version") :test #'equal)
           (format t "tomoe 0.1.0, JSON wire 2, control wire ~D, native ABI ~D~%"
                   +wire-version+ +native-abi-version+)
           (return-from run-cli 0))
          ((equal option "--socket") (setf name (argument option) explicit-name name))
          ((equal option "--backend")
           (setf backend (let ((value (argument option)))
                           (cond ((equal value "winit") "nested")
                                 ((equal value "tty") "drm")
                                 (t value)))))
          ((equal option "--config") (setf config (namestring (truename (argument option)))))
          ((equal option "--drm_device") (setf drm-device (argument option)))
          ((equal option "--bare") (setf bare t))
          ((equal option "--watch") (setf watch t))
          ((equal option "--no-watch") (setf watch nil))
          ((equal option "msg")
           (when (or config bare (not (equal backend "auto")))
             (error "Server options cannot be combined with msg."))
           (unless (<= 1 (length arguments) 2) (error "msg requires METHOD and at most one JSON argument."))
           (return-from run-cli
             (json-msg-client (json-client-path explicit-name) (first arguments)
                              (when (second arguments) (parse-json (second arguments))))))
          ((assoc option +operations+ :test #'equal)
           (when (or config bare (not (equal backend "auto")))
             (error "Server options cannot be combined with a control command."))
           (let ((operation (cdr (assoc option +operations+ :test #'equal))))
             (when (and (eq operation :event) (not (= 1 (length arguments))))
               (error "event requires exactly one data plist argument."))
             (when (eq operation :hit-test)
               (unless (= 2 (length arguments))
                 (error "hit-test requires exactly two screen coordinates."))
               (return-from run-cli
                 (control-client (socket-path name) operation
                                 (mapcar (lambda (text)
                                           (let ((value (read-data text)))
                                             (unless (%finite-real-p value)
                                               (error "hit-test coordinate must be finite: ~S" text))
                                             value))
                                         arguments))))
             (return-from run-cli (control-client (socket-path name) operation arguments))))
          (t (error "Unknown option or command: ~A" option)))))
    (unless (or bare config) (setf config (default-config-file)))
    (let ((builtins (sb-ext:posix-getenv "TOMOE_BUILTINS")))
      (unless (or bare builtins) (error "TOMOE_BUILTINS is required without --bare."))
      (run-compositor name backend
                      (append (unless bare (list (namestring (truename builtins))))
                              (when config (list config)))
                      watch drm-device))))

(defun main ()
  (sb-ext:exit
   :code (handler-case
             (handler-bind ((serious-condition
                              (lambda (condition)
                                (declare (ignore condition))
                                (when (sb-ext:posix-getenv "TOMOE_DEBUG")
                                  (sb-debug:print-backtrace :count 40 :stream *error-output*)))))
               (run-cli (rest sb-ext:*posix-argv*)))
           (serious-condition (condition) (format *error-output* "tomoe: ~A~%" condition) 1))))
