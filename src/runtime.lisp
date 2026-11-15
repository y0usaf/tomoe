(in-package #:tomoe)

(declaim (optimize (speed 3) (safety 1) (debug 1)))

(defun default-mpris-state ()
  (list :available nil :player-name "" :status "" :title "" :artist ""
        :album "" :art-url "" :length 0 :position 0 :volume 1.0d0))

(defun default-battery-state ()
  (list :available nil :percent 100 :charging nil))

(defun default-network-state ()
  (list :connected nil :ssid nil :strength 0))

(defun default-tray-state ()
  (list :items nil))

(defun default-audio-state ()
  (list :volume 1.0d0 :muted nil))

(defvar *materialized* nil
  "Within a transaction: the mounts, invalidation count, output revision and
context of its last materialization.")
(defvar *layout-rebuilds* 0
  "Counts the materializations that folded every effect again, and the ones that
reused the context the last commit settled on.")
(defvar *layout-reuses* 0)
(defvar *invocations* 0
  "Counts the accepted reducer results and replaced rule declarations that change
what a materialization folds. A reducer that returns the effects it already owned
changes nothing, so it does not invalidate the last context.")
(defvar *command-input* nil
  "Within a transaction or a host command call: whether input asked for the command.")
(defvar *definitions-generation* 0
  "Counts loaded sources and restored patch definitions. Both replace functions the
fold calls directly, such as the keyboard default and the host command table, so
they invalidate a context without any effect changing.")

(defvar *patch-generation* 0)

(defvar *speculating* nil
  "Non-nil while a transaction is evaluated for a key that has not arrived yet.
Speculation neither commits nor runs a command, and it stops at the first
reducer that has not declared itself safe to evaluate ahead of its event.")

(defvar *speculation* nil
  "The mounts, context and commands a speculative transaction settled on.")

(defparameter +oracle-focus-unset+ 4294967295
  "Ask the native side for the keyboard focus it holds now when staging a delta.")

(defvar *oracle-enabled* t
  "Whether a key with a staged transition is applied by the native side.
The session sets it from TOMOE_ORACLE when it starts, so a saved image does not
freeze the answer it was built with.")
(defvar *precomputing* nil)
(defvar *timing* nil
  "Whether the native side keeps latency samples, so the loop can report when a
transaction has returned without paying for a call it does not need.")


(defvar *timing-channels* (make-hash-table :test #'equal)
  "Native channel numbers, resolved once per sample name.")

(defvar *timing-consed* (make-hash-table :test #'eql)
  "Per native timing channel: a mutable (consed samples name) list.")

(defun timing-channel (name)
  (or (gethash name *timing-channels*)
      (setf (gethash name *timing-channels*) (%timing-channel name))))

(defun note-timing-consed (channel name bytes)
  (let ((entry (gethash channel *timing-consed*)))
    (if entry
        (progn (incf (first entry) bytes) (incf (second entry)))
        (setf (gethash channel *timing-consed*) (list bytes 1 name)))))

(defun timing-consed-report ()
  "Bytes WITH-SAMPLE recorded per channel, as (:NAME n :CONSED b :SAMPLES c) entries."
  (loop for entry being the hash-values of *timing-consed*
        collect (list :name (third entry) :consed (first entry) :samples (second entry))))

(defmacro with-sample ((name) &body body)
  "Record how long BODY takes on the named native timing path, and how many bytes
it consed, when samples are kept."
  (let ((start (gensym "START-")) (consed (gensym "CONSED-")) (channel (gensym "CHANNEL-")))
    `(if *timing*
         (let ((,channel (timing-channel ,name)) (,start (%timing-now))
               (,consed (sb-ext:get-bytes-consed)))
           (prog1 (progn ,@body)
             (%timing-sample ,channel (- (%timing-now) ,start))
             (note-timing-consed ,channel ,name (- (sb-ext:get-bytes-consed) ,consed))))
         (progn ,@body))))

(defstruct mounted spec state effects (dispatches 0) (failures 0) last-error
  rule-parent rule-name rule-window rule-definition rule-pending)
(defstruct runtime backend socket sources mounts windows client-geometries outputs connectors effective layers
  (services (list :notifications (list :available nil :notifications nil)
                  :mpris (default-mpris-state) :battery (default-battery-state)
                  :network (default-network-state) :tray (default-tray-state)
                  :audio (default-audio-state)))
  system (system-due 0) notification-producer mpris-producer battery-producer network-producer tray-producer
  (running t) last-error (generation 0) (watch t) source-stamps pending-context output-recovery-failures
  (outputs-revision 0) (window-buffer-generation 0) timers executions retired-executions (execution-turn 0)
  reads-cache layers-cache layout-memo
  watches (watch-turn 0)
  managed-processes retired-processes once-processes pending-spawns process-history (process-turn 0)
  background-leases
  json-server ipc-call (ipc-sequence 0) screencasts ipc-published-context ipc-published-announcements
  config-error source-packages retired-commands patches (patch-sequence 0)
  (fd-watches (make-hash-table)) fd-specs watch-failure
  (source-watches (make-hash-table :test #'equal))
  (source-scans 0) (source-deadline 0)
  (activity-serial 0) (idle-since 0) (idle-consed (sb-ext:get-bytes-consed)) idle-trimmed
  idle-swept
  motion-plan
  (oracle-generation 0) (oracle-epoch 0) oracle-ready oracle-memo oracle-mru
  (oracle-adopted 0) (oracle-prepared 0) (oracle-missed 0))

(defconstant +event-readable+ 1)
(defconstant +event-writable+ 2)

(defun socket-descriptor (socket)
  (sb-bsd-sockets:socket-file-descriptor socket))

(declaim (inline least-wait wait-until socket-descriptor))

(defun least-wait (first second)
  (cond ((null first) second) ((null second) first) (t (min first second))))

(defun wait-until (now deadline)
  (max 0 (ceiling (* 1000 (- deadline now)) internal-time-units-per-second)))

(defun sync-fd-watches (runtime specs)
  "Let the native loop wake on SPECS, (DESCRIPTOR MASK IDENTITY) entries. A
descriptor already watched for the same identity and mask costs no syscall; one
whose identity changed is watched again, so a number the kernel has just handed
to another pipe is not left unwatched; descriptors no longer named are dropped."
  (let ((backend (runtime-backend runtime)) (table (runtime-fd-watches runtime)))
    (dolist (spec specs)
      (destructuring-bind (fd mask identity) spec
        (let ((entry (gethash fd table)))
          (cond
            ((and entry (not (equal identity (third entry))))
             (%unwatch-fd backend (first entry))
             (remhash fd table)
             (let ((source (%watch-fd backend fd mask)))
               (if (plusp source)
                   (setf (gethash fd table) (list source mask identity))
                   (setf (runtime-watch-failure runtime) t))))
            ((null entry)
             (let ((source (%watch-fd backend fd mask)))
               (if (plusp source)
                   (setf (gethash fd table) (list source mask identity))
                   (setf (runtime-watch-failure runtime) t))))
            ((/= mask (second entry))
             (%update-fd backend (first entry) mask)
             (setf (second entry) mask))))))
    (when (/= (hash-table-count table) (length specs))
      (let ((stale nil))
        (maphash (lambda (fd entry)
                   (declare (ignore entry))
                   (unless (assoc fd specs) (push fd stale)))
                 table)
        (dolist (fd stale)
          (let ((entry (gethash fd table)))
            (when entry
              (%unwatch-fd backend (first entry))
              (remhash fd table))))))))

(defun drop-dead-fd-watch (runtime handle)
  "Retire the watch the native loop reported dead, by its own handle: the
descriptor number alone cannot tell a live pipe from the one that has gone."
  (let ((table (runtime-fd-watches runtime)) (backend (runtime-backend runtime)) (stale nil))
    (maphash (lambda (fd entry)
               (when (eql (first entry) handle) (push fd stale)))
             table)
    (dolist (fd stale)
      (let ((entry (gethash fd table)))
        (when entry
          (%unwatch-fd backend (first entry))
          (remhash fd table))))))

(define-condition extension-error (error)
  ((unit :initarg :unit :reader extension-error-unit)
   (cause :initarg :cause :reader extension-error-cause))
  (:report (lambda (condition stream)
             (format stream "Extension ~A: ~A"
                     (extension-error-unit condition) (extension-error-cause condition)))))

(defun focused-window (runtime context)
  "The window that accepts keys now, or NIL."
  (if (runtime-backend runtime)
      (let ((id (%keyboard-focus (runtime-backend runtime)))) (unless (zerop id) id))
      (let* ((id (getf context :focus))
             (window (find id (runtime-windows runtime) :key (lambda (window) (getf window :id)))))
        (when (and window (getf window :buffered t)) id))))

(defun window-output (runtime id)
  "The output holding window ID's center, else the first output."
  (let* ((context (runtime-effective runtime))
         (box (and id (find id (getf context :window-geometry)
                            :key (lambda (record) (getf record :id)))))
         (x (and box (+ (getf box :x) (floor (getf box :width) 2))))
         (y (and box (+ (getf box :y) (floor (getf box :height) 2)))))
    (or (and box
             (find-if (lambda (output)
                        (and (<= (getf output :x) x) (< x (+ (getf output :x) (getf output :width)))
                             (<= (getf output :y) y) (< y (+ (getf output :y) (getf output :height)))))
                      (getf context :outputs)))
        (first (getf context :outputs)))))

(defun command-argument-value (runtime context spec value)
  "VALUE as argument SPEC demands: the `:FOCUSED` window or output resolved, the
window and output live, and every type checked."
  (let ((value (if (and (eq value :focused) (member (getf spec :type) '(:window :output)))
                   (ecase (getf spec :type)
                     (:window (or (focused-window runtime context) (error "No focused window.")))
                     (:output (or (window-output runtime (focused-window runtime context))
                                  (error "No output."))))
                   value)))
    (%check-command-value spec value)
    (case (getf spec :type)
      (:window
       (unless (find value (runtime-windows runtime) :key (lambda (window) (getf window :id)))
         (error "No window ~D." value)))
      (:output
       (unless (find value (getf context :outputs)
                     :test #'equal :key (lambda (output) (getf output :name)))
         (error "No output named ~A." value))))
    value))

(defun resolve-command-arguments (runtime entry arguments)
  "Every argument ENTRY declares, in order: given, defaulted, or (for windows and
outputs) the focused one. Absent optional arguments are dropped."
  (let ((context (runtime-effective runtime)) (resolved nil) (name (getf entry :name)))
    (loop for (key) on arguments by #'cddr
          unless (find key (getf entry :arguments) :key (lambda (spec) (getf spec :name)))
            do (error "~A has no ~(~A~) argument." name key))
    (dolist (spec (getf entry :arguments) (nreverse resolved))
      (let* ((key (getf spec :name))
             (value (if (member key arguments) (getf arguments key) nil))
             (present (member key arguments)))
        (cond
          (present (setf value (%coerce-command-value spec value)))
          ((and (not (getf spec :default-p)) (member (getf spec :type) '(:window :output)))
           (setf value :focused))
          ((getf spec :default-p) (setf value (getf spec :default)))
          ((getf spec :required)
           (error "~A needs a ~(~A~) argument." name key))
          (t (setf present nil value nil)))
        (when (or present (getf spec :default-p) (member (getf spec :type) '(:window :output)))
          (push key resolved)
          (push (command-argument-value runtime context spec value) resolved))))))

(defun command-event (runtime entry name arguments event)
  "EVENT as the :COMMAND event that runs NAME for its owning extension."
  (let ((copy (copy-list event)))
    (remf copy :command)
    (setf (getf copy :type) :command
          (getf copy :input) (and (member (getf event :type) '(:key :button :grab)) t)
          (getf copy :owner) (getf entry :owner)
          (getf copy :name) name
          (getf copy :arguments) (resolve-command-arguments runtime entry arguments))
    copy))

(defun callback-event (event name)
  "EVENT as the :COMMAND event of a private dialog callback named NAME."
  (let ((copy (copy-list event)))
    (remf copy :command)
    (setf (getf copy :type) :command (getf copy :name) name (getf copy :arguments) nil)
    copy))

(defun keyboard-call (runtime event)
  "The call a :KEY event names: its binding's press or release call, the
keyboard grab's otherwise call, or NIL for a private dialog callback."
  (let ((command (getf event :command)) (owner (getf event :owner)))
    (when (and command owner)
      (let ((binding (find-if (lambda (binding)
                                (and (equal (getf binding :owner) owner)
                                     (or (equal (getf binding :command) command)
                                         (equal (getf binding :release) command))))
                              (getf (runtime-effective runtime) :bindings))))
        (cond (binding (if (equal command (getf binding :command))
                           (getf binding :call)
                           (getf binding :release-call)))
              (t (let ((grab (getf (runtime-effective runtime) :keyboard-grab)))
                   (when (and grab (equal (getf grab :owner) owner)
                              (equal (getf grab :otherwise) command))
                     (getf grab :call)))))))))

(defun commit-call-resolution (runtime context)
  "Resolve every binding in CONTEXT against the commands it declares, remembering
command names whose owner has gone away so their bindings retire with it."
  (let* ((commands (getf context :commands))
         (retired (remove-if (lambda (name) (registered-command commands name))
                             (union (runtime-retired-commands runtime)
                                    (loop for entry in (getf (runtime-effective runtime) :commands)
                                          unless (registered-command commands (getf entry :name))
                                            collect (getf entry :name))))))
    (setf (runtime-retired-commands runtime) retired)
    (resolve-context-calls context retired)))

(defun run-command (runtime name arguments)
  "Run the registered command NAME for the caller, filling declared defaults.
Returns (:NAME NAME :ARGUMENTS a plist of the values it ran with)."
  (let* ((context (runtime-effective runtime))
         (entry (or (registered-command (getf context :commands) name)
                    (error "Unknown command: ~A." name))))
    (if (getf entry :host)
        (host-command-call runtime entry arguments)
        (let* ((owner (getf entry :owner))
               (mounted (find owner (runtime-mounts runtime)
                              :test #'equal
                              :key (lambda (mounted) (spec-name (mounted-spec mounted)))))
               (failures (and mounted (mounted-failures mounted)))
               (event (command-event runtime entry name arguments (list :type :command :state :pressed))))
          (dispatch-event runtime event)
          (when (and mounted (> (mounted-failures mounted) (or failures 0)))
            (error "~A: ~A" owner (mounted-last-error mounted)))
          (list :name name :arguments (getf event :arguments))))))

(defun canonical-effect (effect)
  (check-type effect effect)
  (funcall (effect-definition-canonical (%effect-definition (effect-kind effect)))
           (copy-data (effect-arguments effect)) effect))

(defun canonical-command (command &optional directory)
  (check-type command command)
  (let ((args (copy-data (command-arguments command))))
    (ecase (command-kind command)
      (:reply (destructuring-bind (value) args (ipc-reply value)))
      (:broadcast (destructuring-bind (name value) args (broadcast name value)))
      (:launch (apply #'launch args))
      (:spawn (destructuring-bind (launch cwd env focus) args
                (let ((result (spawn launch :cwd cwd :env env :focus focus)))
                  (when (and cwd (not (char= (char cwd 0) #\/)))
                    (unless directory (error "Relative spawn cwd requires a declaring source."))
                    (setf (second (command-arguments result))
                          (concatenate 'string directory cwd)))
                  result)))
      (:close (destructuring-bind (id) args (close-window id)))
      (:screenshot (destructuring-bind (screen) args (screenshot (and screen :screen))))
      (:screencast (destructuring-bind (token answer value) args (screencast-answer token answer value)))
      (:media (destructuring-bind (action) args (media-control action)))
      (:clipboard (destructuring-bind (path type delete) args (clipboard-copy path type :delete delete)))
      (:brightness (destructuring-bind (percent) args (adjust-brightness percent)))
      (:power (destructuring-bind (mode name) args (output-power mode name)))
      (:quit (unless (null args) (error "QUIT takes no arguments.")) (quit))
      (:reload (unless (null args) (error "RELOAD takes no arguments.")) (reload)))))

(defun effect-key (effect)
  (let ((key (effect-definition-key (%effect-definition (effect-kind effect)))))
    (if key
        (funcall key (effect-arguments effect))
        (list (effect-kind effect) (first (effect-arguments effect))))))

(defun equivalent-effects (before after)
  "Whether AFTER contributes to a materialization exactly what BEFORE did.
Reducers fold an effect's kind and arguments into M, and both lists belong to the
same mounted owner, so identical contributions owe no rebuild. The predicate and
application closures of a rule declaration are not folded; RECONCILE compares
declarations by identity where that matters."
  (and (= (length before) (length after))
       (every (lambda (old new)
                (and (eq (effect-kind old) (effect-kind new))
                     (equal (effect-arguments old) (effect-arguments new))))
              before after)))

(defun check-duplicate-effects (effects)
  "Reject two effects of one reducer that claim the same key. Reducers own a
handful of effects, so a scan beats a hash table until the list is long."
  (if (<= (length effects) 8)
      (loop for tail on effects
            for key = (effect-key (first tail))
            do (when (loop for other in (rest tail) thereis (equal key (effect-key other)))
                 (error "Duplicate effect: ~S" key)))
      (let ((keys (make-hash-table :test #'equal :size (length effects))))
        (dolist (effect effects)
          (let ((key (effect-key effect)))
            (when (gethash key keys) (error "Duplicate effect: ~S" key))
            (setf (gethash key keys) t))))))


(defun spec-immutable-p (spec)
  (and spec (eql (spec-immutable-generation spec) *patch-generation*)))

(defun spec-precomputable-p (spec)
  (and spec (spec-precompute spec) (eql (spec-precompute-generation spec) *patch-generation*)))
(defun invoke-extension (runtime mounted context event)
  (let* ((spec (mounted-spec mounted)) (reads (mount-context-reads mounted))
         (rule-p (not (null (mounted-rule-parent mounted))))
         (shared (and (not rule-p) (spec-immutable-p spec)))
         (*source-directory* (spec-directory spec)))
    (handler-case
        (sb-ext:with-timeout (/ (getf (getf context :settings) :watchdog-ms 1000) 1000)
          (multiple-value-bind (state effects commands)
              (funcall (if rule-p
                           (lambda (snapshot state event)
                             (invoke-rule-application runtime mounted snapshot state event))
                           (spec-update spec))
                       (make-snapshot context (if shared reads (copy-list reads))
                                      (runtime-effective runtime) shared)
                       (if shared (mounted-state mounted) (copy-data (mounted-state mounted)))
                       (if shared event (copy-data event)))
            (when (> (length effects) 512) (error "More than 512 effects."))
            (when (> (length commands) 32) (error "More than 32 commands."))
            (when (and commands
                       (not (or (member (getf event :type)
                                        '(:command :button :timer :watch :exec :request :screenshot
                                          :screencast :ipc :ui))
                                (and rule-p (eq (getf event :type) :mount)))))
              (error "One-shot commands require a command, button, timer, watch, exec, request, screenshot, screencast, IPC, or UI event."))
            ;; A reducer proven never to mutate or retain its arguments already
            ;; builds canonical effects through the public constructors, so its
            ;; effects are adopted as they are; anything unproven keeps its copy.
            (let ((effects (if shared
                               (copy-list effects)
                               (mapcar #'canonical-effect effects)))
                  (commands (when commands
                              (mapcar (lambda (command) (canonical-command command (spec-directory spec))) commands))))
              (let ((replies (if commands (count :reply commands :key #'command-kind) 0)))
                (when (plusp replies)
                  (unless (and (= replies 1) (eq (getf event :type) :ipc)
                               (equal (getf event :owner) (spec-name spec))
                               (runtime-ipc-call runtime)
                               (eql (getf event :call) (getf (runtime-ipc-call runtime) :token)))
                    (error "IPC-REPLY requires this scope's current IPC request and permits one answer."))))
              (check-duplicate-effects effects)
              (unless (equivalent-effects (mounted-effects mounted) effects)
                (incf *invocations*))
              (setf (mounted-state mounted) (if shared (check-data state) (copy-data state))
                    (mounted-effects mounted) effects)
              (when rule-p (setf (mounted-rule-pending mounted) nil))
              (incf (mounted-dispatches mounted))
              commands)))
      (serious-condition (condition)
        (let ((live (find (spec-name spec) (runtime-mounts runtime)
                          :test #'equal :key (lambda (m) (spec-name (mounted-spec m))))))
          (when live
            (incf (mounted-failures live))
            (setf (mounted-last-error live) (princ-to-string condition))))
        (error 'extension-error :unit (spec-name spec) :cause condition)))))

(defun resolved-grab (runtime mounts)
  "The last owner's grab on a buffered target, and that owner. A detached or
destroyed target cannot mask another owner's available grab. Registry lifetime
is independent."
  (let ((grab nil) (owner nil))
    (dolist (mounted mounts (values grab owner))
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :grab)
          (let* ((args (effect-arguments effect))
                 (id (first args))
                 (generation (third args))
                 (window (find id (runtime-windows runtime) :key (lambda (w) (getf w :id)))))
            (when (or (eq (second args) :pointer)
                      (and window (getf window :buffered t)
                           (or (null generation) (eql generation (getf window :buffer-generation))))
                      (and (null generation)
                           (find id (runtime-layers runtime) :key (lambda (l) (getf l :id)))))
              (setf grab (list id (second args))
                    owner mounted))))))))

(defparameter +motion-reads+ '(:pointer :grab)
  "Context keys whose readers see pointer motion and therefore need a contract
before native code may move the pointer without them.")

(defun motion-contract-of (mounted)
  "MOUNTED's published native motion contract, or NIL."
  (let ((effect (find :motion (mounted-effects mounted) :key #'effect-kind)))
    (and effect (effect-arguments effect))))

(defun motion-reader-p (mounted)
  "Whether MOUNTED's answer depends on pointer motion."
  (let ((reads (mount-context-reads mounted)))
    (or (member :pointer reads) (member :grab reads))))

(defun resolved-motion-plan (mounts grab grab-owner)
  "The pointer gate native code may follow, or NIL when it must leave motion to
Lisp. Fast only when every extension that reads pointer motion publishes a
contract, and only while no grab is active unless the grab's owner translates it
natively. A gate that cannot be fast stages nothing: a fresh presentation is
already inert, and motion-plan-changed-p is what clears a gate that was fast."
  (let ((fast t) (hover nil) (regions nil) (edges nil) (distance nil) (readers nil))
    (dolist (mounted mounts)
      (when (motion-reader-p mounted)
        (setf readers t)
        (let ((contract (motion-contract-of mounted)))
          (if (null contract)
              (setf fast nil)
              (setf hover (or hover (getf contract :hover))
                    regions (append regions (getf contract :regions))
                    edges (append edges (getf contract :edges))
                    distance (if distance (min distance (getf contract :distance))
                                 (getf contract :distance)))))))
    (when readers
      (let* ((drag (and grab-owner (second grab) (motion-contract-of grab-owner)))
             (drag (and drag
                        (member (second grab) '(:move :resize))
                        (getf drag :drag)
                        (list* :id (first grab) :mode (second grab) (getf drag :drag)))))
        (when (and grab (not drag)) (setf fast nil))
        (when fast
          (list :fast t :hover (and hover t)
                :regions (remove-duplicates regions :test #'equal)
                :edges (remove-duplicates edges :test #'equal)
                :distance (or distance 0)
                :drag drag))))))

(defun motion-plan-changed-p (runtime motion)
  "Whether the native core must be told about MOTION. An inert plan and none at
all are the same thing, so a desktop that keeps the Lisp path never republishes
just to say so."
  (let ((applied (runtime-motion-plan runtime)))
    (if (getf motion :fast)
        (not (equal motion applied))
        (and applied (getf applied :fast)))))

(defun resolved-layer-overrides (mounts)
  "Resolve owned fields separately from client facts. NIL fields abstain, so
removing the final owner restores the native protocol's live defaults."
  (let ((overrides nil))
    (dolist (mounted mounts overrides)
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :layer)
          (destructuring-bind (id layer zone keyboard visible) (effect-arguments effect)
            (let ((entry (assoc id overrides)))
              (unless entry
                (setf entry (list id nil nil nil t))
                (push entry overrides))
              (when layer (setf (second entry) layer))
              (when zone (setf (third entry) zone))
              (when keyboard (setf (fourth entry) keyboard))
              (setf (fifth entry) visible))))))))

(defun applied-grab (backend)
  "Read the backend's actual grab. No Lisp cache can outlive a native target."
  (let ((id (%grab-id backend)))
    (unless (zerop (%grab-mode backend))
      (list id (ecase (%grab-mode backend) (1 :move) (2 :resize) (3 :pointer))))))

(defun describe-grab (grab)
  (when grab (list :id (first grab) :mode (second grab))))

(defun %view-screen-point (window view)
  "Project a physical world point through VIEW into screen coordinates."
  (let ((zoom (getf view :zoom 1d0)))
    (values (* (- (getf window :x) (getf view :x 0)) zoom)
            (* (- (getf window :y) (getf view :y 0)) zoom))))

(defun %output-containing-point (outputs x y)
  (find-if (lambda (output)
             (let ((ox (getf output :x)) (oy (getf output :y))
                   (width (getf output :width)) (height (getf output :height)))
               (and (integerp ox) (integerp oy)
                    (integerp width) (integerp height)
                    (plusp width) (plusp height)
                    (<= ox x) (< x (+ ox width))
                    (<= oy y) (< y (+ oy height)))))
           outputs))

(defun %output-scale-120 (x y output-facts)
  "Return the scale of the screen point, or the first output's scale."
  (let* ((outputs output-facts)
         (output (or (%output-containing-point outputs x y) (first outputs)))
         (scale (and output (getf output :scale-120)))
         (fallback (and (first outputs) (getf (first outputs) :scale-120))))
    (cond ((and (%finite-real-p scale) (plusp scale)) (coerce scale 'double-float))
          ((and (%finite-real-p fallback) (plusp fallback)) (coerce fallback 'double-float))
          (t 120d0))))

(defun %quantize-layout (layout view output-facts)
  "Apply the physical/client-size round trip after every owner effect settles."
  (dolist (window layout)
    (multiple-value-bind (screen-x screen-y) (%view-screen-point window view)
      (let* ((scale (%output-scale-120 screen-x screen-y output-facts))
             (physical-width (coerce (getf window :width) 'double-float))
             (physical-height (coerce (getf window :height) 'double-float))
             (logical-width
               (max 1 (%round-away-positive (/ (* physical-width 120d0) scale))))
             (logical-height
               (max 1 (%round-away-positive (/ (* physical-height 120d0) scale))))
             (actual-width
               (max 1 (%round-away-positive (/ (* logical-width scale) 120d0))))
             (actual-height
               (max 1 (%round-away-positive (/ (* logical-height scale) 120d0)))))
        (setf (getf window :width) actual-width
              (getf window :height) actual-height))))
  layout)

(defun output-connectors (snapshot)
  "Connected ports survive policy disablement; older/pure events describe active ports only."
  (if (member :connectors snapshot)
      (getf snapshot :connectors)
      (loop for output in (getf snapshot :outputs) collect
        (list :name (getf output :name) :enabled t
              :adaptive-sync-supported (getf output :adaptive-sync-supported)
              :adaptive-sync (getf output :adaptive-sync)))))

(defun connector-configuration-identity (connector)
  (list (getf connector :id) (getf connector :request-id)
        (copy-data (getf connector :modes))
        (getf connector :adaptive-sync-supported)))

(defun resolved-native-output-config (context)
  (if (member :native-output-config context)
      (getf context :native-output-config)
      (connected-output-config context)))

(defun output-failure (connector config condition)
  (list :name (getf connector :name) :config (copy-data config)
        :connector (connector-configuration-identity connector)
        :message (princ-to-string condition)))

(defun matching-output-failures (failures requested connectors)
  (remove-duplicates
   (remove-if-not
    (lambda (failure)
      (let* ((name (getf failure :name))
             (connector (find name connectors :test #'equal
                              :key (lambda (item) (getf item :name))))
             (config (find name requested :test #'equal
                           :key (lambda (item) (getf item :name)))))
        (and connector (equal config (getf failure :config))
             (equal (connector-configuration-identity connector)
                    (getf failure :connector)))))
    failures)
   :test #'equal :key (lambda (item) (getf item :name)) :from-end t))

(defun resolve-output-preview (runtime requested connectors)
  "Keep output failures local to their connector, request and declaration.
Unavailable advertised modes use the native preferred-mode fallback. Actual
failures hold arriving outputs inactive and active outputs at accepted geometry
while other owners can settle; a new request or declaration permits recovery."
  (let* ((old (runtime-effective runtime))
         (failures
           (matching-output-failures
            (append (runtime-output-recovery-failures runtime)
                    (copy-list (getf old :output-failures)))
            requested connectors)))
    (loop
      (let* ((native-config
               (sort
                (append
                 (remove-if (lambda (config)
                              (find (getf config :name) failures :test #'equal
                                    :key (lambda (item) (getf item :name))))
                            requested)
                 (loop for failure in failures collect
                   (list :name (getf failure :name) :hold t)))
                #'string< :key (lambda (item) (getf item :name))))
             (backend (runtime-backend runtime)))
        (handler-case
            (return
              (values
               (if (and backend
                        (or (pending-native-outputs-p backend)
                            (not (equal (resolved-native-output-config old) native-config))))
                   (preview-native-outputs backend native-config)
                   (list :outputs (runtime-outputs runtime) :connectors connectors))
               native-config (sort failures #'string< :key (lambda (item) (getf item :name)))))
          (output-configuration-error (condition)
            (let* ((name (output-error-name condition))
                   (connector (find name connectors :test #'equal
                                    :key (lambda (item) (getf item :name))))
                   (config (find name requested :test #'equal
                                 :key (lambda (item) (getf item :name))))
                   (retained (find name (getf old :output-config) :test #'equal
                                   :key (lambda (item) (getf item :name)))))
              (unless (and config retained (equal config retained)
                           (or (getf connector :pending) (getf connector :request-pending))
                           (not (find name failures :test #'equal
                                      :key (lambda (item) (getf item :name)))))
                (error condition))
              (push (output-failure connector config condition) failures))))))))

(defun pending-output-failures (runtime condition)
  (loop for connector in (runtime-connectors runtime)
          when (or (getf connector :request-pending)
                   (and (typep condition 'output-configuration-error)
                        (getf connector :pending))) collect
            (output-failure connector
                            (find (getf connector :name)
                                  (getf (runtime-effective runtime) :output-config)
                                  :test #'equal :key (lambda (item) (getf item :name)))
                            condition)))

(defun newer-native-output-request-p (runtime)
  (let ((backend (runtime-backend runtime)))
    (when (and backend
               (> (%outputs-revision backend) (runtime-outputs-revision runtime)))
      (loop for connector in (output-connectors (current-native-outputs backend))
            for previous = (find (getf connector :name) (runtime-connectors runtime)
                                 :test #'equal :key (lambda (item) (getf item :name)))
            thereis (and (getf connector :request-pending)
                         (or (not (eql (getf connector :id) (getf previous :id)))
                             (not (eql (getf connector :request-id)
                                       (getf previous :request-id)))))))))

(defun refresh-output-facts (runtime)
  "Adopt newer committed native output facts before rebuilding policy context."
  (let ((backend (and runtime (runtime-backend runtime))))
    (when backend
      (let ((revision (%outputs-revision backend)))
        (when (> revision (runtime-outputs-revision runtime))
          (let* ((snapshot (current-native-outputs backend))
                 (facts (getf snapshot :outputs))
                 (connectors (output-connectors snapshot))
                 (snapshot-revision (getf snapshot :revision)))
            (unless (and (integerp snapshot-revision)
                         (>= snapshot-revision revision))
              (error "Native output snapshot revision regressed: ~S" snapshot))
            (unless (equal facts (runtime-outputs runtime))
              (pushnew :outputs (runtime-pending-context runtime)))
            (unless (equal connectors (runtime-connectors runtime))
              (pushnew :connectors (runtime-pending-context runtime)))
            (setf (runtime-outputs runtime) facts
                  (runtime-connectors runtime) connectors
                  (runtime-outputs-revision runtime) snapshot-revision))))))
  runtime)

(defun resolved-window-geometry (runtime layout view outputs)
  "Use committed logical sizes with the same candidate scale/location as rendering."
  (loop for box in layout when (getf box :visible) collect
    (let* ((id (getf box :id))
           (client (find id (runtime-client-geometries runtime)
                         :key (lambda (record) (getf record :id)))))
      (multiple-value-bind (screen-x screen-y) (%view-screen-point box view)
        (let ((scale (%output-scale-120 screen-x screen-y outputs)))
          (list :id id :x (getf box :x) :y (getf box :y)
                :width (if client (%round-away-positive (/ (* (getf client :width) scale) 120d0))
                           (getf box :width))
                :height (if client (%round-away-positive (/ (* (getf client :height) scale) 120d0))
                            (getf box :height))))))))

(defun observe-client-geometry (runtime id width height)
  (unless (and (typep width '(integer 0 2147483647))
               (typep height '(integer 0 2147483647)))
    (error "Invalid committed client geometry: ~S x ~S." width height))
  (let* ((old (find id (runtime-client-geometries runtime) :key (lambda (entry) (getf entry :id))))
         (new (list :id id :width width :height height)))
    (unless (equal old new)
      (setf (runtime-client-geometries runtime)
            (if old (substitute new old (runtime-client-geometries runtime))
                (append (runtime-client-geometries runtime) (list new))))
      t)))

(defvar *layout-trace-path* nil
  "When the environment names a file, every materialization appends what it settled
on to it, so a recorded trace can compare an incremental layout with a full one.")

(defun layout-step-record (context)
  "What a materialization settled on: the layout, its inputs and the commands its
owners declared, in a form whose printed representation is stable across builds."
  (list :layout (loop for window in (getf context :layout)
                      collect (list (getf window :id) (getf window :x) (getf window :y)
                                    (getf window :width) (getf window :height)
                                    (and (getf window :visible) t)
                                    (getf window :properties)))
        :stacking (getf context :stacking)
        :window-geometry (getf context :window-geometry)
        :workareas (getf context :workareas)
        :surfaces (getf context :surfaces)
        :view (getf context :view) :focus (getf context :focus)
        :rules (getf context :rules)
        :commands (mapcar (lambda (entry) (getf entry :name)) (getf context :commands))
        :bindings (loop for binding in (getf context :bindings)
                        collect (list (getf binding :owner) (getf binding :modifiers)
                                      (getf binding :code) (getf binding :command)
                                      (getf binding :release)))))

(defun trace-layout-step (context)
  (when *layout-trace-path*
    (let ((*print-pretty* nil) (*print-length* nil) (*print-level* nil)
          (*print-circle* nil) (*print-readably* nil) (*package* *package*))
      (with-open-file (stream *layout-trace-path* :direction :output
                                                   :if-exists :append :if-does-not-exist :create)
        (write (layout-step-record context) :stream stream)
        (terpri stream)))))

(defstruct layout-memo names invocations definitions revision windows layers client-geometries
  services system config-error outputs connectors recovery-failures context)

(defun mount-names (mounts)
  (loop for mounted in mounts collect (spec-name (mounted-spec mounted))))

(defun same-mounts-p (names mounts)
  (and (= (length names) (length mounts))
       (loop for name in names
             for mounted in mounts
             always (equal name (spec-name (mounted-spec mounted))))))

(defun reusable-layout (runtime mounts)
  "The context the last materialization produced, when nothing a materialization
folds has changed since: no owner's effects, no window, layer, client geometry,
service, system, config-error or output fact, and no output proposal in flight.
The fold reads exactly those, so a full recompute would produce this again."
  (let ((memo (runtime-layout-memo runtime)) (backend (runtime-backend runtime)))
    (when (and memo
               (eql (layout-memo-invocations memo) *invocations*)
               (eql (layout-memo-definitions memo) *definitions-generation*)
               (eql (layout-memo-revision memo) (runtime-outputs-revision runtime))
               (eq (layout-memo-windows memo) (runtime-windows runtime))
               (eq (layout-memo-layers memo) (runtime-layers runtime))
               (eq (layout-memo-client-geometries memo) (runtime-client-geometries runtime))
               (eq (layout-memo-services memo) (runtime-services runtime))
               (eq (layout-memo-system memo) (runtime-system runtime))
               (eq (layout-memo-config-error memo) (runtime-config-error runtime))
               (eq (layout-memo-outputs memo) (runtime-outputs runtime))
               (eq (layout-memo-connectors memo) (runtime-connectors runtime))
               (eq (layout-memo-recovery-failures memo) (runtime-output-recovery-failures runtime))
               (or (null backend) (not (pending-native-outputs-p backend)))
               (same-mounts-p (layout-memo-names memo) mounts))
      (layout-memo-context memo))))

(defun remember-layout (runtime mounts context)
  (let ((backend (runtime-backend runtime)))
    (when (or (null backend) (not (pending-native-outputs-p backend)))
      (setf (runtime-layout-memo runtime)
            (make-layout-memo :names (mount-names mounts) :invocations *invocations*
                              :definitions *definitions-generation*
                              :revision (runtime-outputs-revision runtime)
                              :windows (runtime-windows runtime)
                              :layers (runtime-layers runtime)
                              :client-geometries (runtime-client-geometries runtime)
                              :services (runtime-services runtime)
                              :system (runtime-system runtime)
                              :config-error (runtime-config-error runtime)
                              :outputs (runtime-outputs runtime)
                              :connectors (runtime-connectors runtime)
                              :recovery-failures (runtime-output-recovery-failures runtime)
                              :context context)))))

(defun materialize (runtime mounts)
  "Reconstruct owned context. Within a transaction, the same mounts with no
contribution that changes it since the last materialization yield that context
again, and a transaction whose event cannot reach the layout reuses the context
the last one settled on instead of folding every effect again. Adopted native
output facts are a rebuild cause too, so a transaction never commits a context
built from superseded output facts."
  (let ((memo *materialized*) (revision (runtime-outputs-revision runtime)))
    (flet ((republish (context)
             (when memo
               (setf (first memo) (copy-list mounts) (second memo) *invocations*
                     (third memo) context (fourth memo) revision))
             (trace-layout-step context)
             context))
      (cond
        ((and memo (eql (second memo) *invocations*) (eql (fourth memo) revision)
              (= (length mounts) (length (first memo))) (every #'eq mounts (first memo)))
         (incf *layout-reuses*)
         (republish (third memo)))
        (t (let ((reused (reusable-layout runtime mounts)))
             (cond (reused (incf *layout-reuses*) (republish reused))
                   (t (incf *layout-rebuilds*)
                      (republish (%materialize runtime mounts))))))))))

(defun %materialize (runtime mounts)
  (with-sample ("mat-total") (%materialize-fold runtime mounts)))

(defun %materialize-fold (runtime mounts)
  "Reconstruct owned context. Preserve live clients, output facts, and map order.
Unmount removes a contribution, exposing the preceding owner or these defaults.
The single grab is not context data; RESOLVED-GRAB derives it from the mounts."
  (let ((m (with-sample ("mat-init")
             (setf mounts (prune-rule-mounts runtime mounts))
             (refresh-output-facts runtime)
             (make-materialization
              :layout (loop for window in (runtime-windows runtime)
                            collect (list :id (getf window :id) :x 0 :y 0
                                          :width (max 1 (getf window :width))
                                          :height (max 1 (getf window :height)) :visible t
                                          :fullscreen nil :maximize nil :properties nil))
              :stacking (mapcar (lambda (window) (getf window :id)) (runtime-windows runtime))
              :layers (mapcar #'copy-list (runtime-layers runtime))
              :keyboard (default-keyboard-config)
              :settings (%settings-defaults +settings+)
              :view (list :x 0 :y 0 :zoom 1d0)))))
    (with-sample ("mat-replay")
      (dolist (phase '(0 1))
        (when (= phase 1)
          (setf (materialization-mod-bit m)
                (ecase (getf (materialization-settings m) :mod) (:shift 1) (:control 4) (:alt 8) (:super 64))))
        (dolist (mounted mounts)
          (dolist (effect (mounted-effects mounted))
            (let ((definition (%effect-definition (effect-kind effect))))
              (when (and (effect-definition-reduce definition) (= phase (effect-definition-phase definition)))
                (funcall (effect-definition-reduce definition) m (effect-arguments effect) mounted)))))))
    (with-accessors ((layout materialization-layout) (stacking materialization-stacking)
                     (layers materialization-layers) (focused materialization-focused)
                     (data materialization-data) (surfaces materialization-surfaces)
                     (keyboard-grab materialization-keyboard-grab) (outputs materialization-outputs)
                     (bindings materialization-bindings) (keyboard materialization-keyboard)
                     (settings materialization-settings) (view materialization-view))
        m
      (let* ((live-connectors (or (runtime-connectors runtime)
                                  (output-connectors (list :outputs (runtime-outputs runtime)))))
             (output-config
               (with-sample ("mat-config")
                 (default-output-config outputs live-connectors (getf settings :scale))))
             (connected
               (with-sample ("mat-config")
                 (connected-output-config
                  (list :connectors live-connectors :output-config output-config))))
             (output-resolution
               (with-sample ("mat-output")
                 (multiple-value-list
                  (resolve-output-preview runtime connected live-connectors))))
             (output-preview (first output-resolution))
             (native-output-config (second output-resolution))
             (output-failures (third output-resolution))
             (output-facts (getf output-preview :outputs))
             (connector-facts (output-connectors output-preview)))
        (with-sample ("mat-quantize")
          (%quantize-layout layout view output-facts))
        (multiple-value-bind (resolved-layers workareas)
            (with-sample ("mat-layers")
              (let* ((overrides (resolved-layer-overrides mounts))
                     (cache (runtime-layers-cache runtime)))
                (if (and cache (equal (first cache) output-facts)
                         (equal (second cache) layers) (equal (third cache) overrides))
                    (values (fourth cache) (fifth cache))
                    (multiple-value-bind (resolved areas)
                        (if (runtime-backend runtime)
                            (preview-native-layers (runtime-backend runtime) output-facts layers
                                                   overrides)
                            (values layers (full-workareas output-facts)))
                      (setf (runtime-layers-cache runtime)
                            (list output-facts layers overrides resolved areas))
                      (values resolved areas)))))
          (unless (find focused layout :key (lambda (w) (getf w :id)))
            (setf focused nil))
          (multiple-value-bind (surface-plans shell-workareas)
              (with-sample ("mat-surfaces")
               (resolve-shell-surfaces
                (nreverse surfaces) output-facts workareas
                :asset-loader (when (runtime-backend runtime)
                                (native-ui-asset-loader (runtime-backend runtime)))
                :viewport (shell-viewport output-facts (materialization-data m))))
          (with-sample ("mat-build")
           (list :windows (runtime-windows runtime)
                :window-geometry (with-sample ("mat-quantize")
                                   (resolved-window-geometry runtime layout view output-facts))
                :rules (resolved-rule-properties runtime mounts) :data data
                :services (runtime-services runtime)
                :system (runtime-system runtime)
                :outputs output-facts :connectors connector-facts
                :output-config output-config :native-output-config native-output-config
                :output-failures output-failures
                :output-errors (loop for failure in output-failures collect
                                 (list :name (getf failure :name) :message (getf failure :message)))
                :workareas shell-workareas
                :surface-plans surface-plans
                :surfaces (loop for plan in surface-plans
                                collect (list :owner (getf plan :owner) :source-id (getf plan :source-id)
                                              :name (getf plan :name) :output (getf plan :output)
                                              :x (getf plan :x) :y (getf plan :y)
                                              :width (getf plan :width) :height (getf plan :height)
                                              :layer (getf plan :layer) :click-through (getf plan :click-through)))
                :view view :layout layout
                :stacking (remove-if-not
                           (lambda (id) (getf (find id layout :key (lambda (window) (getf window :id))) :visible))
                           stacking)
                :layers resolved-layers :focus focused :keyboard keyboard :settings settings
                :sounds (materialization-sounds m)
                :virtual-outputs (sort (copy-list (materialization-virtual-outputs m)) #'string<
                                       :key (lambda (o) (getf o :name)))
                :config-error (runtime-config-error runtime)
                :keyboard-grab keyboard-grab
                :commands (sort (append (host-command-entries)
                                        (copy-list (materialization-commands m)))
                                #'string< :key (lambda (entry) (getf entry :name)))
                :bindings (sort bindings
                                (lambda (a b) (or (< (getf a :modifiers) (getf b :modifiers))
                                                  (and (= (getf a :modifiers) (getf b :modifiers))
                                                       (< (getf a :code) (getf b :code))))))))))))))

(defun default-output-config (outputs connectors scale)
  "Fill omitted scales from SCALE, and configure every other connector at SCALE
when it is not 1."
  (let ((scale-120 (%round-away-positive (* scale 120))))
    (sort (append
           (loop for output in outputs
                 collect (let ((copy (copy-list output)))
                           (unless (getf copy :scale-120) (setf (getf copy :scale-120) scale-120))
                           copy))
           (unless (= scale-120 120)
             (loop for connector in connectors
                   for name = (getf connector :name)
                   unless (find name outputs :test #'equal :key (lambda (o) (getf o :name)))
                     collect (list :name name :mode :preferred :width 0 :height 0 :refresh-mhz 0
                                   :scale-120 scale-120 :x 0 :y 0 :positioned nil
                                   :disabled nil :mirror nil :vrr nil :icc nil :transform nil
                                   :tearing nil))))
          #'string< :key (lambda (o) (getf o :name)))))

(defun changed-keys (before after)
  (remove-if (lambda (key) (equal (getf before key) (getf after key))) +context-keys+))

(defun connected-output-config (context)
  (remove-if-not (lambda (config)
                   (find (getf config :name) (output-connectors context)
                         :test #'equal :key (lambda (o) (getf o :name))))
                 (getf context :output-config)))

(defun commit-context (runtime mounts context &optional commands &key (native t))
  "The only extension-to-native write path. All reducers have returned and validated.
NATIVE NIL adopts a context whose presentation the native side has published
already, and commits the rest of it without publishing a second time."
  (setf mounts (prune-rule-mounts runtime mounts))
  (let ((watches (prepare-watches runtime mounts)))
    (unwind-protect (%commit-context runtime mounts context commands watches native)
      (discard-watch-plan watches))))

(defun %commit-context (runtime mounts context commands watches native)
  ;; Timers declared :REFRESH resolve against the display rate the context
  ;; reports, so the bar and HUD step once per refresh of the monitor in use.
  (incf (runtime-oracle-generation runtime))
  (setf *frame-ms* (frame-period-ms (getf context :outputs)))
  (let* ((old (runtime-effective runtime)) (backend (runtime-backend runtime))
         (restack (not (equal (getf old :stacking) (getf context :stacking))))
         (layout-changed (not (equal (getf old :layout) (getf context :layout))))
         (focus-changed (not (eql (getf old :focus) (getf context :focus))))
         (outputs (resolved-native-output-config context))
         (outputs-changed (or (pending-native-outputs-p backend)
                              (not (equal (resolved-native-output-config old) outputs))))
         (bindings-changed (not (equal (getf old :bindings) (getf context :bindings))))
         (keyboard-changed (not (equal (getf old :keyboard) (getf context :keyboard))))
         (settings-changed (not (equal (getf old :settings) (getf context :settings))))
         (sounds-changed (not (equal (getf old :sounds) (getf context :sounds))))
         (grab-changed (not (equal (getf old :keyboard-grab) (getf context :keyboard-grab))))
         (overrides (resolved-layer-overrides mounts))
         (grab-details (multiple-value-list (resolved-grab runtime mounts)))
         (grab (first grab-details))
         (motion (resolved-motion-plan mounts grab (second grab-details)))
         (timers (prepare-timers runtime mounts))
         (executions (prepare-executions runtime mounts))
         (processes (prepare-managed-processes runtime mounts))
         (spawns (prepare-session-spawns runtime commands processes))
         (pending-spawns (append (remove nil spawns) (runtime-pending-spawns runtime))))
    (when (or (member :outputs (runtime-pending-context runtime))
              outputs-changed bindings-changed keyboard-changed settings-changed sounds-changed grab-changed
              restack layout-changed focus-changed
              (not (equal (getf old :outputs) (getf context :outputs)))
              (not (equal (getf old :view) (getf context :view)))
              (not (equal (getf old :layers) (getf context :layers)))
              (not (equal (getf old :surface-plans) (getf context :surface-plans)))
              (not (equal (resolved-layer-overrides (runtime-mounts runtime)) overrides))
              (not (equal grab (applied-grab backend)))
              (motion-plan-changed-p runtime motion))
      (when native
        (configure-native-presentation backend outputs context overrides restack
                                       outputs-changed bindings-changed grab motion keyboard-changed
                                       settings-changed sounds-changed)
        (setf (runtime-motion-plan runtime) motion)))
    (when outputs-changed
      (setf (runtime-outputs runtime) (getf context :outputs)
            (runtime-connectors runtime) (getf context :connectors))
      (unless (pending-native-outputs-p backend)
        (setf (runtime-outputs-revision runtime) (%outputs-revision backend))))
    (setf (runtime-mounts runtime) mounts (runtime-effective runtime) context
          (runtime-output-recovery-failures runtime) nil)
    (setf (runtime-reads-cache runtime) nil)
    (install-watches runtime watches)
    (install-timers runtime timers)
    (install-executions runtime executions)
    (install-managed-processes runtime processes)
    (configure-virtual-outputs backend (getf old :virtual-outputs) (getf context :virtual-outputs))
    (setf (runtime-pending-spawns runtime) pending-spawns)
    (when (runtime-json-server runtime) (publish-json-context runtime context mounts))
    (remember-layout runtime mounts context)
    spawns))

(defun mounted-reads (runtime)
  "The context keys the mounted policy reacts to, cached until a commit, which is
the only thing that changes the mounts or the effects they own."
  (or (runtime-reads-cache runtime)
      (setf (runtime-reads-cache runtime)
            (remove-duplicates
             (loop for mounted in (append (root-mounts (runtime-mounts runtime))
                                          (rule-instances (runtime-mounts runtime)))
                   append (mount-context-reads mounted))))))

(defun event-matters-p (runtime changed force)
  "Whether a transaction could differ from the committed context. An event whose
keys no mounted policy reads, with no forced owner and nothing pending, cannot
change anything, so staging a candidate context would rebuild it for nothing."
  (or force
      (runtime-pending-context runtime)
      (and changed (intersection changed (mounted-reads runtime)))
      (loop for mounted in (rule-instances (runtime-mounts runtime))
            thereis (mounted-rule-pending mounted))))

(defun transact (runtime mounts event changed &optional force)
  (let ((*command-input* (and (getf event :input) t)))
    (unwind-protect (with-sample ("transaction")
                      (%transact runtime mounts event changed force))
      (discard-ui-asset-scratch runtime))))

(defun discard-ui-asset-scratch (runtime)
  (when (runtime-backend runtime)
    (%ui-assets-discard (runtime-backend runtime))))

(defun %transact (runtime mounts event changed &optional force)
  "Stage reducers and dependency propagation before touching the native scene.
A bad callback or a dependency cycle discards the entire candidate transaction.
Pending external invalidations join the current event, never an event replay."
  (let* ((*materialized* (list nil -1 nil nil))
         (candidate (with-sample ("tx-mounts")
                      (prune-rule-mounts
                       runtime (mapcar #'copy-mounted
                                       (append (root-mounts mounts)
                                               (rule-instances (runtime-mounts runtime)))))))
         (context (with-sample ("tx-material") (materialize runtime candidate)))
         (dirty (with-sample ("tx-dirty")
                  (union (runtime-pending-context runtime)
                         (union changed (changed-keys (runtime-effective runtime) context)))))
         (commands nil)
         (deferred-deliveries (make-hash-table :test #'eq))
         (proposal-baselines nil)
         (proposal-start nil)
         (proposal-deliveries (make-hash-table :test #'eq))
         (initial-event event)
         (initial-force force))
    (labels ((baseline (mounted)
               "The private state an admission re-run starts from: the state this
mount had when the transaction began, or the declaration's state for a rule
instance that reconciliation created inside it."
               (let ((spec (mounted-spec mounted)))
                 (unless proposal-baselines
                   (setf proposal-baselines (make-hash-table :test #'eq)
                         proposal-start (make-hash-table :test #'eq))
                   (dolist (run candidate)
                     (setf (gethash (mounted-spec run) proposal-start) t)))
                 (multiple-value-bind (baseline captured) (gethash spec proposal-baselines)
                   (if captured baseline
                       (setf (gethash spec proposal-baselines)
                             (if (gethash spec proposal-start)
                                 (if (and (not (mounted-rule-parent mounted)) (spec-immutable-p spec))
                                     (mounted-state mounted)
                                     (copy-data (mounted-state mounted)))
                                 (sixth (effect-arguments (mounted-rule-definition mounted)))))))))
             (invoke (mounted delivered)
               (when *speculating*
                 (unless (and (not (mounted-rule-parent mounted))
                              (spec-precomputable-p (mounted-spec mounted)))
                   (throw 'speculation-aborted nil)))
               (with-sample ("tx-invoke")
                (when (admission-scope-p mounted candidate)
                 (let* ((spec (mounted-spec mounted))
                        (shared (and (not (mounted-rule-parent mounted)) (spec-immutable-p spec))))
                   (setf (mounted-state mounted)
                         (if shared (baseline mounted) (copy-data (baseline mounted))))
                   (multiple-value-bind (original delivered-p) (gethash spec proposal-deliveries)
                     (if delivered-p (setf delivered original)
                         (setf (gethash spec proposal-deliveries)
                               (if shared delivered (copy-data delivered)))))
                   (setf commands (delete spec commands :key #'car))))
               (setf commands
                     (nconc commands
                            (mapcar (lambda (command) (cons (mounted-spec mounted) command))
                                    (invoke-extension runtime mounted context delivered)))))))
      (loop for round below 16 do
        (let ((before context) (invoked nil))
          (dolist (mounted (root-mounts candidate))
            (when (and (not (spec-admission (mounted-spec mounted)))
                       (or (member (spec-name (mounted-spec mounted)) force :test #'equal)
                           (intersection dirty (mount-context-reads mounted))))
              (invoke mounted
                      (if (and (member (getf event :type) '(:timer :watch :exec :ipc :ui))
                               (not (member (spec-name (mounted-spec mounted)) force :test #'equal)))
                          (list :type :change :keys dirty) event))
              (push (mounted-spec mounted) invoked)))
          (setf context (materialize runtime candidate))
          (multiple-value-bind (reconciled new revised)
              (with-sample ("tx-reconcile")
                (reconcile-rule-mounts runtime candidate context))
            (setf candidate reconciled context (materialize runtime reconciled))
            (let ((child-dirty (union dirty (changed-keys before context))))
              (dolist (mounted (rule-instances candidate))
                (let* ((name (spec-name (mounted-spec mounted)))
                       (parent (find (mounted-rule-parent mounted) candidate :key #'mounted-spec))
                       (declaration (and parent (find-rule-declaration parent (mounted-rule-name mounted)))))
                  (when (or (mounted-rule-pending mounted)
                            (member name (append force new revised) :test #'equal)
                            (member (mounted-rule-parent mounted) invoked)
                            (intersection child-dirty (mount-context-reads mounted)))
                    (let* ((spec (mounted-spec mounted))
                           (delivery
                             (or (gethash spec deferred-deliveries)
                                 (cond ((mounted-rule-pending mounted) '(:type :mount))
                                       ((or (and (member (getf event :type) '(:command :timer :watch :exec :ipc :ui))
                                                 (not (member name force :test #'equal)))
                                            (and (member (getf event :type) '(:button :pointer :request))
                                                 (not (eql (getf event :id) (mounted-rule-window mounted)))))
                                        (list :type :change :keys child-dirty))
                                       (t event)))))
                      (if (eq declaration (mounted-rule-definition mounted))
                          (progn
                            (remhash spec deferred-deliveries)
                            (invoke mounted delivery)
                            (push spec invoked))
                          (setf (gethash spec deferred-deliveries) delivery))))))))
          (setf context (materialize runtime candidate))
          (multiple-value-bind (reconciled new revised)
              (reconcile-rule-mounts runtime candidate context)
            (setf candidate reconciled
                  force (union new revised :test #'equal)))
          (setf context (materialize runtime candidate))
          (dolist (mounted (root-mounts candidate))
            (let ((spec (mounted-spec mounted)))
              (when (and (spec-admission spec)
                         (or (and (zerop round) (member (spec-name spec) initial-force :test #'equal))
                             (intersection (union dirty (changed-keys before context))
                                           (mount-context-reads mounted))))
                (invoke mounted
                        (if (and (member (getf initial-event :type) '(:timer :watch :exec :ipc :ui))
                                 (not (member (spec-name spec) initial-force :test #'equal)))
                            (list :type :change :keys dirty) initial-event))
                (setf context (materialize runtime candidate)))))
          (multiple-value-bind (reconciled new revised)
              (reconcile-rule-mounts runtime candidate context)
            (setf candidate reconciled force (union force (union new revised :test #'equal) :test #'equal)))
          (multiple-value-bind (next changes)
              (with-sample ("tx-diff")
                (let ((materialized (materialize runtime candidate)))
                  (values materialized (changed-keys before materialized))))
            (setf context next dirty changes event (list :type :change :keys changes))
            (unless (or changes force)
              (when *speculating*
                (setf *speculation* (list candidate context commands))
                (return-from %transact t))
              (commit-call-resolution runtime context)
              (let* ((commands (loop for (owner . command) in commands
                                     when (find owner candidate :key #'mounted-spec) collect command))
                     (spawns (with-sample ("tx-commit")
                               (commit-context runtime candidate context commands))))
                (setf (runtime-pending-context runtime) nil)
                (unwind-protect
                     (loop for command in commands for spawn in spawns do
                       (when (runtime-running runtime) (execute-command runtime command spawn)))
                  (setf (runtime-pending-spawns runtime)
                        (delete-if (lambda (job) (member job spawns :test #'eq))
                                   (runtime-pending-spawns runtime)))))
              (return-from %transact t)))))
      (error "Extension dependencies did not settle within 16 rounds."))))

(defvar *stamp-buffer* (make-array 0 :element-type '(unsigned-byte 8)))

(defun source-stamp (path)
  "Stamp PATH as (write-date length digest), or nil when it is missing or unreadable.
The write date resolves to one second, so a same-length edit inside the second a
source was loaded would otherwise be invisible. Extension files are small and
stay in the page cache, so the digest decides. It is FNV-1a over the octets, read
into one reused buffer, so watching allocates nothing per file."
  (handler-case
      (with-open-file (stream path :if-does-not-exist nil :element-type '(unsigned-byte 8))
        (when stream
          (let ((size (min (file-length stream) 1048576)))
            (when (< (length *stamp-buffer*) size)
              (setf *stamp-buffer* (make-array size :element-type '(unsigned-byte 8))))
            (let* ((buffer *stamp-buffer*)
                   (count (read-sequence buffer stream :end size))
                   (hash 14695981039346656037))
              (declare (type (simple-array (unsigned-byte 8) (*)) buffer)
                       (type (unsigned-byte 64) hash) (type fixnum count))
              (dotimes (i count)
                (setf hash (ldb (byte 64 0) (* (logxor hash (aref buffer i)) 1099511628211))))
              (list (file-write-date stream) (file-length stream) count hash)))))
    (serious-condition () nil)))

(defun source-path (path)
  (let ((pathname (merge-pathnames path)))
    (unless (probe-file pathname) (error "No extension source at ~A." path))
    (namestring pathname)))

(defun undefined-references ()
  (loop for entry in sb-c::*undefined-warnings*
        for name = (sb-c::undefined-warning-name entry)
        when (case (sb-c::undefined-warning-kind entry)
               (:function (not (fboundp name)))
               (:variable (not (boundp name))))
          collect name))

(defun fresh-source-package (&optional name)
  "A package for one loaded source, with the TOMOE-USER local nickname."
  (let ((package (make-package (or name (string (gensym "TOMOE-USER/"))) :use '(:cl :tomoe))))
    (sb-ext:add-package-local-nickname "TOMOE-USER" package package)
    package))


(defparameter +pure-special-operators+
  '(quote function progn let let* flet labels macrolet symbol-macrolet if block return-from
    tagbody go the locally multiple-value-call multiple-value-prog1 unwind-protect catch throw
    sb-ext:truly-the sb-kernel:the*))

(defparameter +pure-functions+
  (let ((table (make-hash-table :test #'eq)))
    (dolist (sym '(+ - * / = /= < <= > >= 1+ 1- max min mod rem floor ceiling truncate round
                   ffloor fceiling ftruncate fround abs signum minusp plusp zerop evenp oddp
                   logand logior logxor logeqv lognand lognor logandc1 logandc2 logorc1 logorc2
                   lognot logtest logbitp logcount integer-length ash ldb dpb byte byte-size
                   byte-position expt sqrt exp log sin cos tan asin acos atan sinh cosh tanh
                   isqrt numerator denominator rational rationalize float coerce
                   eq eql equal equalp not null atom consp listp numberp integerp rationalp
                   floatp realp stringp characterp symbolp keywordp typep subtypep endp
                   char= char/= char< char<= char> char>= char-equal char-not-equal
                   alpha-char-p alphanumericp digit-char-p upper-case-p lower-case-p
                   graphic-char-p standard-char-p
                   string= string/= string< string<= string> string>= string-equal
                   string-not-equal string-lessp string-greaterp
                   car cdr caar cadr cdar cddr caaar caadr cadar caddr cdaar cdadr cddar cdddr
                   caaaar caaadr caadar caaddr cadaar cadadr caddar cadddr cdaaar cdaadr cdadar
                   cdaddr cddaar cddadr cdddar cddddr first second third fourth fifth sixth
                   seventh eighth ninth tenth rest last butlast nth nthcdr elt length
                   cons list list* acons pairlis copy-list copy-alist copy-tree copy-seq
                   make-list append revappend reverse subseq getf get-properties
                   assoc assoc-if assoc-if-not rassoc member member-if member-if-not tailp ldiff
                   subsetp subst subst-if subst-if-not sublis
                   find find-if find-if-not position position-if position-if-not
                   count count-if count-if-not remove remove-if remove-if-not remove-duplicates
                   substitute substitute-if substitute-if-not
                   set-difference set-exclusive-or union intersection mismatch search
                   mapcar mapc maplist mapl every some notany notevery reduce
                   char schar code-char char-code char-int char-upcase char-downcase digit-char
                   string make-string concatenate string-upcase string-downcase string-capitalize
                   string-trim string-left-trim string-right-trim parse-integer
                   adjoin princ-to-string prin1-to-string write-to-string symbol-name intern
                   values values-list identity constantly complement error
                   sb-c::check-ds-list sb-kernel:check-type-error-trap sb-kernel:case-failure
                   sb-kernel:ecase-failure sb-kernel:etypecase-failure sb-impl::xsubtract
                   sb-impl::copy-list-to sb-kernel:unaligned-dx-cons
                   context previous-context state-value service-state rules-for window-geometry
                   setting theme bar-layout workspaces-widget place focus raise-window
                   show-window hide-window fullscreen maximize grab set-view settings
                   motion-contract window-properties configure-output virtual-output
                   configure-keyboard layer bind-key bind-button bind-scroll keyboard-grab sound
                   define-command invoke publish-state serve-state serve-method announce
                   broadcast ipc-reply window-rule ui shell-surface confirm-dialog menu-dialog
                   menu-choice sheet-dialog toast once interval watch-file exec-async run-once
                   service launch spawn close-window screenshot clipboard-copy media-control
                   adjust-brightness screencast-answer output-power quit reload
                   json-object json-array json-get copy-data))
      (setf (gethash sym table) t))
    table))

(defparameter +clock-read-functions+
  '(clock-text clock-boundary-ms frame-ms get-internal-real-time get-internal-run-time
    get-universal-time get-decoded-time))

(defparameter +local-mutation-operators+
  '(rplaca rplacd sb-kernel:%rplaca sb-kernel:%rplacd sb-impl::%putf
    nreverse nreconc nconc sort stable-sort delete delete-if delete-if-not
    delete-duplicates nsubstitute nsubst))

(defparameter +designator-positions+
  (let ((table (make-hash-table :test #'eq)))
    (dolist (entry '((mapcar 0) (mapc 0) (maplist 0) (mapl 0) (mapcan 0) (mapcon 0)
                     (every 0) (some 0) (notany 0) (notevery 0) (reduce 0)
                     (remove-if 0) (remove-if-not 0) (delete-if 0) (delete-if-not 0)
                     (find-if 0) (find-if-not 0) (position-if 0) (position-if-not 0)
                     (count-if 0) (count-if-not 0) (assoc-if 0) (assoc-if-not 0)
                     (member-if 0) (member-if-not 0) (rassoc-if 0) (rassoc-if-not 0)
                     (subst-if 0) (subst-if-not 0) (substitute-if 0) (substitute-if-not 0)
                     (complement 0) (merge 3) (sort 1) (stable-sort 1)))
      (setf (gethash (first entry) table) (second entry)))
    table)
  "Functions that call one of their arguments, with the zero-based index of the
argument they call. A designator the walker cannot resolve to a verified
function counts as a mutation, so the reducer keeps its private copies.")

(defparameter +designator-keywords+ '(:test :test-not :key)
  "Keyword arguments that name a function a sequence function will call.")

(defun fresh-copy-spine-form-p (form)
  (and (consp form)
       (or (eq (car form) 'copy-list)
           (and (member (car form) '(sort stable-sort))
                (fresh-copy-spine-form-p (second form))))))

(defun callable-arg-safe-p (arg)
  (and (consp arg) (member (car arg) '(function lambda))))

(defun designator-arguments-safe-p (op args check)
  "Whether every function a call through OP will call is a verified function.
CHECK resolves one designator form, returning NIL when it cannot."
  (let ((position (gethash op +designator-positions+)))
    (and (or (null position) (funcall check (nth position args)))
         (loop for tail on args
               never (and (keywordp (car tail))
                          (member (car tail) +designator-keywords+)
                          (not (funcall check (second tail))))))))

(defun form-verified-p (form mode defuns constants &optional seen)
  (let ((ok t)
        (local-fns nil)
        (dx-tails (make-hash-table :test #'eq)))
    (labels ((helper-safe-p (sym)
               (cond ((member sym seen :test #'eq) t)
                     ((gethash sym defuns)
                      (form-verified-p (gethash sym defuns) mode defuns constants (cons sym seen)))
                     ((eq mode :passive) t)
                     (t nil)))
             (operator-safe-p (sym)
               (or (member sym local-fns :test #'eq)
                   (gethash sym +pure-functions+)
                   (and (eq mode :immutable) (member sym +clock-read-functions+ :test #'eq))
                   (and (member mode '(:precompute :passive))
                        (member sym +local-mutation-operators+ :test #'eq))
                   (helper-safe-p sym)))
             (designator-safe-p (arg)
               "Whether ARG, evaluated where the callee will call it, is a function
this form may call. A quoted symbol nothing has defined is data. Anything the
walker cannot resolve counts as unverified: a designator reached through a
variable, an INTERN or a computed name can name any function."
               (cond
                 ((and (consp arg) (eq (car arg) 'function))
                  (let ((fn (second arg)))
                    (cond ((symbolp fn) (operator-safe-p fn))
                          ((and (consp fn) (eq (car fn) 'lambda)) t)
                          (t nil))))
                 ((and (consp arg) (eq (car arg) 'lambda)) t)
                 ((and (consp arg) (eq (car arg) 'quote))
                  (let ((value (second arg)))
                    (and (symbolp value)
                         (not (keywordp value))
                         (or (not (fboundp value)) (operator-safe-p value)))))
                 (t nil))))
      (handler-case
          (let ((sb-walker:*walk-form-expand-macros-p* t))
            (sb-walker:walk-form
             form nil
             (lambda (sub ctx env)
               (unless ok (return-from form-verified-p nil))
               (when (consp sub)
                 (when (and (eq (car sub) 'let*) (consp (second sub)) (= 2 (length (second sub))))
                   (let ((b1 (first (second sub))) (b2 (second (second sub))))
                     (when (and (consp b1) (consp b2)
                                (symbolp (first b1)) (null (symbol-package (first b1)))
                                (symbolp (first b2)) (null (symbol-package (first b2)))
                                (eq (second b2) (first b1))
                                (consp (second b1))
                                (or (equal (second b1) '(sb-kernel:unaligned-dx-cons nil))
                                    (equal (second b1) '(list nil))))
                       (setf (gethash (first b2) dx-tails) t))))
                 (when (and (member (car sub) '(flet labels)) (listp (second sub)))
                   (dolist (def (second sub))
                     (when (and (consp def) (symbolp (car def)))
                       (pushnew (car def) local-fns :test #'eq)))))
               (when (eq ctx :eval)
                 (cond
                   ((and (symbolp sub) (not (keywordp sub)) (not (constantp sub)))
                    (unless (or (and (sb-walker:var-lexical-p sub env)
                                     (not (sb-walker:var-special-p sub env)))
                                (gethash sub constants))
                      (setf ok nil)))
                   ((and (consp sub) (symbolp (car sub)) (not (macro-function (car sub) env)))
                    (let ((op (car sub)) (args (cdr sub)))
                      (cond
                        ((eq op 'setq)
                         (loop for (var val) on args by #'cddr do
                           (unless (and (symbolp var)
                                        (sb-walker:var-lexical-p var env)
                                        (not (sb-walker:var-special-p var env)))
                             (setf ok nil))
                           (when (and (gethash var dx-tails)
                                      (not (and (consp val)
                                                (member (car val) '(list cdr last sb-impl::copy-list-to)))))
                             (setf ok nil))))
                        ((eq op 'function)
                         (let ((fn (first args)))
                           (when (symbolp fn)
                             (unless (operator-safe-p fn) (setf ok nil)))))
                        ((member op +pure-special-operators+ :test #'eq)
                         (when (member op '(let let*))
                           (dolist (binding (first args))
                             (let ((var (if (consp binding) (car binding) binding)))
                               (when (and (symbolp var) (sb-walker:var-special-p var env))
                                 (setf ok nil))))))
                        ((and (member op '(rplacd sb-kernel:%rplacd)) (gethash (first args) dx-tails))
                         nil)
                        ((member op '(sort stable-sort))
                         (unless (fresh-copy-spine-form-p (first args)) (setf ok nil)))
                        ((eq op 'format)
                         (unless (null (first args)) (setf ok nil)))
                        ((member op '(funcall apply))
                         (unless (or (callable-arg-safe-p (first args))
                                     (eq mode :passive))
                           (setf ok nil)))
                        ((not (operator-safe-p op))
                         (setf ok nil)))
                      (unless (member op +pure-special-operators+ :test #'eq)
                        (unless (designator-arguments-safe-p op args #'designator-safe-p)
                          (setf ok nil)))))
                   ((and (consp sub) (not (symbolp (car sub)))
                         (not (and (consp (car sub)) (eq (caar sub) 'lambda))))
                    (setf ok nil))))
               sub)))
        (serious-condition () (setf ok nil)))
      ok)))

(defun collect-mutated-symbols (form into)
  (when (consp form)
    (when (and (listp (cdr form)) (member (car form) '(setq setf incf decf push pop)) (cdr form))
      (loop for tail = (cdr form) then (if (member (car form) '(setq setf))
                                           (and (consp (cdr tail)) (cddr tail))
                                           (cdr tail))
            while (consp tail)
            when (symbolp (car tail)) do (setf (gethash (car tail) into) t)))
    (loop for tail = form then (cdr tail)
          while (consp tail) do (collect-mutated-symbols (car tail) into)
          finally (when tail (collect-mutated-symbols tail into)))))

(defun analyze-source-specs (path package specs)
  (let ((forms nil)
        (defuns (make-hash-table :test #'eq))
        (defun-counts (make-hash-table :test #'eq))
        (params nil)
        (mutated (make-hash-table :test #'eq))
        (constants (make-hash-table :test #'eq)))
    (handler-case
        (let ((*package* package) (*read-eval* nil) (*read-default-float-format* 'double-float))
          (with-open-file (stream path)
            (loop for form = (read stream nil :eof) until (eq form :eof) do
              (if (and (consp form) (eq (car form) 'in-package) (second form))
                  (let ((found (find-package (second form))))
                    (when found (setf *package* found)))
                  (push form forms)))))
      (serious-condition () (return-from analyze-source-specs specs)))
    (setf forms (nreverse forms))
    (dolist (form forms)
      (collect-mutated-symbols form mutated)
      (when (consp form)
        (cond
          ((and (eq (car form) 'defun) (symbolp (second form)) (listp (third form)))
           (incf (gethash (second form) defun-counts 0))
           (setf (gethash (second form) defuns) (list* 'lambda (third form) (cdddr form))))
          ((and (eq (car form) 'defparameter) (symbolp (second form)))
           (push (cons (second form) (third form)) params)))))
    (maphash (lambda (sym count)
               (when (or (> count 1) (gethash sym mutated)) (remhash sym defuns)))
             defun-counts)
    (dolist (entry (nreverse params))
      (let ((sym (car entry)) (init (cdr entry)))
        (when (and (not (gethash sym mutated))
                   (form-verified-p init :precompute defuns constants))
          (setf (gethash sym constants) t))))
    (dolist (spec specs specs)
      (when (spec-form spec)
        (when (form-verified-p (spec-form spec) :immutable defuns constants)
          (setf (spec-immutable-generation spec) *patch-generation*))
        (when (spec-precompute spec)
          (let ((mode (if (eq (spec-precompute spec) :passive) :passive :precompute)))
            (if (form-verified-p (spec-form spec) mode defuns constants)
                (setf (spec-precompute-generation spec) *patch-generation*)
                (setf (spec-precompute spec) nil))))))))
(defun load-specs (runtime path &optional package)
  (incf *definitions-generation*)
  (let* ((*source* path)
         (directory (directory-namestring (truename path)))
         (*source-directory* directory)
         (package (or package (fresh-source-package)))
         (*definitions* nil) (*package* package) (*read-eval* nil)
         (*read-default-float-format* 'double-float))
    (unless (member package (runtime-source-packages runtime))
      (push package (runtime-source-packages runtime)))
    (sb-ext:with-timeout 1
      (with-open-file (stream path)
        (when (> (file-length stream) 1048576) (error "Extension file exceeds 1 MiB."))
        (with-compilation-unit (:override t)
          (load stream :verbose nil :print nil)
          (let ((undefined (undefined-references)))
            (when undefined
              (error "~A refers to undefined ~{~(~A~)~^, ~}." path undefined))))))
    (let ((specs (loop for spec in (nreverse *definitions*)
                       do (setf (spec-directory spec) directory (spec-package spec) package)
                       collect spec)))
      (analyze-source-specs path package specs))))

(defun sweep-source-packages (runtime)
  (let ((live (mapcar (lambda (mounted) (spec-package (mounted-spec mounted)))
                      (runtime-mounts runtime))))
    (setf (runtime-source-packages runtime)
          (loop for package in (runtime-source-packages runtime)
                if (member package live) collect package
                  else do (delete-package package)))))

(defun configure (runtime sources &optional changed-source)
  (unwind-protect
      (let* ((specs (loop for path in sources
                          when (or (null changed-source) (equal path changed-source)) append (load-specs runtime path)))
             (retained (when changed-source
                         (remove changed-source (root-mounts (runtime-mounts runtime)) :test #'equal
                                 :key (lambda (m) (spec-source (mounted-spec m))))))
             (names (make-hash-table :test #'equal)))
        (dolist (spec (append (mapcar #'mounted-spec retained) specs))
          (when (gethash (spec-name spec) names) (error "Duplicate extension: ~A" (spec-name spec)))
          (setf (gethash (spec-name spec) names) t))
        (let ((mounts
                (loop for spec in specs
                      for previous = (find (spec-name spec) (runtime-mounts runtime)
                                           :test #'equal :key (lambda (m) (spec-name (mounted-spec m))))
                      collect (if (and previous (equal (spec-source spec) (spec-source (mounted-spec previous))))
                                  (let ((copy (copy-mounted previous))) (setf (mounted-spec copy) spec) copy)
                                  (make-mounted :spec spec :state (copy-data (spec-initial spec))))))
              (patched (remove-if-not (lambda (mounted) (spec-patch (mounted-spec mounted)))
                                      (root-mounts (runtime-mounts runtime)))))
          (transact runtime
                    (append (loop for source in sources append
                              (remove source (append retained mounts) :test-not #'equal
                                      :key (lambda (m) (spec-source (mounted-spec m)))))
                            patched)
                    '(:type :mount) nil (mapcar #'spec-name specs)))
        (setf (runtime-sources runtime) (copy-list sources) (runtime-last-error runtime) nil
              (runtime-source-stamps runtime)
              (loop for path in sources collect (cons path (source-stamp path))))
        (incf (runtime-generation runtime)))
    (sweep-source-packages runtime)))

(defun unmount (runtime name)
  (unless (find name (root-mounts (runtime-mounts runtime)) :test #'equal
                :key (lambda (m) (spec-name (mounted-spec m))))
    (error "No mounted extension named ~A." name))
  (unwind-protect
      (transact runtime
                (remove name (runtime-mounts runtime) :test #'equal
                        :key (lambda (m) (spec-name (mounted-spec m))))
                (list :type :unmount :name name) nil)
    (sweep-source-packages runtime))
  (incf (runtime-generation runtime)))

(defun execute-command (runtime command &optional spawn)
  (ecase (command-kind command)
    (:reply
     (setf (getf (runtime-ipc-call runtime) :result) (first (command-arguments command))
           (getf (runtime-ipc-call runtime) :replied) t))
    (:broadcast
     (when (runtime-json-server runtime)
       (apply #'json-broadcast (runtime-json-server runtime) (command-arguments command))))
    ((:launch :spawn)
     (unless spawn (error "Session spawn was not reserved before publication."))
     (start-managed-process runtime spawn))
    (:close (%close (runtime-backend runtime) (first (command-arguments command))))
    (:screencast (apply #'answer-screencast runtime (command-arguments command)))
    (:screenshot (%screenshot (runtime-backend runtime) (if (first (command-arguments command)) 0 1)))
    (:media (control-mpris runtime (first (command-arguments command))))
    (:clipboard (destructuring-bind (path type delete) (command-arguments command)
                  (unless (= 1 (%clipboard-copy (runtime-backend runtime) path type (if delete 1 0)))
                    (error "Cannot copy ~A to the clipboard." path))))
    (:brightness (adjust-backlight (first (command-arguments command))))
    (:power (destructuring-bind (mode name) (command-arguments command)
              (%output-power (runtime-backend runtime) name
                             (ecase mode (:off 0) (:on 1) (:toggle 2)))))
    (:quit (setf (runtime-running runtime) nil))
    (:reload (configure runtime (runtime-sources runtime)))))

(defun record-error (runtime condition)
  (setf (runtime-last-error runtime) (princ-to-string condition))
  (format *error-output* "tomoe: ~A~%" condition))

(defun report-config-error (runtime condition fallback)
  "Record a failed source load and publish it as :CONFIG-ERROR."
  (record-error runtime condition)
  (setf (runtime-config-error runtime)
        (list :serial (1+ (getf (runtime-config-error runtime) :serial 0))
              :message (if fallback
                           "Failed to load the config file. Running with defaults; check the log for details."
                           "Failed to load the config file. Keeping the running config; check the log for details.")))
  (handler-case (transact runtime (runtime-mounts runtime) '(:type :change :keys (:config-error))
                          '(:config-error))
    (serious-condition (failure) (record-error runtime failure))))

(defun reconcile-backend-observations (runtime)
  "Adopt queued external observations before selecting a private callback.
A presentation committed by one private callback can queue new layer facts.
Reconcile between callbacks, then recheck resource membership against the newly
accepted registry. Never enter this helper inside a candidate transaction."
  (let ((backend (runtime-backend runtime)))
    (when backend
      (loop while (and (runtime-running runtime) (not *stop-requested*)
                       (plusp (%event-barrier backend))) do
        (let ((text (%event backend)))
          (unless text (error "Backend observation fence outlived its event queue."))
          (dispatch-observation runtime text)))))
  runtime)

(defun dispatch-observation (runtime text)
  (unwind-protect
       (handler-case (dispatch-event runtime
                                     (if *timing*
                                         (with-sample ("parse") (read-data text))
                                         (read-data text)))
         (serious-condition (condition) (record-error runtime condition)))
    (when (and *timing* (runtime-backend runtime)) (%timing-done (runtime-backend runtime)))))

(defparameter *idle-seconds* 1)
(defparameter *idle-consed* (* 4 1024 1024))
(defparameter *nursery-bytes* (* 4 1024 1024))
(defvar *handback-log* (make-array 64 :initial-element nil)
  "The idle handbacks as (START END), in native timing nanoseconds, measured around the
collection itself. SBCL runs the after-gc hooks on its finalizer thread, which cannot see
the bindings of the thread that collected, so the handback records itself here instead.")
(defvar *handback-log-count* 0)
(defvar *test-handback-pause* nil
  "A test-only floor, in native timing nanoseconds, for the pause logged for the next idle
handback, so a check can prove that it rejects a window holding one handback that outlasts
a refresh.")
(defvar *test-handback-pause-floor* nil
  "The floor a check asked for, armed again for one handback by every timing reset.")

(defun hand-back-heap (generations)
  "Collect GENERATIONS for the idle handback, logging the pause the compositor took for it
apart from the collections the allocator starts on its own."
  (if *timing*
      (let ((start (%timing-now))
            (floor (prog1 *test-handback-pause* (setf *test-handback-pause* nil))))
        (sb-ext:gc :gen generations)
        (let ((end (max (%timing-now) (+ start (or floor 0)))))
          (setf (aref *handback-log* (mod *handback-log-count* (length *handback-log*)))
                (list start end))
          (incf *handback-log-count*)))
      (sb-ext:gc :gen generations)))

(defun service-memory (runtime)
  "Hand the heap back while the scene has been quiet for a second, once 4 MB was consed
since the last collection. The younger generations are swept first and the oldest one is
collected on the next pass, so neither pause outlasts a refresh and the loop can take
input between them. Collecting the oldest generation alone rather than the whole heap
still hands the aged garbage's pages back to the system, so an idle desktop keeps only
what it is using, and leaves the younger generations to the allocator's own collections.
The native heap is trimmed once when the scene falls quiet, which returns the pages a
burst of shader compiles, captures or decoded images grew without a collection running."
  (let* ((native (runtime-backend runtime))
         (now (get-internal-real-time))
         (serial (%activity-serial native))
         (quiet (>= (- now (runtime-idle-since runtime))
                    (* *idle-seconds* internal-time-units-per-second))))
    (cond
      ((/= serial (runtime-activity-serial runtime))
       (setf (runtime-activity-serial runtime) serial
             (runtime-idle-since runtime) now
             (runtime-idle-trimmed runtime) nil
             (runtime-idle-swept runtime) nil))
      ((and quiet (runtime-idle-swept runtime))
       (hand-back-heap 2)
       (setf (runtime-idle-swept runtime) nil
             (runtime-idle-trimmed runtime) t
             (runtime-idle-consed runtime) (sb-ext:get-bytes-consed)
             (runtime-idle-since runtime) (get-internal-real-time)))
      ((and quiet (>= (- (sb-ext:get-bytes-consed) (runtime-idle-consed runtime)) *idle-consed*))
       (unless (runtime-idle-trimmed runtime)
         (setf (runtime-idle-trimmed runtime) t)
         (%trim-heap))
       (hand-back-heap 1)
       (setf (runtime-idle-swept runtime) t))
      ((and quiet (not (runtime-idle-trimmed runtime)))
       (setf (runtime-idle-trimmed runtime) t)
       (%trim-heap)))))

(defun memory-wait-milliseconds (runtime)
  "How long the backend may sleep before the idle collection is due."
  (let ((now (get-internal-real-time)))
    (when (>= (- (sb-ext:get-bytes-consed) (runtime-idle-consed runtime)) *idle-consed*)
      (wait-until now (+ (runtime-idle-since runtime)
                         (* *idle-seconds* internal-time-units-per-second))))))

(defun producer-watch (native descriptor mask)
  (when native
    (let ((fd (funcall descriptor native)))
      (when (>= fd 0) (list fd (funcall mask native) native)))))

(defun window-identifier (runtime id)
  (let ((backend (runtime-backend runtime)))
    (and backend (%window-identifier backend id))))


;;; Keybinding oracle.
;;;
;;; A key runs a policy transaction. When the binding's owner has declared that
;;; its reducer is safe to evaluate ahead of its event, the loop can run that
;;; transaction before the key arrives, keep the state it settled on, and hand
;;; the native side the presentation it would have published. The key then
;;; moves the screen from the input handler, and Lisp adopts the state it
;;; already computed.
;;;
;;; Every guard here exists so a delta can never land state the committed
;;; context disagrees with. A delta is keyed to the commit it was computed
;;; from, the native side checks the focus it staged against the focus it
;;; holds, and a binding without a fresh preparation runs the ordinary path.

(defparameter +oracle-fixed-keys+
  '(:outputs :connectors :output-config :native-output-config :layers :surface-plans
    :settings :sounds :keyboard :keyboard-grab :bindings :virtual-outputs)
  "Context keys the native side publishes, other than the view, focus, window
geometry and stacking a delta carries. A transaction that changes any of them is
published the ordinary way. Everything else a context holds is Lisp's alone, so
an adopted transaction may change it freely.")

(defun motion-plan-for (runtime mounts)
  (multiple-value-bind (grab details) (resolved-grab runtime mounts)
    (resolved-motion-plan mounts grab details)))

(defun same-windows-p (before after)
  "Whether two layouts describe the same windows with the same properties.
Geometry, visibility, fullscreen and maximize are what a delta carries, so a
window that appeared, went away or changed a property is published the
ordinary way."
  (and (= (length before) (length after))
       (every (lambda (window)
                (let ((after (find (getf window :id) after :key (lambda (one) (getf one :id)))))
                  (and after (equal (getf window :properties) (getf after :properties)))))
              before)))

(defun presentation-only-p (runtime mounts context)
  "Whether CONTEXT differs from the committed one only in what a delta carries."
  (and (null (remove-if (lambda (key) (equal (getf (runtime-effective runtime) key)
                                             (getf context key)))
                        +oracle-fixed-keys+))
       (same-windows-p (getf (runtime-effective runtime) :layout) (getf context :layout))
       (equal (runtime-motion-plan runtime) (motion-plan-for runtime mounts))))

(defun key-transaction (runtime event)
  "What a :KEY event does, as (:TYPE :TRANSACTION :EVENT :CHANGED :FORCE), or
(:TYPE :HOST :ENTRY :ARGUMENTS) when it runs a host command, or NIL when it
names nothing this session owns."
  (when (getf event :binding-id)
    (let* ((owner (find (getf event :owner) (runtime-mounts runtime) :test #'equal
                        :key (lambda (mount) (spec-name (mounted-spec mount)))))
           (source-id (and owner (spec-id (mounted-spec owner)))))
      (unless (and source-id (eql source-id (getf event :source-id))
                   (runtime-backend runtime)
                   (typep (getf event :binding-id) '(integer 1 18446744073709551615))
                   (= 1 (%binding-current (runtime-backend runtime) (getf event :binding-id))))
        (return-from key-transaction nil))))
  (let ((call (keyboard-call runtime event)))
    (cond
      ((null call) nil)
      ((eq (first call) :callback)
       (list :type :transaction :event (callback-event event (second call))
             :changed '(:command) :force (list (getf event :owner))))
      (t (let ((entry (registered-command (getf (runtime-effective runtime) :commands)
                                          (first call))))
           (unless entry (return-from key-transaction nil))
           (if (getf entry :host)
               (list :type :host :entry entry :arguments (rest call))
               (list :type :transaction
                     :event (command-event runtime entry (first call) (rest call) event)
                     :changed '(:command) :force (list (getf entry :owner)))))))))

(defun run-host-key (runtime dispatch)
  (let ((*command-input* t))
    (handler-case (host-command-call runtime (getf dispatch :entry) (getf dispatch :arguments))
      (serious-condition (condition) (record-error runtime condition)))))

(defun oracle-binding (runtime key)
  "The binding (:OWNER :COMMAND) names, or NIL once it is gone."
  (find-if (lambda (binding)
             (and (stringp (getf binding :command))
                  (equal (getf binding :owner) (first key))
                  (equal (getf binding :command) (second key))))
           (getf (runtime-effective runtime) :bindings)))

(defun precomputable-binding-p (runtime binding)
  (let ((mount (find (getf binding :source-id) (root-mounts (runtime-mounts runtime))
                     :key (lambda (mounted) (spec-id (mounted-spec mounted))))))
    (and mount (spec-precomputable-p (mounted-spec mount)))))

(defun oracle-note-key (runtime event)
  "Remember the binding EVENT ran, so its next transition can be staged. A key
that names no binding of its own, such as the release of one, leaves the
remembered binding alone."
  (let ((binding (and (getf event :owner) (getf event :command)
                      (oracle-binding runtime (list (getf event :owner) (getf event :command))))))
    (when (and binding (precomputable-binding-p runtime binding))
      (setf (runtime-oracle-mru runtime)
            (list (getf binding :owner) (getf binding :command))))))

(defun oracle-key-event (backend binding)
  "The :KEY event the native side emits for BINDING's press, or NIL when the
native side does not have that binding."
  (let ((id (%oracle-binding-id backend (getf binding :modifiers) (getf binding :code)
                                (getf binding :owner) (getf binding :command)
                                (getf binding :source-id))))
    (when (and id (plusp id))
      (list :type :key :state :pressed :binding-id id
            :owner (getf binding :owner) :command (getf binding :command)
            :source-id (getf binding :source-id)))))

(defun speculation-clean-p (runtime candidate)
  "Whether the speculative transaction left every reducer it ran as healthy as
the committed one, so a reducer that failed is never published for a key."
  (every (lambda (mounted)
           (let ((base (find (spec-id (mounted-spec mounted))
                             (root-mounts (runtime-mounts runtime))
                             :key (lambda (one) (spec-id (mounted-spec one))))))
             (or (null base) (= (mounted-failures mounted) (mounted-failures base)))))
         (root-mounts candidate)))

(defun speculative-transaction (runtime dispatch)
  "Evaluate DISPATCH's transaction without committing it. NIL when a reducer
this session cannot evaluate ahead of its event would run."
  (catch 'speculation-aborted
    (let ((*speculating* t) (*speculation* nil))
      (unwind-protect
           (when (%transact runtime (runtime-mounts runtime) (getf dispatch :event)
                            (getf dispatch :changed) (getf dispatch :force))
             *speculation*)
        (discard-ui-asset-scratch runtime)))))

(defun stage-oracle-delta (backend binding-id epoch restack context)
  "Hand the native side the presentation CONTEXT would have published."
  (let ((view (getf context :view))
        (layout (getf context :layout))
        (stacking (getf context :stacking)))
    (unless (= 1 (%oracle-begin backend binding-id epoch +oracle-focus-unset+
                                (getf view :x) (getf view :y) (getf view :zoom)
                                (or (getf context :focus) 0) (if restack 1 0)
                                (length layout) (length stacking)))
      (return-from stage-oracle-delta nil))
    (dolist (window layout)
      (unless (= 1 (%oracle-window backend (getf window :id) (getf window :x) (getf window :y)
                                   (getf window :width) (getf window :height)
                                   (if (getf window :visible t) 1 0)
                                   (if (getf window :fullscreen) 1 0)
                                   (if (getf window :maximize) 1 0)))
        (return-from stage-oracle-delta nil)))
    (dolist (id stacking)
      (unless (= 1 (%oracle-stack backend id)) (return-from stage-oracle-delta nil)))
    (= 1 (%oracle-end backend))))

(defun prepare-oracle-deltas (runtime)
  "Stage the transition of the key pressed most recently, when its owner has
declared that transition safe to evaluate ahead of the event. Runs between
events, at most one speculative transaction per commit, and does nothing once
the committed state has been staged."
  (handler-case (prepare-oracle-delta runtime)
    (serious-condition (condition)
      ;; Evaluation ahead of the event is an optimisation: once it surprises
      ;; us, the session keeps the synchronous path for the rest of its life.
      (setf *oracle-enabled* nil)
      (record-error runtime condition))))

(defun prepare-oracle-delta (runtime)
  (let ((backend (runtime-backend runtime)))
    (when (and backend *oracle-enabled* (runtime-running runtime)
               (zerop (%event-count backend))
               (not (eql (runtime-oracle-ready runtime) (runtime-oracle-generation runtime))))
      (setf (runtime-oracle-ready runtime) (runtime-oracle-generation runtime))
      (let* ((binding (oracle-binding runtime (runtime-oracle-mru runtime)))
             (event (and binding (precomputable-binding-p runtime binding)
                         (oracle-key-event backend binding)))
             (dispatch (and event (key-transaction runtime event))))
        (when (and dispatch (eq (getf dispatch :type) :transaction))
          (let ((result (speculative-transaction runtime dispatch)))
            (when (and result (null (third result))
                       (speculation-clean-p runtime (first result))
                       (presentation-only-p runtime (first result) (second result)))
              (let* ((context (second result))
                     (base (runtime-effective runtime))
                     (restack (not (equal (getf base :stacking) (getf context :stacking))))
                     (epoch (incf (runtime-oracle-epoch runtime))))
                (when (stage-oracle-delta backend (getf event :binding-id) epoch restack context)
                  (incf (runtime-oracle-prepared runtime))
                  (setf (runtime-oracle-memo runtime)
                        (list :epoch epoch :generation (runtime-oracle-generation runtime)
                              :mounts (first result) :context context))
                  (%oracle-arm backend epoch))))))))))

(defun adopt-transaction (runtime memo)
  "Commit the transaction the native side has already applied. The context is
the one staged for this epoch, so nothing is published a second time: the
native presentation of that commit is already on screen."
  (let ((context (getf memo :context)) (mounts (getf memo :mounts)))
    (setf (runtime-oracle-memo runtime) nil)
    (commit-call-resolution runtime context)
    (commit-context runtime mounts context nil :native nil)
    (setf (runtime-pending-context runtime) nil)))

(defun dispatch-event (runtime event)
  (let ((changed nil) (force nil))
    (ecase (getf event :type)
      (:system
       (setf (runtime-system runtime) (getf event :system))
       (push :system changed))
      (:services
       (let ((services (copy-data (getf event :services))))
         (unless (equal services (runtime-services runtime))
           (setf (runtime-services runtime) services)
           (push :services changed))))
      (:ui
       (let ((owner (find (getf event :owner) (runtime-mounts runtime)
                          :test #'equal :key (lambda (mounted) (spec-name (mounted-spec mounted))))))
         (unless (and owner (eql (getf event :source-id) (spec-id (mounted-spec owner)))
                      (runtime-backend runtime)
                      (typep (getf event :callback-id) '(integer 1 18446744073709551615))
                      (= 1 (%ui-callback-current (runtime-backend runtime) (getf event :callback-id))))
           (return-from dispatch-event))
         (setf force (list (spec-name (mounted-spec owner))))
         (push :ui changed)))
      (:activity
       (when (runtime-json-server runtime)
         (json-broadcast (runtime-json-server runtime) "keyboard_activity"
                         (json-object (cons "hand" (getf event :hand)))))
       (unless (find :activity (runtime-mounts runtime)
                     :test (lambda (key mounted) (member key (mount-context-reads mounted))))
         (return-from dispatch-event))
       (push :activity changed))
      ((:map :metadata)
       (let* ((id (getf event :id))
              (old (find id (runtime-windows runtime) :key (lambda (w) (getf w :id))))
              (new (list :id id :identifier (window-identifier runtime id)
                         :title (getf event :title) :app-id (getf event :app-id)
                         :focus (and (getf old :focus t)
                                     (not (background-window-p runtime (getf event :pid 0))))
                         :content-type (getf event :content-type :none)
                         :tag (getf event :tag "") :description (getf event :description "")
                         :parent (getf event :parent) :dialog (and (getf event :dialog) t)
                         :modal (and (getf event :modal) t)
                         :width (getf event :width) :height (getf event :height)
                         :buffered (and (getf event :buffered (getf old :buffered t)) t)
                         :buffer-generation (or (getf old :buffer-generation)
                                                (incf (runtime-window-buffer-generation runtime)))
                         :fullscreen (and (getf event :fullscreen) t)
                         :maximize (and (getf event :maximize) t)
                         :fullscreen-requested (and (getf event :fullscreen-requested) t)
                         :maximize-requested (and (getf event :maximize-requested) t)
                         :output (getf event :output))))
         (when (and (getf event :client-width) (getf event :client-height)
                    (observe-client-geometry runtime id (getf event :client-width) (getf event :client-height)))
           (push :window-geometry changed))
         (if (equal old new)
             (when (and (eq (getf event :type) :metadata) (getf event :request))
               (push :windows changed))
             (progn
               (setf (runtime-windows runtime)
                     (if old (substitute new old (runtime-windows runtime))
                         (append (runtime-windows runtime) (list new))))
               (push :windows changed)))))
      (:buffer
       (let* ((id (getf event :id))
              (old (find id (runtime-windows runtime) :key (lambda (window) (getf window :id)))))
         (when old
           (let ((new (copy-data old)))
             (loop for (key value) on event by #'cddr
                   when (member key '(:title :app-id :fullscreen-requested :maximize-requested :output))
                     do (setf (getf new key) (copy-data value)))
             (when (and (getf event :attached) (not (getf old :buffered t)))
               (setf (getf new :buffer-generation)
                     (incf (runtime-window-buffer-generation runtime))))
             (setf (getf new :identifier) (window-identifier runtime id)
                   (getf new :buffered) (and (getf event :attached) t)
                   (getf new :fullscreen) (and (getf event :fullscreen) t)
                   (getf new :maximize) (and (getf event :maximize) t))
             (when (observe-client-geometry runtime id (getf event :width) (getf event :height))
               (push :window-geometry changed))
             (unless (equal old new)
               (setf (runtime-windows runtime) (substitute new old (runtime-windows runtime)))
               (push :windows changed))))))
      (:geometry
       (let ((old (find (getf event :id) (runtime-windows runtime)
                        :key (lambda (window) (getf window :id)))))
         (when old
           (when (observe-client-geometry runtime (getf event :id)
                                          (getf event :client-width) (getf event :client-height))
             (push :window-geometry changed))
           (let ((new (copy-list old)))
             (setf (getf new :width) (getf event :width)
                   (getf new :height) (getf event :height))
             (unless (equal old new)
               (setf (runtime-windows runtime)
                     (substitute new old (runtime-windows runtime)))
               (push :windows changed))))))
      (:layer
       (let* ((id (getf event :id))
              (record (list :id id :namespace (getf event :namespace) :layer (getf event :layer)
                            :anchors (getf event :anchors)
                            :exclusive-zone (getf event :exclusive-zone)
                            :margin (getf event :margin) :width (getf event :width)
                            :height (getf event :height) :keyboard (getf event :keyboard)
                            :scale-120 (getf event :scale-120 120) :output (getf event :output)
                            :request (getf event :request)
                            :visible t))
              (old (find id (runtime-layers runtime) :key (lambda (l) (getf l :id)))))
         (setf (runtime-layers runtime)
               (if old (substitute record old (runtime-layers runtime))
                   (append (runtime-layers runtime) (list record))))
         (unless (and old (getf record :request)
                      (equal (getf old :request) (getf record :request))
                      (equal (getf old :output) (getf record :output))
                      (equal (getf old :namespace) (getf record :namespace)))
           (push :layers changed))))
      (:unmap
       (let ((id (getf event :id)))
         (setf (runtime-windows runtime)
               (remove id (runtime-windows runtime) :key (lambda (w) (getf w :id)))
               (runtime-layers runtime)
               (remove id (runtime-layers runtime) :key (lambda (l) (getf l :id)))
               (runtime-client-geometries runtime)
               (remove id (runtime-client-geometries runtime) :key (lambda (entry) (getf entry :id))))
         (push :windows changed)
         (push :layers changed)))
      (:outputs
       (let ((facts (getf event :outputs))
             (connectors (output-connectors event))
             (revision (getf event :revision)))
         (when revision
           (when (< revision (runtime-outputs-revision runtime))
             (return-from dispatch-event))
           (setf (runtime-outputs-revision runtime) revision))
         (setf (runtime-outputs runtime) facts (runtime-connectors runtime) connectors)
         (unless (equal (getf (runtime-effective runtime) :outputs) facts)
           (push :outputs changed))
         (unless (equal (getf (runtime-effective runtime) :connectors) connectors)
           (push :connectors changed))))
      (:key
       (unless (getf event :state) (setf event (list* :state :pressed event)))
       (let ((dispatch (key-transaction runtime event)))
         (cond
           ((null dispatch) (return-from dispatch-event))
           ((eq (getf dispatch :type) :host)
            (run-host-key runtime dispatch)
            (return-from dispatch-event))
           (t (oracle-note-key runtime event)
              (setf event (getf dispatch :event))
              (dolist (name (getf dispatch :force)) (push name force))
              (dolist (key (getf dispatch :changed)) (pushnew key changed))))))
      (:adopted
       ;; The native side applied a staged transition before Lisp saw the key.
       (let ((memo (runtime-oracle-memo runtime))
             (key (list :type :key :state :pressed :owner (getf event :owner)
                        :command (getf event :command) :source-id (getf event :source-id)
                        :binding-id (getf event :binding-id))))
         (oracle-note-key runtime key)
         (if (and memo (eql (getf memo :epoch) (getf event :epoch))
                  (eql (getf memo :generation) (runtime-oracle-generation runtime)))
             (progn
               (incf (runtime-oracle-adopted runtime))
               (adopt-transaction runtime memo)
               (return-from dispatch-event))
             ;; Without the staged transaction the screen has still moved, so
             ;; run the key the ordinary way and let it commit what it finds.
             (progn
               (incf (runtime-oracle-missed runtime))
               (setf (runtime-oracle-memo runtime) nil)
               (let ((dispatch (key-transaction runtime key)))
                 (cond
                   ((null dispatch) (return-from dispatch-event))
                   ((eq (getf dispatch :type) :host)
                    (run-host-key runtime dispatch)
                    (return-from dispatch-event))
                   (t (setf event (getf dispatch :event))
                      (dolist (name (getf dispatch :force)) (push name force))
                      (dolist (change (getf dispatch :changed)) (pushnew change changed)))))))))
      (:command
       (push (getf event :owner) force)
       (push :command changed))
      (:request
       (when (find (getf event :id) (runtime-windows runtime) :key (lambda (w) (getf w :id)))
         (push :request changed)))
      (:button (push :button changed))
      (:pointer (push :pointer changed))
      (:screenshot (push :screenshot changed))
      (:screencast (push :screencast changed))
      (:grab (push :grab changed)))
    (setf (runtime-pending-context runtime)
          (union (intersection changed '(:windows :window-geometry :layers :outputs :connectors :services :system))
                 (runtime-pending-context runtime)))
    (when (and (or changed (runtime-pending-context runtime))
               (event-matters-p runtime changed force))
      (handler-case (transact runtime (runtime-mounts runtime) event changed force)
        (serious-condition (condition)
          (record-error runtime condition)
          (handler-case
              (progn
                (setf (runtime-output-recovery-failures runtime)
                      (matching-output-failures
                       (append (pending-output-failures runtime condition)
                               (runtime-output-recovery-failures runtime))
                       (getf (runtime-effective runtime) :output-config)
                       (runtime-connectors runtime)))
                (when (newer-native-output-request-p runtime)
                  (return-from dispatch-event))
                (when (runtime-running runtime)
                  (unwind-protect
                      (let ((context (materialize runtime (runtime-mounts runtime))))
                        (dolist (key '(:workareas :window-geometry :surfaces :output-errors))
                          (unless (equal (getf context key)
                                         (getf (runtime-effective runtime) key))
                            (pushnew key (runtime-pending-context runtime))))
                        (commit-context runtime (runtime-mounts runtime) context))
                    (discard-ui-asset-scratch runtime))))
            (serious-condition (failure) (record-error runtime failure))))))))

(defun describe-runtime (runtime)
  (let ((grab (resolved-grab runtime (runtime-mounts runtime))))
    (append (copy-data (loop for (key value) on (runtime-effective runtime) by #'cddr
                            unless (member key '(:surface-plans :native-output-config :output-failures))
                              append (list key value)))
            (list :socket (runtime-socket runtime) :generation (runtime-generation runtime)
                  :x-display (sb-ext:posix-getenv "DISPLAY")
                  :session-bus (sb-ext:posix-getenv "DBUS_SESSION_BUS_ADDRESS")
                  :last-error (runtime-last-error runtime)
                  :pending-context (copy-list (runtime-pending-context runtime))
                  :grab (describe-grab grab)
                  :motion-plan (copy-data (runtime-motion-plan runtime))
                  :native-grab (describe-grab (applied-grab (runtime-backend runtime)))
                  :native-ui (when (runtime-backend runtime)
                               (read-data (%ui-stats (runtime-backend runtime))))
                  :native-sound (when (runtime-backend runtime)
                                  (read-data (%sound-stats (runtime-backend runtime))))
                  :native-errors (read-data (%native-errors))
                  :oracle (list :enabled (and *oracle-enabled* t)
                                :prepared (runtime-oracle-prepared runtime)
                                :adopted (runtime-oracle-adopted runtime)
                                :missed (runtime-oracle-missed runtime))
                  :watch (runtime-watch runtime)
                  :timers (describe-timers runtime)
                  :watches (describe-watches runtime)
                  :executions (describe-executions runtime)
                  :retiring-executions (length (runtime-retired-executions runtime))
                  :managed-processes (describe-managed-processes runtime)
                  :retiring-processes (length (runtime-retired-processes runtime))
                  :once-processes (length (runtime-once-processes runtime))
                  :pending-spawns (count :pending (runtime-pending-spawns runtime) :key #'managed-process-status)
                  :process-history (if (runtime-process-history runtime)
                                       (hash-table-count (runtime-process-history runtime)) 0)
                  :rule-instances (describe-rule-instances runtime)
                  :patches (describe-patches runtime)
                  :extensions
                  (loop for m in (root-mounts (runtime-mounts runtime)) for spec = (mounted-spec m)
                        collect (list :name (spec-name spec) :source (spec-source spec)
                                      :reads (spec-reads spec) :admission (spec-admission spec) :state (mounted-state m)
                                      :shared (and (spec-immutable-p spec) t)
                                      :precompute (if (spec-precomputable-p spec) t nil)
                                      :dispatches (mounted-dispatches m)
                                      :failures (mounted-failures m)
                                      :last-error (mounted-last-error m)
                                      :effects (loop for e in (mounted-effects m)
                                                     collect (cons (effect-kind e)
                                                                   (effect-arguments e)))))))))
