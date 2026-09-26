(in-package #:tomoe)

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

(defun default-sysinfo-state ()
  (list :cpu-percent 0 :memory-percent 0))

(defvar *materialized* nil
  "Within a transaction: the mounts, invocation count and context of its last materialization.")
(defvar *invocations* 0
  "Counts accepted reducer results and rule definitions replaced in place.")

(defstruct mounted spec state effects (dispatches 0) (failures 0) last-error
  rule-parent rule-name rule-window rule-definition rule-pending)
(defstruct runtime backend socket sources mounts windows client-geometries outputs connectors effective layers
  (services (list :notifications (list :available nil :notifications nil)
                  :mpris (default-mpris-state) :battery (default-battery-state)
                  :network (default-network-state) :tray (default-tray-state)
                  :audio (default-audio-state) :sysinfo (default-sysinfo-state)))
  notification-producer mpris-producer battery-producer network-producer tray-producer
  (running t) last-error (generation 0) (watch t) source-stamps pending-context output-recovery-failures
  (outputs-revision 0) (window-buffer-generation 0) timers executions retired-executions (execution-turn 0)
  watches (watch-turn 0)
  managed-processes retired-processes once-processes pending-spawns process-history (process-turn 0)
  json-server ipc-call (ipc-sequence 0) screencasts ipc-published-context ipc-published-announcements
  config-error)

(define-condition extension-error (error)
  ((unit :initarg :unit :reader extension-error-unit)
   (cause :initarg :cause :reader extension-error-cause))
  (:report (lambda (condition stream)
             (format stream "Extension ~A: ~A"
                     (extension-error-unit condition) (extension-error-cause condition)))))

(defun canonical-effect (effect)
  (check-type effect effect)
  (let ((args (copy-data (effect-arguments effect))))
    (ecase (effect-kind effect)
      (:method
       (destructuring-bind (name mode value) args
         (ecase mode
           (:state (serve-state name value))
           (:command
            (check-type value string)
            (unless (<= 1 (length value) 256) (error "Invalid IPC command."))
            (%effect :method (list (ipc-name name) :command value))))))
      (:announce (destructuring-bind (name value) args (announce name value)))
      (:data (destructuring-bind (name value) args (publish-state name value)))
      (:settings (apply #'settings args))
      (:keyboard-grab (destructuring-bind (otherwise) args
                        (check-type otherwise (or null string))
                        (%effect :keyboard-grab (list otherwise))))
      (:window-properties (destructuring-bind (id properties) args
                            (apply #'window-properties id properties)))
      (:surface (%effect :surface (canonical-shell-surface args)))
      (:rule
       (destructuring-bind (name app-id title properties reads state) args
         (window-rule name :app-id app-id :title title :properties properties :reads reads :state state
                      :match (effect-predicate effect) :apply (effect-application effect))))
      (:process (destructuring-bind (kind name command cwd env run restart reload) args
                  (ecase kind
                    (:once (unless (and (null restart) (null reload)) (error "Invalid once process policies."))
                           (run-once name command :cwd cwd :env env :run run))
                    (:service (unless (null run) (error "Invalid service run policy."))
                              (service name command :cwd cwd :env env :restart restart :reload reload)))))
      (:exec (destructuring-bind (name command timeout limit) args
               (exec-async name command :timeout timeout :output-limit limit)))
      (:timer (destructuring-bind (name milliseconds repeat) args
                (check-type repeat boolean)
                (funcall (if repeat #'interval #'once) name milliseconds)))
      (:watch (destructuring-bind (name path limit) args
                (watch-file name path :content-limit limit)))
      (:place (destructuring-bind (id x y width height visible) args
                (place id x y width height visible)))
      (:output (apply #'%output-effect args))
      (:keyboard
       (destructuring-bind (rules model layout variant options rate delay) args
         (configure-keyboard :rules rules :model model :layout layout :variant variant :options options
                             :repeat-rate rate :repeat-delay delay)))
      (:view (destructuring-bind (x y zoom) args
               (set-view x y zoom)))
      (:focus (destructuring-bind (id &optional (raise t)) args (focus id :raise raise)))
      (:raise (destructuring-bind (id) args (raise-window id)))
      (:visible (destructuring-bind (id visible) args
                  (check-type visible boolean)
                  (funcall (if visible #'show-window #'hide-window) id)))
      (:layer
       (destructuring-bind (id layer exclusive-zone keyboard visible) args
         (layer id :layer layer :exclusive-zone exclusive-zone
                :keyboard keyboard :visible visible)))
      (:fullscreen (destructuring-bind (id flag) args (fullscreen id flag)))
      (:maximize (destructuring-bind (id flag) args (maximize id flag)))
      (:grab (destructuring-bind (id mode &optional buffer-generation) args
               (grab id mode :buffer-generation buffer-generation)))
      (:bind
       (destructuring-bind (mask keysym command &optional release description) args
         (check-type mask (integer 0 205))
         (unless (zerop (logandc2 mask 205)) (error "Unsupported modifier mask."))
         (check-type keysym string)
         (check-type command string)
         (check-type release (or null string))
         (when (or (find #\Null keysym) (find #\Null command)
                   (zerop (length command)) (zerop (%keysym keysym))
                   (and release (or (zerop (length release)) (find #\Null release))))
           (error "Invalid key binding: ~S" args))
         (check-type description (or null string))
         (%effect :bind (list mask keysym command release description)))))))

(defun canonical-command (command &optional source)
  (check-type command command)
  (let ((args (copy-data (command-arguments command))))
    (ecase (command-kind command)
      (:reply (destructuring-bind (value) args (ipc-reply value)))
      (:broadcast (destructuring-bind (name value) args (broadcast name value)))
      (:launch (apply #'launch args))
      (:spawn (destructuring-bind (launch cwd env) args
                (let ((result (spawn launch :cwd cwd :env env)))
                  (when (and cwd (not (char= (char cwd 0) #\/)))
                    (unless source (error "Relative spawn cwd requires a declaring source."))
                    (setf (second (command-arguments result))
                          (concatenate 'string (directory-namestring source) cwd)))
                  result)))
      (:close (destructuring-bind (id) args (close-window id)))
      (:screenshot (destructuring-bind (screen) args (screenshot (and screen :screen))))
      (:screencast (destructuring-bind (token answer value) args (screencast-answer token answer value)))
      (:power (destructuring-bind (mode name) args (output-power mode name)))
      (:quit (unless (null args) (error "QUIT takes no arguments.")) (quit))
      (:reload (unless (null args) (error "RELOAD takes no arguments.")) (reload)))))

(defun effect-key (effect)
  (ecase (effect-kind effect)
    (:method (list :method (first (effect-arguments effect))))
    (:announce (list :announce (first (effect-arguments effect))))
    (:data (list :data (first (effect-arguments effect))))
    (:rule (list :rule (first (effect-arguments effect))))
    (:process (list :process (second (effect-arguments effect))))
    (:exec (list :exec (first (effect-arguments effect))))
    (:timer (list :timer (first (effect-arguments effect))))
    (:watch (list :watch (first (effect-arguments effect))))
    (:place (list :place (first (effect-arguments effect))))
    (:output (list :output (first (effect-arguments effect))))
    (:keyboard '(:keyboard))
    (:settings '(:settings))
    (:keyboard-grab '(:keyboard-grab))
    (:window-properties (list :window-properties (first (effect-arguments effect))))
    (:view '(:view))
    (:focus '(:focus))
    (:raise (list :raise (first (effect-arguments effect))))
    (:visible (list :visible (first (effect-arguments effect))))
    (:layer (list :layer (first (effect-arguments effect))))
    (:fullscreen (list :fullscreen (first (effect-arguments effect))))
    (:maximize (list :maximize (first (effect-arguments effect))))
    (:grab '(:grab))
    (:surface (list :surface (getf (effect-arguments effect) :name)))
    (:bind (list :bind (first (effect-arguments effect))
                 (%keysym (second (effect-arguments effect)))))))

(defun invoke-extension (runtime mounted context event)
  (let ((spec (mounted-spec mounted)) (reads (mount-context-reads mounted)))
    (handler-case
        (sb-ext:with-timeout (/ (getf (getf context :settings) :watchdog-ms 1000) 1000)
          (multiple-value-bind (state effects commands)
              (funcall (if (mounted-rule-parent mounted)
                           (lambda (snapshot state event)
                             (invoke-rule-application runtime mounted snapshot state event))
                           (spec-update spec))
                       (make-snapshot context (copy-list reads) (runtime-effective runtime))
                       (copy-data (mounted-state mounted)) (copy-data event))
            (when (> (length effects) 512) (error "More than 512 effects."))
            (when (> (length commands) 32) (error "More than 32 commands."))
            (when (and commands
                       (not (or (member (getf event :type) '(:key :button :timer :watch :exec :request :screenshot :screencast :ipc :ui))
                                (and (mounted-rule-parent mounted) (eq (getf event :type) :mount)))))
              (error "One-shot commands require a key, button, timer, watch, exec, request, screenshot, screencast, IPC, or UI command event."))
            (let ((effects (mapcar #'canonical-effect effects))
                  (commands (mapcar (lambda (command) (canonical-command command (spec-source spec))) commands))
                  (keys (make-hash-table :test #'equal)))
              (let ((replies (count :reply commands :key #'command-kind)))
                (when (plusp replies)
                  (unless (and (= replies 1) (eq (getf event :type) :ipc)
                               (equal (getf event :owner) (spec-name spec))
                               (runtime-ipc-call runtime)
                               (eql (getf event :call) (getf (runtime-ipc-call runtime) :token)))
                    (error "IPC-REPLY requires this scope's current IPC request and permits one answer."))))
              (dolist (effect effects)
                (let ((key (effect-key effect)))
                  (when (gethash key keys) (error "Duplicate effect: ~S" key))
                  (setf (gethash key keys) t)))
              (setf (mounted-state mounted) (copy-data state)
                    (mounted-effects mounted) effects)
              (incf *invocations*)
              (when (mounted-rule-parent mounted) (setf (mounted-rule-pending mounted) nil))
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
  "The last owner's grab on a buffered target. A detached or destroyed target
cannot mask another owner's available grab. Registry lifetime is independent."
  (let ((grab nil))
    (dolist (mounted mounts grab)
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
              (setf grab (list id (second args))))))))))

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

(defun %output-scale-120 (runtime x y &optional (output-facts (runtime-outputs runtime)))
  "Return the scale of the screen point, or the first output's scale."
  (let* ((outputs output-facts)
         (output (or (%output-containing-point outputs x y) (first outputs)))
         (scale (and output (getf output :scale-120)))
         (fallback (and (first outputs) (getf (first outputs) :scale-120))))
    (cond ((and (%finite-real-p scale) (plusp scale)) (coerce scale 'double-float))
          ((and (%finite-real-p fallback) (plusp fallback)) (coerce fallback 'double-float))
          (t 120d0))))

(defun %quantize-layout (runtime layout view &optional (output-facts (runtime-outputs runtime)))
  "Apply the physical/client-size round trip after every owner effect settles."
  (dolist (window layout)
    (multiple-value-bind (screen-x screen-y) (%view-screen-point window view)
      (let* ((scale (%output-scale-120 runtime screen-x screen-y output-facts))
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
        (let ((scale (%output-scale-120 runtime screen-x screen-y outputs)))
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

(defun materialize (runtime mounts)
  "Reconstruct owned context. Within a transaction, the same mounts with no
invocation since the last materialization yield that context again."
  (let ((memo *materialized*))
    (if (and memo (eql (second memo) *invocations*)
             (= (length mounts) (length (first memo))) (every #'eq mounts (first memo)))
        (third memo)
        (let ((context (%materialize runtime mounts)))
          (when memo
            (setf (first memo) (copy-list mounts) (second memo) *invocations* (third memo) context))
          context))))

(defun %materialize (runtime mounts)
  "Reconstruct owned context. Preserve live clients, output facts, and map order.
Unmount removes a contribution, exposing the preceding owner or these defaults.
The single grab is not context data; RESOLVED-GRAB derives it from the mounts."
  (setf mounts (prune-rule-mounts runtime mounts))
  (refresh-output-facts runtime)
  (let ((layout (loop for window in (runtime-windows runtime)
                      collect (list :id (getf window :id) :x 0 :y 0
                                    :width (max 1 (getf window :width))
                                    :height (max 1 (getf window :height)) :visible t
                                    :fullscreen nil :maximize nil :properties nil)))
        (stacking (mapcar (lambda (window) (getf window :id)) (runtime-windows runtime)))
        (layers (copy-data (runtime-layers runtime)))
        (focused nil)
        (data nil)
        (surfaces nil)
        (keyboard-grab nil)
        (outputs nil)
        (bindings nil)
        (binding-order 0)
        (keyboard (default-keyboard-config))
        (settings (%settings-defaults +settings+)))
    (dolist (mounted mounts)
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :settings)
          (setf settings (%settings-merge settings (effect-arguments effect) +settings+)))))
    (let ((mod-bit (ecase (getf settings :mod) (:shift 1) (:control 4) (:alt 8) (:super 64)))
        (view (list :x 0 :y 0 :zoom 1d0)))
    (dolist (mounted mounts)
      (dolist (effect (mounted-effects mounted))
        (let ((args (effect-arguments effect)))
          (ecase (effect-kind effect)
            (:surface
             (push (append (list :owner (spec-name (mounted-spec mounted))
                                 :source-id (spec-id (mounted-spec mounted))
                                 :source (spec-source (mounted-spec mounted)))
                           (copy-data args)) surfaces))
            (:data
             (destructuring-bind (name value) args
               (let ((entry (assoc name data)))
                 (if entry (setf (cdr entry) (copy-data value))
                     (setf data (nconc data (list (cons name (copy-data value)))))))))
            (:place
             (destructuring-bind (id x y width height visible) args
               (let ((window (find id layout :key (lambda (w) (getf w :id)))))
                 (when window
                   (when (and visible (not (getf window :visible)))
                     (setf stacking (append (remove id stacking) (list id))))
                   (setf (getf window :x) x (getf window :y) y
                         (getf window :width) width (getf window :height) height
                         (getf window :visible) visible)))))
            (:output
             (destructuring-bind (name mode width height refresh scale x y positioned
                                      &optional disabled mirror vrr) args
               (setf outputs (delete name outputs :test #'equal :key (lambda (o) (getf o :name))))
               (push (list :name name :mode mode :width width :height height
                           :refresh-mhz refresh :scale-120 scale :x x :y y :positioned positioned
                           :disabled disabled :mirror mirror :vrr vrr)
                     outputs)))
            (:view
             (destructuring-bind (x y zoom) args
               (setf view (list :x x :y y :zoom zoom))))
            (:settings nil)
            (:keyboard-grab
             (setf keyboard-grab (list :owner (spec-name (mounted-spec mounted))
                                       :source-id (spec-id (mounted-spec mounted))
                                       :otherwise (first args))))
            (:window-properties
             (destructuring-bind (id properties) args
               (let ((window (find id layout :key (lambda (w) (getf w :id)))))
                 (when window
                   (setf (getf window :properties)
                         (%settings-merge (copy-list (getf window :properties)) properties
                                          '((:border (:group)))))))))
            (:keyboard
             (destructuring-bind (rules model layout variant options rate delay) args
               (setf keyboard (list :rules rules :model model :layout layout :variant variant :options options
                                    :repeat-rate rate :repeat-delay delay))))
            (:focus
             (setf focused (first args))
             (when (and (second args) (member focused stacking))
               (setf stacking (append (remove focused stacking) (list focused)))))
            (:raise
             (let* ((id (first args))
                    (window (find id layout :key (lambda (window) (getf window :id)))))
               (when (and window (getf window :visible))
                 (setf stacking (append (remove id stacking) (list id))))))
            (:visible
             (destructuring-bind (id visible) args
               (let ((window (find id layout :key (lambda (window) (getf window :id)))))
                 (when window
                   (when (and visible (not (getf window :visible)))
                     (setf stacking (append (remove id stacking) (list id))))
                   (setf (getf window :visible) visible)))))
            (:layer
             (destructuring-bind (id layer exclusive-zone keyboard visible) args
               (let ((record (find id layers :key (lambda (l) (getf l :id)))))
                 (when record
                   (when layer (setf (getf record :layer) layer))
                   (when exclusive-zone (setf (getf record :exclusive-zone) exclusive-zone))
                   (when keyboard (setf (getf record :keyboard) keyboard))
                   (setf (getf record :visible) (and visible t))))))
            (:fullscreen
             (destructuring-bind (id flag) args
               (let ((window (find id layout :key (lambda (w) (getf w :id)))))
                 (when window (setf (getf window :fullscreen) (and flag t))))))
            (:maximize
             (destructuring-bind (id flag) args
               (let ((window (find id layout :key (lambda (w) (getf w :id)))))
                 (when window (setf (getf window :maximize) (and flag t))))))
            ((:grab :timer :watch :exec :process :rule :method :announce) nil)
            (:bind
             (destructuring-bind (mask keysym command release &optional description) args
               (let ((code (%keysym keysym)) (declared mask)
                     (mask (if (logtest 128 mask) (logior (logandc2 mask 128) mod-bit) mask)))
                 (setf bindings
                       (delete-if (lambda (b) (and (= mask (getf b :modifiers))
                                                   (= code (getf b :code)))) bindings))
                 (push (list :modifiers mask :code code :keysym keysym
                             :owner (spec-name (mounted-spec mounted)) :command command :release release
                             :source-id (spec-id (mounted-spec mounted))
                             :description description :declared declared
                             :order (incf binding-order))
                       bindings))))))))
    (let* ((live-connectors (or (runtime-connectors runtime)
                                (output-connectors (list :outputs (runtime-outputs runtime)))))
           (output-config (default-output-config outputs live-connectors (getf settings :scale)))
           (connected
             (connected-output-config
              (list :connectors live-connectors :output-config output-config)))
           (output-resolution
             (multiple-value-list (resolve-output-preview runtime connected live-connectors)))
           (output-preview (first output-resolution))
           (native-output-config (second output-resolution))
           (output-failures (third output-resolution))
           (output-facts (getf output-preview :outputs))
           (connector-facts (output-connectors output-preview)))
      (%quantize-layout runtime layout view output-facts)
      (multiple-value-bind (resolved-layers workareas)
          (if (runtime-backend runtime)
              (preview-native-layers (runtime-backend runtime) output-facts layers
                                     (resolved-layer-overrides mounts))
              (values layers (full-workareas output-facts)))
        (unless (find focused layout :key (lambda (w) (getf w :id)))
          (setf focused nil))
        (multiple-value-bind (surface-plans shell-workareas)
            (resolve-shell-surfaces (nreverse surfaces) output-facts workareas
                                    (when (runtime-backend runtime)
                                      (native-ui-asset-loader (runtime-backend runtime))))
        (list :windows (runtime-windows runtime)
              :window-geometry (resolved-window-geometry runtime layout view output-facts)
              :rules (resolved-rule-properties runtime mounts) :data data
              :services (copy-data (runtime-services runtime))
              :outputs output-facts :connectors connector-facts
              :output-config output-config :native-output-config native-output-config
              :output-failures output-failures
              :output-errors (loop for failure in output-failures collect
                               (list :name (getf failure :name) :message (getf failure :message)))
              :workareas shell-workareas
              :surface-plans (copy-data surface-plans)
              :surfaces (loop for plan in surface-plans
                              collect (loop for key in '(:owner :source-id :name :output :x :y :width :height :layer)
                                            append (list key (copy-data (getf plan key)))))
              :view view :layout layout
              :stacking (remove-if-not
                         (lambda (id) (getf (find id layout :key (lambda (window) (getf window :id))) :visible))
                         stacking)
              :layers resolved-layers :focus focused :keyboard keyboard :settings settings
              :config-error (copy-data (runtime-config-error runtime))
              :keyboard-grab keyboard-grab
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
                                   :disabled nil :mirror nil :vrr nil))))
          #'string< :key (lambda (o) (getf o :name)))))

(defun changed-keys (before after)
  (remove-if (lambda (key) (equal (getf before key) (getf after key))) +context-keys+))

(defun connected-output-config (context)
  (remove-if-not (lambda (config)
                   (find (getf config :name) (output-connectors context)
                         :test #'equal :key (lambda (o) (getf o :name))))
                 (getf context :output-config)))

(defun commit-context (runtime mounts context &optional commands)
  "The only extension-to-native write path. All reducers have returned and validated."
  (setf mounts (prune-rule-mounts runtime mounts))
  (let ((watches (prepare-watches runtime mounts)))
    (unwind-protect (%commit-context runtime mounts context commands watches)
      (discard-watch-plan watches))))

(defun %commit-context (runtime mounts context commands watches)
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
         (grab-changed (not (equal (getf old :keyboard-grab) (getf context :keyboard-grab))))
         (overrides (resolved-layer-overrides mounts))
         (grab (resolved-grab runtime mounts))
         (timers (prepare-timers runtime mounts))
         (executions (prepare-executions runtime mounts))
         (processes (prepare-managed-processes runtime mounts))
         (spawns (prepare-session-spawns runtime commands processes))
         (pending-spawns (append (remove nil spawns) (runtime-pending-spawns runtime))))
    (when (or (member :outputs (runtime-pending-context runtime))
              outputs-changed bindings-changed keyboard-changed settings-changed grab-changed
              restack layout-changed focus-changed
              (not (equal (getf old :outputs) (getf context :outputs)))
              (not (equal (getf old :view) (getf context :view)))
              (not (equal (getf old :layers) (getf context :layers)))
              (not (equal (getf old :surface-plans) (getf context :surface-plans)))
              (not (equal (resolved-layer-overrides (runtime-mounts runtime)) overrides))
              (not (equal grab (applied-grab backend))))
      (configure-native-presentation backend outputs context overrides restack
                                     outputs-changed bindings-changed grab keyboard-changed
                                     settings-changed))
    (when outputs-changed
      (setf (runtime-outputs runtime) (getf context :outputs)
            (runtime-connectors runtime) (getf context :connectors))
      (unless (pending-native-outputs-p backend)
        (setf (runtime-outputs-revision runtime) (%outputs-revision backend))))
    (setf (runtime-mounts runtime) mounts (runtime-effective runtime) context
          (runtime-output-recovery-failures runtime) nil)
    (install-watches runtime watches)
    (install-timers runtime timers)
    (install-executions runtime executions)
    (install-managed-processes runtime processes)
    (setf (runtime-pending-spawns runtime) pending-spawns)
    (when (runtime-json-server runtime) (publish-json-context runtime context mounts))
    spawns))

(defun transact (runtime mounts event changed &optional force)
  (unwind-protect (%transact runtime mounts event changed force)
    (discard-ui-asset-scratch runtime)))

(defun discard-ui-asset-scratch (runtime)
  (when (runtime-backend runtime)
    (%ui-assets-discard (runtime-backend runtime))))

(defun %transact (runtime mounts event changed &optional force)
  "Stage reducers and dependency propagation before touching the native scene.
A bad callback or a dependency cycle discards the entire candidate transaction.
Pending external invalidations join the current event, never an event replay."
  (let* ((*materialized* (list nil -1 nil))
         (candidate (prune-rule-mounts
                     runtime (mapcar #'copy-mounted
                                     (append (root-mounts mounts)
                                             (rule-instances (runtime-mounts runtime))))))
         (context (materialize runtime candidate))
         (dirty (union (runtime-pending-context runtime)
                       (union changed (changed-keys (runtime-effective runtime) context))))
         (commands nil)
         (deferred-deliveries (make-hash-table :test #'eq))
         (proposal-baselines
           (let ((states (make-hash-table :test #'eq)))
             (dolist (mounted candidate states)
               (setf (gethash (mounted-spec mounted) states) (copy-data (mounted-state mounted))))))
         (proposal-deliveries (make-hash-table :test #'eq))
         (initial-event event)
         (initial-force force))
    (labels ((invoke (mounted delivered)
               (when (admission-scope-p mounted candidate)
                 (let ((spec (mounted-spec mounted)))
                   (multiple-value-bind (baseline existed) (gethash spec proposal-baselines)
                     (setf (mounted-state mounted)
                           (copy-data (if existed baseline
                                          (sixth (effect-arguments (mounted-rule-definition mounted)))))))
                   (multiple-value-bind (original delivered-p) (gethash spec proposal-deliveries)
                     (if delivered-p (setf delivered original)
                         (setf (gethash spec proposal-deliveries) (copy-data delivered))))
                   (setf commands (delete spec commands :key #'car))))
               (setf commands
                     (nconc commands
                            (mapcar (lambda (command) (cons (mounted-spec mounted) command))
                                    (invoke-extension runtime mounted context delivered))))))
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
              (reconcile-rule-mounts runtime candidate context)
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
                                       ((or (and (member (getf event :type) '(:timer :watch :exec :ipc :ui))
                                                 (not (member name force :test #'equal)))
                                            (and (eq (getf event :type) :key)
                                                 (not (equal (getf event :owner) name)))
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
          (let* ((next (materialize runtime candidate)) (changes (changed-keys before next)))
            (setf context next dirty changes event (list :type :change :keys changes))
            (unless (or changes force)
              (let* ((commands (loop for (owner . command) in commands
                                     when (find owner candidate :key #'mounted-spec) collect command))
                     (spawns (commit-context runtime candidate context commands)))
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

(defun load-specs (path)
  (let* ((*source* (namestring (truename path)))
         (*definitions* nil) (*package* (find-package :tomoe-user)) (*read-eval* nil)
         (*read-default-float-format* 'double-float))
    (sb-ext:with-timeout 1
      (with-open-file (stream *source*)
        (when (> (file-length stream) 1048576) (error "Extension file exceeds 1 MiB."))
        (load stream :verbose nil :print nil)))
    (nreverse *definitions*)))

(defun configure (runtime sources &optional changed-source)
  (let* ((specs (loop for path in sources
                      when (or (null changed-source) (equal path changed-source)) append (load-specs path)))
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
                              (make-mounted :spec spec :state (copy-data (spec-initial spec)))))))
      (transact runtime
                (loop for source in sources append
                  (remove source (append retained mounts) :test-not #'equal
                          :key (lambda (m) (spec-source (mounted-spec m)))))
                '(:type :mount) nil (mapcar #'spec-name specs)))
    (setf (runtime-sources runtime) (copy-list sources) (runtime-last-error runtime) nil
          (runtime-source-stamps runtime)
          (loop for path in sources collect (cons path (source-stamp path))))
    (incf (runtime-generation runtime))))

(defun unmount (runtime name)
  (unless (find name (root-mounts (runtime-mounts runtime)) :test #'equal
                :key (lambda (m) (spec-name (mounted-spec m))))
    (error "No mounted extension named ~A." name))
  (transact runtime
            (remove name (runtime-mounts runtime) :test #'equal
                    :key (lambda (m) (spec-name (mounted-spec m))))
            (list :type :unmount :name name) nil)
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
          (dispatch-event runtime (read-data text))))))
  runtime)

(defun dispatch-event (runtime event)
  (let ((changed nil) (force nil))
    (ecase (getf event :type)
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
       (return-from dispatch-event))
      ((:map :metadata)
       (let* ((id (getf event :id))
              (old (find id (runtime-windows runtime) :key (lambda (w) (getf w :id))))
              (new (list :id id :title (getf event :title) :app-id (getf event :app-id)
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
             (setf (getf new :buffered) (and (getf event :attached) t)
                   (getf new :fullscreen) (and (getf event :fullscreen) t)
                   (getf new :maximize) (and (getf event :maximize) t))
             (when (observe-client-geometry runtime id (getf event :width) (getf event :height))
               (push :window-geometry changed))
             (unless (equal old new)
               (setf (runtime-windows runtime) (substitute new old (runtime-windows runtime)))
               (push :windows changed))))))
      (:geometry
       (when (and (find (getf event :id) (runtime-windows runtime)
                        :key (lambda (window) (getf window :id)))
                  (observe-client-geometry runtime (getf event :id)
                                           (getf event :width) (getf event :height)))
         (push :window-geometry changed)))
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
       (when (getf event :binding-id)
         (let* ((owner (find (getf event :owner) (runtime-mounts runtime) :test #'equal
                             :key (lambda (mount) (spec-name (mounted-spec mount)))))
                (source-id (and owner (spec-id (mounted-spec owner)))))
           (unless (and source-id (eql source-id (getf event :source-id))
                        (runtime-backend runtime)
                        (typep (getf event :binding-id) '(integer 1 18446744073709551615))
                        (= 1 (%binding-current (runtime-backend runtime) (getf event :binding-id))))
             (return-from dispatch-event))))
       (push :key changed))
      (:request
       (when (find (getf event :id) (runtime-windows runtime) :key (lambda (w) (getf w :id)))
         (push :request changed)))
      (:button (push :button changed))
      (:pointer (push :pointer changed))
      (:screenshot (push :screenshot changed))
      (:screencast (push :screencast changed))
      (:grab (push :grab changed)))
    (setf (runtime-pending-context runtime)
          (union (intersection changed '(:windows :window-geometry :layers :outputs :connectors :services))
                 (runtime-pending-context runtime)))
    (when (or changed (runtime-pending-context runtime))
      (handler-case (transact runtime (runtime-mounts runtime) event changed force)
        (serious-condition (condition)
          (record-error runtime condition)
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
              (discard-ui-asset-scratch runtime))))))))

(defun describe-runtime (runtime)
  (let ((grab (resolved-grab runtime (runtime-mounts runtime))))
    (append (copy-data (loop for (key value) on (runtime-effective runtime) by #'cddr
                            unless (member key '(:surface-plans :native-output-config :output-failures))
                              append (list key value)))
            (list :socket (runtime-socket runtime) :generation (runtime-generation runtime)
                  :x-display (sb-ext:posix-getenv "DISPLAY")
                  :last-error (runtime-last-error runtime)
                  :pending-context (copy-list (runtime-pending-context runtime))
                  :grab (describe-grab grab)
                  :native-grab (describe-grab (applied-grab (runtime-backend runtime)))
                  :native-ui (when (runtime-backend runtime)
                               (read-data (%ui-stats (runtime-backend runtime))))
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
                  :extensions
                  (loop for m in (root-mounts (runtime-mounts runtime)) for spec = (mounted-spec m)
                        collect (list :name (spec-name spec) :source (spec-source spec)
                                      :reads (spec-reads spec) :admission (spec-admission spec) :state (mounted-state m)
                                      :dispatches (mounted-dispatches m)
                                      :failures (mounted-failures m)
                                      :last-error (mounted-last-error m)
                                      :effects (loop for e in (mounted-effects m)
                                                     collect (cons (effect-kind e)
                                                                   (effect-arguments e)))))))))
