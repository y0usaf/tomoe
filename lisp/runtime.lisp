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
(defvar *command-input* nil
  "Within a transaction or a host command call: whether input asked for the command.")
(defvar *layout-trace-path* nil
  "When the environment names a file, every materialization appends what it settled
on to it, so a recorded trace can compare an incremental layout with a full one.")

(defun make-runtime-state (socket)
  (list :socket socket
        :windows nil :client-geometries nil :layers nil
        :outputs nil :connectors nil :outputs-revision 0 :window-buffer-generation 0
        :services (list :notifications (list :available nil :notifications nil)
                        :mpris (default-mpris-state) :battery (default-battery-state)
                        :network (default-network-state) :tray (default-tray-state)
                        :audio (default-audio-state))
        :system nil :mounts nil :effective nil :running t
        :last-error nil :generation 0 :watch t :source-stamps nil
        :pending-context nil :output-recovery-failures nil
        :retired-commands nil :reads-cache nil :layout-memo nil
        :ipc-call nil :ipc-sequence 0 :ipc-published-context nil :ipc-published-announcements nil
        :config-error nil :sources nil :source-packages nil
        :patches nil :patch-sequence 0 :motion-plan nil :layers-cache nil :keysym-codes nil
        :frame-ms 16 :invocations 0 :layout-rebuilds 0 :layout-reuses 0
        :definitions-generation 0 :package-serial 0 :next-source-id 0 :budget-limits nil
        :ipc-baseline-sent nil))

(defvar *pending-writes* nil
  "This hook's state writes, held in the turn's arena until %PERSIST-STATE.")

(defun %r (key)
  (let ((tail *pending-writes*) (value nil) (found nil))
    (loop while (consp tail) do
      (when (eq (car tail) key)
        (setf value (car (cdr tail)) found t)
        (return))
      (setf tail (cddr tail)))
    (if found value (getf *runtime* key))))

(defun %r! (key value)
  (let ((tail *pending-writes*) (found nil))
    (loop while (consp tail) do
      (when (eq (car tail) key)
        (setf (car (cdr tail)) value found t)
        (return))
      (setf tail (cddr tail)))
    (unless found
      (setf *pending-writes* (cons key (cons value *pending-writes*)))))
  value)

(defun %counter (key) (or (%r key) 0))
(defun %bump (key)
  (let ((next (1+ (%counter key))))
    (%r! key next)
    next))
(defun %invocations () (%counter :invocations))
(defun %invocations+ () (%bump :invocations))
(defun %frame-ms-value () (or (%r :frame-ms) 16))

(defun runtime-windows () (%r :windows))
(defun runtime-client-geometries () (%r :client-geometries))
(defun runtime-layers () (%r :layers))
(defun runtime-outputs () (%r :outputs))
(defun runtime-connectors () (%r :connectors))
(defun runtime-outputs-revision () (%r :outputs-revision))
(defun runtime-services () (%r :services))
(defun runtime-system () (%r :system))
(defun runtime-mounts () (%r :mounts))
(defun runtime-effective () (%r :effective))
(defun runtime-running () (%r :running))
(defun runtime-last-error () (%r :last-error))
(defun runtime-generation () (%r :generation))
(defun runtime-config-error () (%r :config-error))
(defun runtime-pending-context () (%r :pending-context))
(defun runtime-output-recovery-failures () (%r :output-recovery-failures))
(defun runtime-retired-commands () (%r :retired-commands))
(defun runtime-reads-cache () (%r :reads-cache))
(defun runtime-layout-memo () (%r :layout-memo))
(defun runtime-ipc-call () (%r :ipc-call))
(defun runtime-ipc-sequence () (%r :ipc-sequence))
(defun runtime-ipc-published-context () (%r :ipc-published-context))
(defun runtime-ipc-published-announcements () (%r :ipc-published-announcements))
(defun runtime-sources () (%r :sources))
(defun runtime-source-packages () (%r :source-packages))
(defun runtime-patches () (%r :patches))
(defun runtime-patch-sequence () (%r :patch-sequence))
(defun runtime-motion-plan () (%r :motion-plan))
(defun runtime-layers-cache () (%r :layers-cache))
(defun runtime-socket () (%r :socket))
(defun %keysym-codes () (%r :keysym-codes))
(defun bump-generation ()
  (%r! :generation (1+ (runtime-generation))))
(defun runtime-watch () (%r :watch))

(defvar *retired-state* nil
  "The block the last keep replaced. It is released one hook later, so values the
current hook built from it stay valid until the turn ends.")

(defun %persist-state ()
  "Publish this hook's writes into the state the next hook reads."
  (let ((state *runtime*) (writes *pending-writes*))
    (loop while (consp writes) do
      (let ((key (car writes)) (value (car (cdr writes)))
            (tail state) (found nil))
        (loop while (consp tail) do
          (when (eq (car tail) key)
            (setf (car (cdr tail)) value found t)
            (return))
          (setf tail (cddr tail)))
        (unless found
          (setf state (cons key (cons value state)))))
      (setf writes (cddr writes)))
    (setf *pending-writes* nil)
    (let ((old *runtime*))
      (setf *runtime* (%keep state))
      (when *retired-state* (%release *retired-state*))
      (setf *retired-state* old)
      *runtime*)))

(defun make-mounted (&key spec state effects dispatches failures last-error
                          rule-parent rule-name rule-window rule-definition rule-pending)
  (list :mounted spec state effects (or dispatches 0) (or failures 0) last-error
        rule-parent rule-name rule-window rule-definition rule-pending))
(defun mounted-spec (mounted) (nth 1 mounted))
(defun mounted-state (mounted) (nth 2 mounted))
(defun mounted-effects (mounted) (nth 3 mounted))
(defun mounted-dispatches (mounted) (nth 4 mounted))
(defun mounted-failures (mounted) (nth 5 mounted))
(defun mounted-last-error (mounted) (nth 6 mounted))
(defun mounted-rule-parent (mounted) (nth 7 mounted))
(defun mounted-rule-name (mounted) (nth 8 mounted))
(defun mounted-rule-window (mounted) (nth 9 mounted))
(defun mounted-rule-definition (mounted) (nth 10 mounted))
(defun mounted-rule-pending (mounted) (nth 11 mounted))

(defun copy-mounted (mounted)
  (list :mounted (mounted-spec mounted) (mounted-state mounted) (mounted-effects mounted)
        (mounted-dispatches mounted) (mounted-failures mounted) (mounted-last-error mounted)
        (mounted-rule-parent mounted) (mounted-rule-name mounted) (mounted-rule-window mounted)
        (mounted-rule-definition mounted) (mounted-rule-pending mounted)))

(defun focused-window ()
  "The window that accepts keys now, or NIL."
  (let ((id (%keyboard-focus)))
    (if (and id (not (zerop id)))
        id
        (let* ((context (runtime-effective))
               (id (getf context :focus))
               (window (and id (find id (runtime-windows) :key (lambda (window) (getf window :id))))))
          (when (and window (getf window :buffered t)) id)))))

(defun window-output (id)
  "The output holding window ID's center, else the first output."
  (let* ((context (runtime-effective))
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

(defun command-argument-value (spec value)
  "VALUE as argument SPEC demands: the focused window or output resolved, the
window and output live, and every type checked."
  (let ((value (if (and (eq value :focused) (member (getf spec :type) '(:window :output)))
                   (ecase (getf spec :type)
                     (:window (or (focused-window) (error "No focused window.")))
                     (:output (or (window-output (focused-window)) (error "No output."))))
                   value)))
    (%check-command-value spec value)
    (case (getf spec :type)
      (:window
       (unless (find value (runtime-windows) :key (lambda (window) (getf window :id)))
         (error "No window ~D." value)))
      (:output
       (unless (find value (getf (runtime-effective) :outputs)
                     :test #'equal :key (lambda (output) (getf output :name)))
         (error "No output named ~A." value))))
    value))

(defun resolve-command-arguments (entry arguments)
  "Every argument ENTRY declares, in order: given, defaulted, or (for windows and
outputs) the focused one. Absent optional arguments are dropped."
  (let ((resolved nil) (name (getf entry :name)))
    (loop for (key) on arguments by #'cddr
          unless (find key (getf entry :arguments) :key (lambda (spec) (getf spec :name)))
            do (error "~A has no ~A argument." name (%lower key)))
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
           (error "~A needs a ~A argument." name (%lower key)))
          (t (setf present nil value nil)))
        (when (or present (getf spec :default-p) (member (getf spec :type) '(:window :output)))
          (push key resolved)
          (push (command-argument-value spec value) resolved))))))

(defun command-event (entry name arguments event)
  "EVENT as the :COMMAND event that runs NAME for its owning extension."
  (let ((copy (copy-list event)))
    (setf copy (%plist-remove :command copy))
    (setf (getf copy :type) :command
          (getf copy :input) (and (member (getf event :type) '(:key :button :grab)) t)
          (getf copy :owner) (getf entry :owner)
          (getf copy :name) name
          (getf copy :arguments) (resolve-command-arguments entry arguments))
    copy))

(defun callback-event (event name)
  "EVENT as the :COMMAND event of a private dialog callback named NAME."
  (let ((copy (copy-list event)))
    (setf copy (%plist-remove :command copy))
    (setf (getf copy :type) :command (getf copy :name) name (getf copy :arguments) nil)
    copy))

(defun keyboard-call (event)
  "The call a :KEY event names: its binding's press or release call, the
keyboard grab's otherwise call, or NIL for a private dialog callback."
  (let ((command (getf event :command)) (owner (getf event :owner)))
    (when (and command owner)
      (let ((binding (find-if (lambda (binding)
                                (and (equal (getf binding :owner) owner)
                                     (or (equal (getf binding :command) command)
                                         (equal (getf binding :release) command))))
                              (getf (runtime-effective) :bindings))))
        (cond (binding (if (equal command (getf binding :command))
                           (getf binding :call)
                           (getf binding :release-call)))
              (t (let ((grab (getf (runtime-effective) :keyboard-grab)))
                   (when (and grab (equal (getf grab :owner) owner)
                              (equal (getf grab :otherwise) command))
                     (getf grab :call)))))))))

(defun commit-call-resolution (context)
  "Resolve every binding in CONTEXT against the commands it declares, remembering
command names whose owner has gone away so their bindings retire with it."
  (let* ((commands (getf context :commands))
         (retired (remove-if (lambda (name) (registered-command commands name))
                             (union (runtime-retired-commands)
                                    (loop for entry in (getf (runtime-effective) :commands)
                                          unless (registered-command commands (getf entry :name))
                                            collect (getf entry :name))))))
    (%r! :retired-commands retired)
    (resolve-context-calls context retired)))

(defun run-command (name arguments)
  "Run the registered command NAME for the caller, filling declared defaults."
  (let* ((context (runtime-effective))
         (entry (or (registered-command (getf context :commands) name)
                    (error "Unknown command: ~A." name))))
    (if (getf entry :host)
        (host-command-call entry arguments)
        (let* ((owner (getf entry :owner))
               (mounted (find owner (runtime-mounts)
                              :test #'equal
                              :key (lambda (mounted) (spec-name (mounted-spec mounted))))))
          (when mounted
            (let ((failures (mounted-failures mounted))
                  (event (command-event entry name arguments (list :type :command :state :pressed))))
              (dispatch-event event)
              (when (> (mounted-failures mounted) (or failures 0))
                (error "~A: ~A" owner (mounted-last-error mounted)))
              (list :name name :arguments (getf event :arguments))))))))

(defun canonical-effect (effect)
  (unless (and (consp effect) (eq (first effect) :effect))
    (error "Not an effect: ~S" effect))
  (funcall (second (%effect-definition (effect-kind effect)))
           (copy-data (effect-arguments effect)) effect))

(defun canonical-command (command &optional directory)
  (unless (and (consp command) (eq (first command) :command))
    (error "Not a command: ~S" command))
  (let ((args (copy-data (command-arguments command))))
    (ecase (command-kind command)
      (:reply (destructuring-bind (value) args (ipc-reply value)))
      (:broadcast (destructuring-bind (name value) args (broadcast name value)))
      (:launch (apply #'launch args))
      (:spawn (destructuring-bind (launch cwd env focus) args
                (let* ((result (spawn launch :cwd cwd :env env :focus focus))
                       (spawn-arguments (command-arguments result)))
                  (when (and cwd (not (char= (char cwd 0) #\/)))
                    (unless directory (error "Relative spawn cwd requires a declaring source."))
                    (setf (nth 1 spawn-arguments)
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
  (let ((key (third (%effect-definition (effect-kind effect)))))
    (if key
        (funcall key (effect-arguments effect))
        (list (effect-kind effect) (first (effect-arguments effect))))))

(defun equivalent-effects (before after)
  "Whether AFTER contributes to a materialization exactly what BEFORE did."
  (and (= (length before) (length after))
       (every (lambda (old new)
                (and (eq (effect-kind old) (effect-kind new))
                     (equal (effect-arguments old) (effect-arguments new))))
              before after)))

(defun check-duplicate-effects (effects)
  "Reject two effects of one reducer that claim the same key."
  (let ((keys nil))
    (dolist (effect effects)
      (let ((key (effect-key effect)))
        (when (member key keys :test #'equal) (error "Duplicate effect: ~S" key))
        (push key keys)))))

(defun record-failure (mounted message)
  (when mounted
    (let ((live (find (spec-name (mounted-spec mounted)) (runtime-mounts)
                      :test #'equal :key (lambda (m) (spec-name (mounted-spec m))))))
      (when live
        (setf (nth 5 live) (1+ (mounted-failures live)))
        (setf (nth 6 live) message)))))

(defun invoke-extension (mounted context event)
  (let* ((spec (mounted-spec mounted))
         (name (spec-name spec))
         (reads (mount-context-reads mounted))
         (*source-directory* (spec-directory spec))
         (run (lambda ()
                (multiple-value-bind (state effects commands)
                    (funcall (if (mounted-rule-parent mounted)
                                 (lambda (snapshot state event)
                                   (invoke-rule-application mounted snapshot state event))
                                 (spec-update spec))
                             (make-snapshot context (copy-list reads) (runtime-effective))
                             (copy-data (mounted-state mounted)) (copy-data event))
                  (when (> (length effects) (%limit :effects))
                    (error "More than ~D effects." (%limit :effects)))
                  (when (> (length commands) (%limit :commands))
                    (error "More than ~D commands." (%limit :commands)))
                  (when (and commands
                             (not (or (member (getf event :type)
                                              '(:command :button :timer :watch :exec :request
                                                :screenshot :screencast :ipc :ui))
                                      (and (mounted-rule-parent mounted)
                                           (eq (getf event :type) :mount)))))
                    (error "One-shot commands require a command, button, timer, watch, exec, request, screenshot, screencast, IPC, or UI event."))
                  (let ((effects (mapcar #'canonical-effect effects))
                        (commands (mapcar (lambda (command)
                                            (canonical-command command (spec-directory spec)))
                                          commands)))
                    (let ((replies (count-if (lambda (command)
                                               (eq (command-kind command) :reply))
                                             commands)))
                      (when (plusp replies)
                        (unless (and (= replies 1) (eq (getf event :type) :ipc)
                                     (equal (getf event :owner) name)
                                     (runtime-ipc-call)
                                     (eql (getf event :call) (getf (runtime-ipc-call) :token)))
                          (error "IPC-REPLY requires this scope's current IPC request and permits one answer."))))
                    (check-duplicate-effects effects)
                    (unless (equivalent-effects (mounted-effects mounted) effects)
                      (%invocations+))
                    (setf (nth 2 mounted) (copy-data state)
                          (nth 3 mounted) effects)
                    (when (mounted-rule-parent mounted) (setf (nth 11 mounted) nil))
                    (setf (nth 4 mounted) (1+ (mounted-dispatches mounted)))
                    commands)))))
    (let ((outcome nil) (message nil))
      (%catch (lambda () (setf outcome (%with-limits nil (%limit :watchdog-ms) run)))
              (lambda (text) (setf message text)))
      (if message
          (progn
            (record-failure mounted message)
            (error "Extension ~A: ~A" name message))
          outcome))))

(defun resolved-grab (mounts)
  "The last owner's grab on a buffered target, and that owner."
  (let ((grab nil) (owner nil))
    (dolist (mounted mounts (values grab owner))
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :grab)
          (let* ((args (effect-arguments effect))
                 (id (first args))
                 (generation (third args))
                 (window (find id (runtime-windows) :key (lambda (w) (getf w :id)))))
            (when (or (eq (second args) :pointer)
                      (and window (getf window :buffered t)
                           (or (null generation) (eql generation (getf window :buffer-generation))))
                      (and (null generation)
                           (find id (runtime-layers) :key (lambda (l) (getf l :id)))))
              (setf grab (list id (second args))
                    owner mounted))))))))

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
Lisp."
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

(defun motion-plan-changed-p (motion)
  (let ((applied (runtime-motion-plan)))
    (if (getf motion :fast)
        (not (equal motion applied))
        (and applied (getf applied :fast)))))

(defun resolved-layer-overrides (mounts)
  "Resolve owned fields separately from client facts."
  (let ((overrides nil))
    (dolist (mounted mounts overrides)
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :layer)
          (destructuring-bind (id layer zone keyboard visible) (effect-arguments effect)
            (let ((entry (assoc id overrides)))
              (unless entry
                (setf entry (list id nil nil nil t))
                (push entry overrides))
              (when layer (setf (nth 1 entry) layer))
              (when zone (setf (nth 2 entry) zone))
              (when keyboard (setf (nth 3 entry) keyboard))
              (setf (nth 4 entry) visible))))))))

(defun applied-grab ()
  "Read the backend's actual grab. No Lisp cache can outlive a native target."
  (let ((id (%grab-id)) (mode (%grab-mode)))
    (unless (zerop mode)
      (list id (ecase mode (1 :move) (2 :resize) (3 :pointer))))))

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
  (let* ((output (or (%output-containing-point output-facts x y) (first output-facts)))
         (scale (and output (getf output :scale-120)))
         (fallback (and (first output-facts) (getf (first output-facts) :scale-120))))
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

(defun output-failure (connector config message)
  (list :name (getf connector :name) :config (copy-data config)
        :connector (connector-configuration-identity connector)
        :message message))

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

(defun resolve-output-preview (requested connectors)
  "Keep output failures local to their connector, request and declaration.
Unavailable advertised modes use the native preferred-mode fallback. Actual
failures hold arriving outputs inactive and active outputs at accepted geometry
while other owners can settle; a new request or declaration permits recovery."
  (let* ((old (runtime-effective))
         (failures
           (matching-output-failures
            (append (runtime-output-recovery-failures)
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
             (preview nil) (message nil) (*output-error-name* nil))
        (%catch (lambda ()
                  (setf preview
                        (if (not (equal (resolved-native-output-config old) native-config))
                            (preview-native-outputs native-config)
                            (list :outputs (runtime-outputs) :connectors connectors))))
                (lambda (text) (setf message text)))
        (if (null message)
            (return (values preview native-config
                            (sort failures #'string< :key (lambda (item) (getf item :name)))))
            (let* ((name *output-error-name*)
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
                (error "~A" message))
              (push (output-failure connector config message) failures)))))))

(defun pending-output-failures (message)
  (loop for connector in (runtime-connectors)
        when (or (getf connector :request-pending)
                 (and *output-error-name* (getf connector :pending)))
          collect (output-failure connector
                                  (find (getf connector :name)
                                        (getf (runtime-effective) :output-config)
                                        :test #'equal :key (lambda (item) (getf item :name)))
                                  message)))

(defun resolved-window-geometry (layout view outputs)
  "Use committed logical sizes with the same candidate scale/location as rendering."
  (loop for box in layout when (getf box :visible) collect
    (let* ((id (getf box :id))
           (client (find id (runtime-client-geometries)
                         :key (lambda (record) (getf record :id)))))
      (multiple-value-bind (screen-x screen-y) (%view-screen-point box view)
        (let ((scale (%output-scale-120 screen-x screen-y outputs)))
          (list :id id :x (getf box :x) :y (getf box :y)
                :width (if client (%round-away-positive (/ (* (getf client :width) scale) 120d0))
                           (getf box :width))
                :height (if client (%round-away-positive (/ (* (getf client :height) scale) 120d0))
                            (getf box :height))))))))

(defun observe-client-geometry (id width height)
  (unless (and (%in-range-p width 0 2147483647) (%in-range-p height 0 2147483647))
    (error "Invalid committed client geometry: ~S x ~S." width height))
  (let* ((old (find id (runtime-client-geometries) :key (lambda (entry) (getf entry :id))))
         (new (list :id id :width width :height height)))
    (unless (equal old new)
      (%r! :client-geometries
           (if old (substitute new old (runtime-client-geometries))
               (append (runtime-client-geometries) (list new))))
      t)))

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
    (ignore-errors (%write-append *layout-trace-path* (format nil "~S~%" (layout-step-record context))))))

(defun make-layout-memo (&key names invocations definitions revision windows layers
                              client-geometries services system config-error outputs
                              connectors recovery-failures context)
  (list :memo names invocations definitions revision windows layers client-geometries
        services system config-error outputs connectors recovery-failures context))
(defun layout-memo-names (memo) (nth 1 memo))
(defun layout-memo-invocations (memo) (nth 2 memo))
(defun layout-memo-definitions (memo) (nth 3 memo))
(defun layout-memo-revision (memo) (nth 4 memo))
(defun layout-memo-windows (memo) (nth 5 memo))
(defun layout-memo-layers (memo) (nth 6 memo))
(defun layout-memo-client-geometries (memo) (nth 7 memo))
(defun layout-memo-services (memo) (nth 8 memo))
(defun layout-memo-system (memo) (nth 9 memo))
(defun layout-memo-config-error (memo) (nth 10 memo))
(defun layout-memo-outputs (memo) (nth 11 memo))
(defun layout-memo-connectors (memo) (nth 12 memo))
(defun layout-memo-recovery-failures (memo) (nth 13 memo))
(defun layout-memo-context (memo) (nth 14 memo))

(defun mount-names (mounts)
  (loop for mounted in mounts collect (spec-name (mounted-spec mounted))))

(defun same-mounts-p (names mounts)
  (and (= (length names) (length mounts))
       (loop for name in names
             for mounted in mounts
             always (equal name (spec-name (mounted-spec mounted))))))

(defun reusable-layout (mounts)
  "The context the last materialization produced, when nothing a materialization
folds has changed since."
  (let ((memo (runtime-layout-memo)))
    (when (and memo
               (eql (layout-memo-invocations memo) (%invocations))
               (eql (layout-memo-definitions memo) (%counter :definitions-generation))
               (eql (layout-memo-revision memo) (runtime-outputs-revision))
               (eq (layout-memo-windows memo) (runtime-windows))
               (eq (layout-memo-layers memo) (runtime-layers))
               (eq (layout-memo-client-geometries memo) (runtime-client-geometries))
               (eq (layout-memo-services memo) (runtime-services))
               (eq (layout-memo-system memo) (runtime-system))
               (eq (layout-memo-config-error memo) (runtime-config-error))
               (eq (layout-memo-outputs memo) (runtime-outputs))
               (eq (layout-memo-connectors memo) (runtime-connectors))
               (eq (layout-memo-recovery-failures memo) (runtime-output-recovery-failures))
               (same-mounts-p (layout-memo-names memo) mounts))
      (layout-memo-context memo))))

(defun remember-layout (mounts context)
  (%r! :layout-memo
       (make-layout-memo :names (mount-names mounts) :invocations (%invocations)
                         :definitions (%counter :definitions-generation)
                         :revision (runtime-outputs-revision)
                         :windows (runtime-windows)
                         :layers (runtime-layers)
                         :client-geometries (runtime-client-geometries)
                         :services (runtime-services)
                         :system (runtime-system)
                         :config-error (runtime-config-error)
                         :outputs (runtime-outputs)
                         :connectors (runtime-connectors)
                         :recovery-failures (runtime-output-recovery-failures)
                         :context context)))

(defun materialize (mounts)
  "Reconstruct owned context, reusing what the last materialization settled on
when nothing it folds has changed."
  (let ((memo *materialized*) (revision (runtime-outputs-revision)))
    (labels ((republish (context)
               (when memo
                 (setf (nth 0 memo) (copy-list mounts)
                       (nth 1 memo) (%invocations)
                       (nth 2 memo) context
                       (nth 3 memo) revision))
               (trace-layout-step context)
               context))
      (cond
        ((and memo (eql (second memo) (%invocations)) (eql (fourth memo) revision)
              (= (length mounts) (length (first memo))) (%same-elements-p mounts (first memo)))
         (%bump :layout-reuses)
         (republish (third memo)))
        (t (let ((reused (reusable-layout mounts)))
             (cond (reused (%bump :layout-reuses) (republish reused))
                   (t (%bump :layout-rebuilds)
                      (republish (%materialize mounts))))))))))

(defun %materialize (mounts)
  "Reconstruct owned context. Preserve live clients, output facts, and map order.
Unmount removes a contribution, exposing the preceding owner or these defaults.
The single grab is not context data; RESOLVED-GRAB derives it from the mounts."
  (refresh-output-facts)
  (let* ((mounts (prune-rule-mounts mounts))
         (m (make-materialization
             :layout (loop for window in (runtime-windows)
                           collect (list :id (getf window :id) :x 0 :y 0
                                         :width (max 1 (getf window :width))
                                         :height (max 1 (getf window :height)) :visible t
                                         :fullscreen nil :maximize nil :properties nil))
             :stacking (mapcar (lambda (window) (getf window :id)) (runtime-windows))
             :layers (copy-data (runtime-layers))
             :keyboard (default-keyboard-config)
             :settings (%settings-defaults +settings+)
             :view (list :x 0 :y 0 :zoom 1d0))))
    (dolist (phase '(0 1))
      (when (= phase 1)
        (setf (getf m :mod-bit)
              (ecase (getf (materialization-settings m) :mod) (:shift 1) (:control 4) (:alt 8) (:super 64))))
      (dolist (mounted mounts)
        (dolist (effect (mounted-effects mounted))
          (let ((definition (%effect-definition (effect-kind effect))))
            (when (and (fourth definition) (= phase (fifth definition)))
              (funcall (fourth definition) m (effect-arguments effect) mounted))))))
    (let* ((layout (materialization-layout m))
           (stacking (materialization-stacking m))
           (layers (materialization-layers m))
           (focused (materialization-focused m))
           (data (materialization-data m))
           (surfaces (materialization-surfaces m))
           (keyboard-grab (materialization-keyboard-grab m))
           (outputs (materialization-outputs m))
           (bindings (materialization-bindings m))
           (keyboard (materialization-keyboard m))
           (settings (materialization-settings m))
           (view (materialization-view m))
           (live-connectors (or (runtime-connectors)
                                (output-connectors (list :outputs (runtime-outputs)))))
           (output-config (default-output-config outputs live-connectors (getf settings :scale)))
           (connected (connected-output-config
                       (list :connectors live-connectors :output-config output-config)))
           (output-resolution (multiple-value-list (resolve-output-preview connected live-connectors)))
           (output-preview (first output-resolution))
           (native-output-config (second output-resolution))
           (output-failures (third output-resolution))
           (output-facts (getf output-preview :outputs))
           (connector-facts (output-connectors output-preview)))
      (%quantize-layout layout view output-facts)
      (let* ((overrides (resolved-layer-overrides mounts))
             (cache (runtime-layers-cache)))
        (multiple-value-bind (resolved-layers workareas)
            (if (and cache (equal (first cache) output-facts)
                     (equal (second cache) layers) (equal (third cache) overrides))
                (values (fourth cache) (fifth cache))
                (multiple-value-bind (resolved areas)
                    (preview-native-layers output-facts layers overrides)
                  (%r! :layers-cache (list output-facts layers overrides resolved areas))
                  (values resolved areas)))
          (unless (find focused layout :key (lambda (w) (getf w :id)))
            (setf focused nil))
          (multiple-value-bind (surface-plans shell-workareas)
              (resolve-shell-surfaces
               (nreverse surfaces) output-facts workareas
               :asset-loader nil
               :viewport (shell-viewport output-facts (materialization-data m)))
            (list :windows (runtime-windows)
                  :window-geometry (resolved-window-geometry layout view output-facts)
                  :rules (resolved-rule-properties mounts) :data data
                  :services (copy-data (runtime-services))
                  :system (copy-data (runtime-system))
                  :outputs output-facts :connectors connector-facts
                  :output-config output-config :native-output-config native-output-config
                  :output-failures output-failures
                  :output-errors (loop for failure in output-failures collect
                                   (list :name (getf failure :name) :message (getf failure :message)))
                  :workareas shell-workareas
                  :surface-plans (mapcar #'copy-data surface-plans)
                  :surfaces (loop for plan in surface-plans
                                  collect (loop for key in '(:owner :source-id :name :output :x :y :width :height :layer :click-through)
                                                append (list key (copy-data (getf plan key)))))
                  :view view :layout layout
                  :stacking (remove-if-not
                             (lambda (id) (getf (find id layout :key (lambda (window) (getf window :id))) :visible))
                             stacking)
                  :layers resolved-layers :focus focused :keyboard keyboard :settings settings
                  :sounds (materialization-sounds m)
                  :cursors (stable-sort (copy-list (materialization-cursors m)) #'<
                                        :key (lambda (entry) (getf entry :order)))
                  :virtual-outputs (sort (copy-list (materialization-virtual-outputs m)) #'string<
                                         :key (lambda (o) (getf o :name)))
                  :config-error (copy-data (runtime-config-error))
                  :keyboard-grab keyboard-grab
                  :commands (sort (append (host-command-entries)
                                          (copy-list (materialization-commands m)))
                                  #'string< :key (lambda (entry) (getf entry :name)))
                  :bindings (sort bindings
                                  (lambda (a b) (or (< (getf a :modifiers) (getf b :modifiers))
                                                    (and (= (getf a :modifiers) (getf b :modifiers))
                                                         (< (getf a :code) (getf b :code)))))))))))))

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

(defun wanted-resources (mounts)
  "Every owned timer, watch, execution and process the committed plan asks for."
  (let ((wanted nil))
    (dolist (mounted mounts (nreverse wanted))
      (let* ((spec (mounted-spec mounted))
             (owner (spec-name spec))
             (directory (spec-directory spec))
             (source-id (spec-id spec)))
        (dolist (effect (mounted-effects mounted))
          (let ((kind (effect-kind effect)))
            (when (member kind '(:timer :watch :exec :process))
              (push (list :kind kind :owner owner
                          :arguments (copy-data (effect-arguments effect))
                          :directory directory :source-id source-id :token source-id)
                    wanted))))))))

(defun mounted-reads ()
  "The context keys the mounted policy reacts to, cached until a commit."
  (or (runtime-reads-cache)
      (let ((reads (%dedupe
                    (loop for mounted in (append (root-mounts (runtime-mounts))
                                                 (rule-instances (runtime-mounts)))
                          append (mount-context-reads mounted)))))
        (%r! :reads-cache reads)
        reads)))

(defun event-matters-p (changed force)
  "Whether a transaction could differ from the committed context."
  (or force
      (runtime-pending-context)
      (and changed (intersection changed (mounted-reads)))
      (loop for mounted in (rule-instances (runtime-mounts))
            thereis (mounted-rule-pending mounted))))

(defun transact (mounts event changed &optional force)
  (let ((*command-input* (and (getf event :input) t)))
    (%transact mounts event changed force)))

(defun %transact (mounts event changed &optional force)
  "Stage reducers and dependency propagation before touching the native scene.
A bad callback or a dependency cycle discards the entire candidate transaction."
  (let* ((*materialized* (list nil -1 nil nil))
         (candidate (prune-rule-mounts
                     (mapcar #'copy-mounted
                             (append (root-mounts mounts)
                                     (rule-instances (runtime-mounts))))))
         (context (materialize candidate))
         (dirty (union (runtime-pending-context)
                       (union changed (changed-keys (runtime-effective) context))))
         (commands nil)
         (deferred-deliveries nil)
         (proposal-baselines nil)
         (proposal-start (mapcar #'mounted-spec candidate))
         (proposal-deliveries nil)
         (initial-event event)
         (initial-force force))
    (labels ((baseline (mounted)
               (let* ((spec (mounted-spec mounted))
                      (entry (assoc spec proposal-baselines :test #'eq)))
                 (if entry
                     (cdr entry)
                     (let ((value (if (member spec proposal-start :test #'eq)
                                      (copy-data (mounted-state mounted))
                                      (sixth (effect-arguments (mounted-rule-definition mounted))))))
                       (setf proposal-baselines
                             (%alist-set spec value proposal-baselines #'eq))
                       value))))
             (invoke (mounted delivered)
               (when (admission-scope-p mounted candidate)
                 (let ((spec (mounted-spec mounted)))
                   (setf (nth 2 mounted) (copy-data (baseline mounted)))
                   (let ((entry (assoc spec proposal-deliveries :test #'eq)))
                     (if entry
                         (setf delivered (cdr entry))
                         (setf proposal-deliveries
                               (%alist-set spec (copy-data delivered) proposal-deliveries #'eq))))
                   (setf commands (remove spec commands :key #'car :test #'eq))))
               (setf commands
                     (nconc commands
                            (mapcar (lambda (command) (cons (mounted-spec mounted) command))
                                    (invoke-extension mounted context delivered))))))
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
          (setf context (materialize candidate))
          (multiple-value-bind (reconciled new revised)
              (reconcile-rule-mounts candidate context)
            (setf candidate reconciled context (materialize reconciled))
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
                           (entry (assoc spec deferred-deliveries :test #'eq))
                           (delivery
                             (or (cdr entry)
                                 (cond ((mounted-rule-pending mounted) '(:type :mount))
                                       ((or (and (member (getf event :type) '(:command :timer :watch :exec :ipc :ui))
                                                 (not (member name force :test #'equal)))
                                            (and (member (getf event :type) '(:button :pointer :request))
                                                 (not (eql (getf event :id) (mounted-rule-window mounted)))))
                                        (list :type :change :keys child-dirty))
                                       (t event)))))
                      (if (eq declaration (mounted-rule-definition mounted))
                          (progn
                            (setf deferred-deliveries (%alist-remove spec deferred-deliveries #'eq))
                            (invoke mounted delivery)
                            (push spec invoked))
                          (setf deferred-deliveries
                                (%alist-set spec delivery deferred-deliveries #'eq)))))))))
          (setf context (materialize candidate))
          (multiple-value-bind (reconciled new revised)
              (reconcile-rule-mounts candidate context)
            (setf candidate reconciled
                  force (union new revised :test #'equal)))
          (setf context (materialize candidate))
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
                (setf context (materialize candidate)))))
          (multiple-value-bind (reconciled new revised)
              (reconcile-rule-mounts candidate context)
            (setf candidate reconciled force (union force (union new revised :test #'equal) :test #'equal)))
          (multiple-value-bind (next changes)
              (let ((materialized (materialize candidate)))
                (values materialized (changed-keys before materialized)))
            (setf context next dirty changes event (list :type :change :keys changes))
            (unless (or changes force)
              (commit-call-resolution context)
              (let ((live-commands (loop for (owner . command) in commands
                                         when (find owner candidate :key #'mounted-spec) collect command)))
                (commit-context candidate context live-commands)
                (%r! :pending-context nil)
                (loop for command in live-commands
                      do (when (runtime-running) (execute-command command))))
              (return-from %transact t)))))
      (error "Extension dependencies did not settle within 16 rounds."))))

(defun execute-command (command)
  (ecase (command-kind command)
    (:reply
     (let ((call (runtime-ipc-call)))
       (when call
         (setf (getf call :result) (first (command-arguments command))
               (getf call :replied) t))))
    (:broadcast (%json-broadcast (first (command-arguments command))
                                 (second (command-arguments command))))
    (:launch (destructuring-bind (launch cwd env focus) (command-arguments command)
               (%spawn launch cwd env focus)))
    (:spawn (destructuring-bind (launch cwd env focus) (command-arguments command)
              (%spawn launch cwd env focus)))
    (:close (%close (first (command-arguments command))))
    (:screencast (destructuring-bind (token answer value) (command-arguments command)
                   (%screencast-answer token answer value)))
    (:screenshot (%screenshot (if (first (command-arguments command)) nil t)))
    (:media (%media (first (command-arguments command))))
    (:clipboard (destructuring-bind (path type delete) (command-arguments command)
                  (%clipboard-copy path type delete)))
    (:brightness (%backlight (first (command-arguments command))))
    (:power (destructuring-bind (mode name) (command-arguments command)
              (%output-power name (ecase mode (:off 0) (:on 1) (:toggle 2)))))
    (:quit (%quit))
    (:reload (%reload-policy))))

(defun commit-context (mounts context &optional commands)
  "The only extension-to-native write path. All reducers have returned and validated."
  (setf mounts (prune-rule-mounts mounts))
  (%commit-context mounts context commands))

(defun %commit-context (mounts context commands)
  ;; Timers declared :REFRESH resolve against the display rate the context
  ;; reports, so the bar and HUD step once per refresh of the monitor in use.
  (%r! :frame-ms (frame-period-ms (getf context :outputs)))
  (sync-budgets (getf context :settings))
  (let* ((old (runtime-effective))
         (restack (not (equal (getf old :stacking) (getf context :stacking))))
         (layout-changed (not (equal (getf old :layout) (getf context :layout))))
         (focus-changed (not (eql (getf old :focus) (getf context :focus))))
         (outputs (resolved-native-output-config context))
         (outputs-changed (not (equal (resolved-native-output-config old) outputs)))
         (bindings-changed (not (equal (getf old :bindings) (getf context :bindings))))
         (keyboard-changed (not (equal (getf old :keyboard) (getf context :keyboard))))
         (settings-changed (not (equal (getf old :settings) (getf context :settings))))
         (sounds-changed (not (equal (getf old :sounds) (getf context :sounds))))
         (grab-changed (not (equal (getf old :keyboard-grab) (getf context :keyboard-grab))))
         (overrides (resolved-layer-overrides mounts))
         (grab-details (multiple-value-list (resolved-grab mounts)))
         (grab (first grab-details))
         (motion (resolved-motion-plan mounts grab (second grab-details)))
         (wanted (wanted-resources mounts)))
    (when (or (member :outputs (runtime-pending-context))
              outputs-changed bindings-changed keyboard-changed settings-changed sounds-changed grab-changed
              restack layout-changed focus-changed
              (not (equal (getf old :outputs) (getf context :outputs)))
              (not (equal (getf old :view) (getf context :view)))
              (not (equal (getf old :layers) (getf context :layers)))
              (not (equal (getf old :surface-plans) (getf context :surface-plans)))
              (not (equal (resolved-layer-overrides (runtime-mounts)) overrides))
              (not (equal grab (applied-grab)))
              (motion-plan-changed-p motion))
      (configure-native-presentation context overrides restack
                                     outputs-changed bindings-changed grab motion keyboard-changed
                                     settings-changed sounds-changed)
      (%r! :motion-plan motion))
    (when outputs-changed
      (%r! :outputs (getf context :outputs))
      (%r! :connectors (getf context :connectors)))
    (when (not (equal (getf old :virtual-outputs) (getf context :virtual-outputs)))
      (configure-virtual-outputs (getf old :virtual-outputs) (getf context :virtual-outputs)))
    (%r! :mounts mounts)
    (%r! :effective context)
    (%r! :output-recovery-failures nil)
    (%r! :reads-cache nil)
    (%set-reads (mounted-reads))
    (stage-cursor-effects context)
    (let ((message (%resources-commit wanted)))
      (when message (error "~A" message)))
    (%sources (loop for mounted in (root-mounts mounts)
                    for source = (spec-source (mounted-spec mounted))
                    when (and source (not (%embedded-source-p source))) collect source))
    (publish-json-context context mounts)
    (remember-layout mounts context)
    nil))

(defvar (%counter :package-serial) 0)

(defun %directory-of (path)
  (let ((slash (position #\/ path :from-end t)))
    (if (and slash (plusp slash)) (subseq path 0 (1+ slash)) nil)))

(defun %embedded-source-p (path)
  (not (find #\/ path)))

(defun %fresh-package-name (path)
  (%bump :package-serial)
  (format nil "TOMOE-USER/~A-~D"
          (string-upcase (or (and (%directory-of path)
                                  (subseq path (length (%directory-of path))))
                             path))
          (%counter :package-serial)))

(defvar *library-specs* :outside)

(defun load-specs (path &optional package)
  (%bump :definitions-generation)
  (let* ((*library-specs* nil)
         (*source* path)
         (*source-directory* (%directory-of path))
         (package (or package (%fresh-package-name path)))
         (*definitions* nil)
         (text (unless (%embedded-source-p path)
                 (or (%read-file path (%limit :source))
                     (error "No extension source at ~A." path)))))
    (unless (member package (runtime-source-packages) :test #'equal)
      (%r! :source-packages (cons package (runtime-source-packages))))
    (%load-source path text package)
    (when *library-specs*
      (%r! :library-owners
           (append (loop for spec in *library-specs* collect (cons (spec-name spec) path))
                   (remove-if (lambda (entry)
                                (find (car entry) *library-specs* :key #'spec-name :test #'equal))
                              (%r :library-owners)))))
    (append *library-specs*
            (loop for spec in (nreverse *definitions*)
                  do (setf (nth 6 spec) *source-directory*)
                     (setf (nth 8 spec) package)
                  collect spec))))

(defun sweep-source-packages ()
  (let ((live (remove nil (mapcar (lambda (mounted) (spec-package (mounted-spec mounted)))
                                  (runtime-mounts)))))
    (%r! :source-packages
         (loop for package in (runtime-source-packages)
               if (member package live :test #'equal) collect package
                 else do (when (stringp package) (%drop-package package))))))

(defun %owner-source (spec)
  (or (cdr (assoc (spec-name spec) (%r :library-owners) :test #'equal))
      (spec-source spec)))

(defun configure (sources &optional changed-source)
  (unwind-protect
      (let* ((specs (loop for path in sources
                          when (or (null changed-source) (equal path changed-source))
                            append (load-specs path)))
             (retained (when changed-source
                         (remove changed-source (root-mounts (runtime-mounts)) :test #'equal
                                 :key (lambda (m) (%owner-source (mounted-spec m))))))
             (names nil))
        (dolist (spec (append (mapcar #'mounted-spec retained) specs))
          (when (member (spec-name spec) names :test #'equal)
            (error "Duplicate extension: ~A" (spec-name spec)))
          (push (spec-name spec) names))
        (when (> (length (append retained specs)) (%limit :extensions))
          (error "More than ~D mounted extensions." (%limit :extensions)))
        (let ((mounts
                (loop for spec in specs
                      for previous = (find (spec-name spec) (runtime-mounts)
                                           :test #'equal :key (lambda (m) (spec-name (mounted-spec m))))
                      collect (if (and previous (equal (spec-source spec) (spec-source (mounted-spec previous))))
                                  (let ((copy (copy-mounted previous))) (setf (nth 1 copy) spec) copy)
                                  (make-mounted :spec spec :state (copy-data (spec-initial spec))))))
              (patched (remove-if-not (lambda (mounted) (spec-patch (mounted-spec mounted)))
                                      (root-mounts (runtime-mounts)))))
          (transact
           (append (loop for source in sources append
                     (remove source (append retained mounts) :test-not #'equal
                             :key (lambda (m) (%owner-source (mounted-spec m)))))
                   patched)
           '(:type :mount) nil (mapcar #'spec-name specs)))
        (%r! :sources (copy-list sources))
        (%r! :last-error nil)
        (bump-generation))
    (sweep-source-packages)))

(defun unmount (name)
  (unless (find name (root-mounts (runtime-mounts)) :test #'equal
                :key (lambda (m) (spec-name (mounted-spec m))))
    (error "No mounted extension named ~A." name))
  (unwind-protect
       (transact (remove name (runtime-mounts) :test #'equal
                         :key (lambda (m) (spec-name (mounted-spec m))))
                 (list :type :unmount :name name) nil)
    (sweep-source-packages))
  (bump-generation))

(defun record-error (message)
  (%r! :last-error message)
  (%log :error message))

(defun report-config-error (message fallback)
  "Record a failed source load and publish it as :CONFIG-ERROR."
  (record-error message)
  (%r! :config-error
       (list :serial (1+ (getf (runtime-config-error) :serial 0))
             :message (if fallback
                          "Failed to load the config file. Running with defaults; check the log for details."
                          "Failed to load the config file. Keeping the running config; check the log for details.")))
  (let ((failure nil))
    (%catch (lambda ()
              (transact (runtime-mounts) '(:type :change :keys (:config-error))
                        '(:config-error)))
            (lambda (text) (setf failure text)))
    (when failure (record-error failure))))

(defun newer-native-output-request-p ()
  (let ((revision (%outputs-revision)) (state (%r :outputs-revision)))
    (unless (and (numberp revision) (numberp state))
      (error "refresh: revision=~S state=~S runtime=~S keys=~S"
             revision state (and *runtime* t)
             (and *runtime* (loop for (key value) on *runtime* by #'cddr collect key))))
    (when (> revision state)
      (loop for connector in (output-connectors (current-native-outputs))
            for previous = (find (getf connector :name) (runtime-connectors)
                                 :test #'equal :key (lambda (item) (getf item :name)))
            thereis (and (getf connector :request-pending)
                         (or (not (eql (getf connector :id) (getf previous :id)))
                             (not (eql (getf connector :request-id)
                                       (getf previous :request-id)))))))))

(defun refresh-output-facts ()
  "Adopt newer committed native output facts before rebuilding policy context."
  (let ((revision (%outputs-revision)))
    (when (> revision (runtime-outputs-revision))
      (let* ((snapshot (current-native-outputs))
             (facts (getf snapshot :outputs))
             (connectors (output-connectors snapshot))
             (snapshot-revision (getf snapshot :revision)))
        (unless (and (integerp snapshot-revision) (>= snapshot-revision revision))
          (error "Native output snapshot revision regressed: ~S" snapshot))
        (unless (equal facts (runtime-outputs))
          (%r! :pending-context (adjoin :outputs (runtime-pending-context))))
        (unless (equal connectors (runtime-connectors))
          (%r! :pending-context (adjoin :connectors (runtime-pending-context))))
        (%r! :outputs facts)
        (%r! :connectors connectors)
        (%r! :outputs-revision snapshot-revision)))))

(defun dispatch-event (event)
  (let ((changed nil) (force nil))
    (ecase (getf event :type)
      (:system
       (%r! :system (getf event :system))
       (push :system changed))
      (:services
       (let ((services (copy-data (getf event :services))))
         (unless (equal services (runtime-services))
           (%r! :services services)
           (push :services changed))))
      (:ui
       (let ((owner (find (getf event :owner) (runtime-mounts)
                          :test #'equal :key (lambda (mounted) (spec-name (mounted-spec mounted))))))
         (unless (and owner (eql (getf event :source-id) (spec-id (mounted-spec owner))))
           (return-from dispatch-event))
         (setf force (list (spec-name (mounted-spec owner))))
         (push :ui changed)))
      (:activity
       (unless (find :activity (runtime-mounts)
                     :test (lambda (key mounted) (member key (mount-context-reads mounted))))
         (return-from dispatch-event))
       (push :activity changed))
      ((:map :metadata)
       (let* ((id (getf event :id))
              (old (find id (runtime-windows) :key (lambda (w) (getf w :id))))
              (new (list :id id :identifier (%window-identifier id)
                         :title (getf event :title) :app-id (getf event :app-id)
                         :focus (and (getf old :focus t)
                                     (not (%background-window-p (getf event :pid 0))))
                         :content-type (getf event :content-type :none)
                         :tag (getf event :tag "") :description (getf event :description "")
                         :parent (getf event :parent) :dialog (and (getf event :dialog) t)
                         :modal (and (getf event :modal) t)
                         :width (getf event :width) :height (getf event :height)
                         :buffered (and (getf event :buffered (getf old :buffered t)) t)
                         :buffer-generation (if (getf old :buffer-generation)
                                                (getf old :buffer-generation)
                                                (next-buffer-generation))
                         :fullscreen (and (getf event :fullscreen) t)
                         :maximize (and (getf event :maximize) t)
                         :fullscreen-requested (and (getf event :fullscreen-requested) t)
                         :maximize-requested (and (getf event :maximize-requested) t)
                         :output (getf event :output))))
         (when (and (getf event :client-width) (getf event :client-height)
                    (observe-client-geometry id (getf event :client-width) (getf event :client-height)))
           (push :window-geometry changed))
         (if (equal old new)
             (when (and (eq (getf event :type) :metadata) (getf event :request))
               (push :windows changed))
             (progn
               (%r! :windows
                    (if old (substitute new old (runtime-windows))
                        (append (runtime-windows) (list new))))
               (push :windows changed)))))
      (:buffer
       (let* ((id (getf event :id))
              (old (find id (runtime-windows) :key (lambda (window) (getf window :id)))))
         (when old
           (let ((new (copy-data old)))
             (loop for (key value) on event by #'cddr
                   when (member key '(:title :app-id :fullscreen-requested :maximize-requested :output))
                     do (setf (getf new key) (copy-data value)))
             (when (and (getf event :attached) (not (getf old :buffered t)))
               (setf (getf new :buffer-generation) (next-buffer-generation)))
             (setf (getf new :identifier) (%window-identifier id)
                   (getf new :buffered) (and (getf event :attached) t)
                   (getf new :fullscreen) (and (getf event :fullscreen) t)
                   (getf new :maximize) (and (getf event :maximize) t))
             (when (observe-client-geometry id (getf event :width) (getf event :height))
               (push :window-geometry changed))
             (unless (equal old new)
               (%r! :windows (substitute new old (runtime-windows)))
               (push :windows changed))))))
      (:geometry
       (let ((old (find (getf event :id) (runtime-windows)
                        :key (lambda (window) (getf window :id)))))
         (when old
           (when (observe-client-geometry (getf event :id)
                                          (getf event :client-width) (getf event :client-height))
             (push :window-geometry changed))
           (let ((new (copy-data old)))
             (setf (getf new :width) (getf event :width)
                   (getf new :height) (getf event :height))
             (unless (equal old new)
               (%r! :windows (substitute new old (runtime-windows)))
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
              (old (find id (runtime-layers) :key (lambda (l) (getf l :id)))))
         (%r! :layers
              (if old (substitute record old (runtime-layers))
                  (append (runtime-layers) (list record))))
         (unless (and old (getf record :request)
                      (equal (getf old :request) (getf record :request))
                      (equal (getf old :output) (getf record :output))
                      (equal (getf old :namespace) (getf record :namespace)))
           (push :layers changed))))
      (:unmap
       (let ((id (getf event :id)))
         (%r! :windows (remove id (runtime-windows) :key (lambda (w) (getf w :id))))
         (%r! :layers (remove id (runtime-layers) :key (lambda (l) (getf l :id))))
         (%r! :client-geometries
              (remove id (runtime-client-geometries) :key (lambda (entry) (getf entry :id))))
         (push :windows changed)
         (push :layers changed)))
      (:outputs
       (let ((facts (getf event :outputs))
             (connectors (output-connectors event))
             (revision (getf event :revision)))
         (when revision
           (when (< revision (runtime-outputs-revision))
             (return-from dispatch-event))
           (%r! :outputs-revision revision))
         (%r! :outputs facts)
         (%r! :connectors connectors)
         (unless (equal (getf (runtime-effective) :outputs) facts)
           (push :outputs changed))
         (unless (equal (getf (runtime-effective) :connectors) connectors)
           (push :connectors changed))))
      (:input
       (return-from dispatch-event))
      (:key
       (unless (getf event :state) (setf event (list* :state :pressed event)))
       (let ((call (keyboard-call event)))
         (cond
           ((null call) (return-from dispatch-event))
           ((eq (first call) :callback)
            (setf event (callback-event event (second call)))
            (push (getf event :owner) force)
            (push :command changed))
           (t (let ((entry (registered-command (getf (runtime-effective) :commands)
                                               (first call))))
                (unless entry (return-from dispatch-event))
                (when (getf entry :host)
                  (let ((*command-input* t) (failure nil))
                    (%catch (lambda () (host-command-call entry (rest call)))
                            (lambda (text) (setf failure text)))
                    (when failure (record-error failure)))
                  (return-from dispatch-event))
                (setf event (command-event entry (first call) (rest call) event))
                (push (getf entry :owner) force)
                (push :command changed))))))
      (:command
       (push (getf event :owner) force)
       (push :command changed))
      (:ipc
       (push (getf event :owner) force)
       (push :ipc changed))
      (:request
       (when (find (getf event :id) (runtime-windows) :key (lambda (w) (getf w :id)))
         (push :request changed)))
      (:button (push :button changed))
      (:pointer (push :pointer changed))
      (:screenshot (push :screenshot changed))
      (:screencast (push :screencast changed))
      (:grab (push :grab changed))
      (:timer
       (push (getf event :owner) force)
       (setf event (list :type :timer :name (getf event :name)))
       (push :timer changed))
      (:watch
       (push (getf event :owner) force)
       (push :watch changed))
      (:exec
       (push (getf event :owner) force)
       (setf event (%plist-remove :owner event))
       (push :exec changed))
      (:process (push :process changed)))
    (%r! :pending-context
         (union (intersection changed '(:windows :window-geometry :layers :outputs :connectors :services :system))
                (runtime-pending-context)))
    (when (and (or changed (runtime-pending-context))
               (event-matters-p changed force))
      (let ((failure nil))
        (%catch (lambda () (transact (runtime-mounts) event changed force))
                (lambda (text) (setf failure text)))
        (when failure
          (record-error failure)
          (let ((recovery nil))
            (%catch
             (lambda ()
               (%r! :output-recovery-failures
                    (matching-output-failures
                     (append (pending-output-failures failure)
                             (runtime-output-recovery-failures))
                     (getf (runtime-effective) :output-config)
                     (runtime-connectors)))
               (unless (newer-native-output-request-p)
                 (when (runtime-running)
                   (let ((context (materialize (runtime-mounts))))
                     (dolist (key '(:workareas :window-geometry :surfaces :output-errors))
                       (unless (equal (getf context key) (getf (runtime-effective) key))
                         (%r! :pending-context (adjoin key (runtime-pending-context)))))
                     (commit-context (runtime-mounts) context)))))
             (lambda (text) (setf recovery text)))
            (when recovery (record-error recovery))))))))

(defun next-buffer-generation ()
  (let ((next (1+ (%r :window-buffer-generation))))
    (%r! :window-buffer-generation next)
    next))

(defun describe-runtime ()
  (let ((grab (resolved-grab (runtime-mounts))))
    (append (copy-data (loop for (key value) on (runtime-effective) by #'cddr
                             unless (member key '(:surface-plans :native-output-config :output-failures))
                               append (list key value)))
            (list :socket (runtime-socket) :generation (runtime-generation)
                  :x-display (%getenv "DISPLAY")
                  :session-bus (%getenv "DBUS_SESSION_BUS_ADDRESS")
                  :last-error (runtime-last-error)
                  :pending-context (copy-list (runtime-pending-context))
                  :grab (describe-grab grab)
                  :motion-plan (copy-data (runtime-motion-plan))
                  :native-grab (describe-grab (applied-grab))
                  :native-ui (%ui-stats)
                  :native-sound (%sound-stats)
                  :native-errors (%native-errors)
                  :watch (runtime-watch)
                  :timers (%describe :timers)
                  :watches (%describe :watches)
                  :executions (%describe :executions)
                  :managed-processes (%describe :managed-processes)
                  :rule-instances (describe-rule-instances)
                  :patches (describe-patches)
                  :extensions
                  (loop for mounted in (root-mounts (runtime-mounts))
                        for spec = (mounted-spec mounted)
                        collect (list :name (spec-name spec) :source (spec-source spec)
                                      :reads (spec-reads spec) :admission (spec-admission spec)
                                      :state (mounted-state mounted)
                                      :dispatches (mounted-dispatches mounted)
                                      :failures (mounted-failures mounted)
                                      :last-error (mounted-last-error mounted)
                                      :effects (loop for e in (mounted-effects mounted)
                                                     collect (cons (effect-kind e)
                                                                   (effect-arguments e)))))))))

(defun %boot-state (socket)
  (setf *runtime* (%keep (make-runtime-state socket))))

(defun boot-default-config (fallback)
  "Load the embedded default config, the built-in Lisp config."
  (let ((failure nil))
    (%catch (lambda () (load-policy "desktop"))
            (lambda (text) (setf failure text)))
    (when failure
      (report-config-error failure fallback)
      failure)))

(defun TOMOE-BOOT (sources bare watch)
  (let ((*pending-writes* nil))
  (%boot-state nil)
  (let ((trace (%getenv "TOMOE_LAYOUT_TRACE")))
    (setf *layout-trace-path* (and trace (plusp (length trace)) (%keep trace))))
  (%r! :watch (and watch t))
  (cond
    (bare nil)
    ((and sources (consp sources))
     (let ((failure nil))
       (%catch (lambda () (configure sources))
               (lambda (text) (setf failure text)))
       (if failure
           (progn (report-config-error failure t) (boot-default-config t) (%persist-state) failure)
           (progn (%persist-state) nil))))
    (t (let ((failure (boot-default-config t)))
         (%persist-state)
         failure)))))

(defun TOMOE-LOAD (sources changed-source)
  (let ((*pending-writes* nil) (failure nil))
    (%catch (lambda () (configure sources changed-source))
            (lambda (text) (setf failure text)))
    (%persist-state)
    failure))

(defun TOMOE-UNMOUNT (name)
  (let ((*pending-writes* nil) (failure nil))
    (%catch (lambda () (unmount name)) (lambda (text) (setf failure text)))
    (%persist-state)
    failure))

(defun TOMOE-DISPATCH (events)
  (let ((*pending-writes* nil))
  (dolist (event events)
    (cond ((eq (getf event :type) :source)
           (let ((failure nil))
             (%catch (lambda () (configure (runtime-sources) (getf event :path)))
                     (lambda (text) (setf failure text)))
             (when failure (report-config-error failure nil))))
          (t (dispatch-event event))))
  (%persist-state)
  nil))

(defun TOMOE-CONTROL (op arguments)
  (let ((*pending-writes* nil))
  (ecase op
    (:inspect (describe-runtime))
    (:reload (%reload-policy) (%persist-state) nil)
    (:mount (%mount-policy (first arguments)) (%persist-state) nil)
    (:unmount (unload-policy (first arguments)) (%persist-state) nil)
    (:event (dispatch-event (first arguments)) (%persist-state) nil))))

(defun TOMOE-JSON (method params)
  (let ((*pending-writes* nil) (reply nil) (failure nil))
    (%catch (lambda ()
              (setf reply (ipc-request method params))
              (%json-validate-value reply))
            (lambda (text) (setf failure text)))
    (%persist-state)
    (if failure
        (error "~A" failure)
        reply)))

(defun TOMOE-COMMAND (name arguments origin)
  (declare (ignore origin))
  (let ((*pending-writes* nil) (report (run-command name arguments)))
    (%persist-state)
    report))

(defun TOMOE-INSPECT ()
  (describe-runtime))
