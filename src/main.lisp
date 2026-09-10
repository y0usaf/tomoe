(in-package #:tomoe)

(defvar *stop-requested* nil)
(defparameter +operations+
  '(("inspect" . :inspect) ("reload" . :reload) ("mount" . :mount)
    ("unmount" . :unmount) ("command" . :command) ("event" . :event)
    ("quit" . :quit)))

(defun usage ()
  (write-line "Usage: tomoe-lisp [--socket NAME] [--backend auto|nested|headless|drm|lisp] [--bare]
       [--config FILE] [--watch|--no-watch]
       tomoe-lisp [--socket NAME] inspect|reload|mount FILE|unmount NAME|command OWNER NAME|event PLIST|quit

Default socket: tomoe-lisp-0. Default backend: auto.
The lisp backend is loaded by dev.lisp and needs no wlroots.
Auto nests in an existing Wayland display, or uses DRM when none is found.
Nested mode discovers live wayland-N sockets when WAYLAND_DISPLAY is unset.
Extensions are trusted Common Lisp programs. --bare omits all shipped policy.
Loads $XDG_CONFIG_HOME/tomoe-lisp/init.lisp or ~/.config/tomoe-lisp/init.lisp when present.
--config overrides that file; --bare skips it.
Extension sources are watched and reloaded when edited; --no-watch disables that.
event sends one data plist to a live instance as an injected input event.
Control replies are versioned Lisp data. Mutating commands are silent on success."))

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
           ;; One attempt per stamp. Failures leave the previous policy mounted.
           (setf (getf entry :loaded) current (getf entry :stable) 0)
           (handler-case (configure runtime (runtime-sources runtime) path)
             (serious-condition (condition) (record-error runtime condition)))))))))

(defun run-compositor (name backend sources watch)
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
    ((equal backend "nested") (sb-posix:setenv "WLR_BACKENDS" "wayland" 1))
    ((equal backend "headless")
     (sb-posix:setenv "WLR_BACKENDS" "headless" 1)
     (unless (sb-ext:posix-getenv "WLR_RENDERER") (sb-posix:setenv "WLR_RENDERER" "pixman" 1)))
    ((equal backend "drm") (sb-posix:setenv "WLR_BACKENDS" "drm,libinput" 1))
    ;; The Lisp backend creates the display itself; no wlroots backend selection.
    ((equal backend "lisp") nil)
    (t (error "Unknown backend: ~A" backend)))
  (setf *stop-requested* nil)
  (flet ((stop (signal info context)
           (declare (ignore signal info context)) (setf *stop-requested* t)))
    (sb-sys:enable-interrupt sb-posix:sigterm #'stop)
    (sb-sys:enable-interrupt sb-posix:sigint #'stop))
  (let ((control (open-control (socket-path name))) (native nil) (runtime nil)
        (stamps (make-hash-table :test #'equal)) (next-watch 0))
    (unwind-protect
         (progn
           (setf native (open-backend name)
                 runtime (make-runtime :backend native :socket name))
           (setf (runtime-watch runtime) watch)
           ;; Only this process and its children inherit the new display. Never
           ;; import it into systemd, D-Bus, or the surrounding desktop session.
           (sb-posix:setenv "WAYLAND_DISPLAY" name 1)
           (sb-posix:unsetenv "DISPLAY")
           (loop for text = (%event native) while text do (dispatch-event runtime (read-data text)))
           (configure runtime sources)
           (loop while (and (runtime-running runtime) (not *stop-requested*)) do
             (let ((status (%step native 8)))
               (when (< status 0) (error "Native event loop failed."))
               (when (> status 0) (return)))
             (loop for text = (%event native) while text do (dispatch-event runtime (read-data text)))
             (serve-control runtime control)
             (reap-processes runtime)
             ;; Bounded to one stamp sweep per 250 ms; a sweep is a few stats.
             (when (and (runtime-watch runtime) (>= (get-internal-real-time) next-watch))
               (setf next-watch (+ (get-internal-real-time)
                                   (floor internal-time-units-per-second 4)))
               (watch-sources runtime stamps)))
           0)
      (unwind-protect (when runtime (stop-processes runtime))
        (unwind-protect (when native (%destroy native)) (close-control control))))))

(defun default-config-file ()
  (let* ((root (sb-ext:posix-getenv "XDG_CONFIG_HOME"))
         (directory (if (and root (plusp (length root)))
                        (format nil "~A/" (string-right-trim "/" root))
                        (merge-pathnames ".config/" (user-homedir-pathname))))
         (path (merge-pathnames "tomoe-lisp/init.lisp" directory)))
    (when (probe-file path) (namestring (truename path)))))

(defun run-cli (arguments)
  (let ((name "tomoe-lisp-0") (backend "auto") (bare nil) (config nil) (watch t))
    (labels ((argument (option)
               (or (pop arguments) (error "~A requires a value." option))))
      (loop while arguments for option = (pop arguments) do
        (cond
          ((equal option "--help") (usage) (return-from run-cli 0))
          ((equal option "--version") (write-line "tomoe-lisp 0.1.0, wire 1, native ABI 3") (return-from run-cli 0))
          ((equal option "--socket") (setf name (argument option)))
          ((equal option "--backend") (setf backend (argument option)))
          ((equal option "--config") (setf config (namestring (truename (argument option)))))
          ((equal option "--bare") (setf bare t))
          ((equal option "--watch") (setf watch t))
          ((equal option "--no-watch") (setf watch nil))
          ((assoc option +operations+ :test #'equal)
           (when (or config bare (not (equal backend "auto")))
             (error "Server options cannot be combined with a control command."))
           (let ((operation (cdr (assoc option +operations+ :test #'equal))))
             (when (and (eq operation :event) (not (= 1 (length arguments))))
               (error "event requires exactly one data plist argument."))
             (return-from run-cli (control-client (socket-path name) operation arguments))))
          (t (error "Unknown option or command: ~A" option)))))
    (unless (or bare config) (setf config (default-config-file)))
    (let ((builtins (sb-ext:posix-getenv "TOMOE_LISP_BUILTINS")))
      (unless (or bare builtins) (error "TOMOE_LISP_BUILTINS is required without --bare."))
      (run-compositor name backend
                      (append (unless bare (list (namestring (truename builtins))))
                              (when config (list config)))
                      watch))))

(defun main ()
  (sb-ext:exit
   :code (handler-case (run-cli (rest sb-ext:*posix-argv*))
           (serious-condition (condition) (format *error-output* "tomoe-lisp: ~A~%" condition) 1))))
