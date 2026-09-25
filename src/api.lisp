(defpackage #:tomoe
  (:use #:cl)
  (:export #:define-extension #:context #:previous-context #:place #:focus #:bind-key #:configure-output #:configure-keyboard
           #:window-rule #:rules-for #:raise-window #:show-window #:hide-window
           #:publish-state #:state-value #:service-state #:window-geometry
           #:ui #:shell-surface
           #:json-object #:json-array #:json-get #:+json-false+
           #:serve-state #:serve-method #:announce #:broadcast #:ipc-reply
           #:layer #:fullscreen #:maximize #:grab #:set-view
           #:once #:interval #:watch-file #:exec-async #:run-once #:service #:spawn #:launch #:close-window #:quit #:reload))
(defpackage #:tomoe-user (:use #:cl #:tomoe))
(in-package #:tomoe)

(define-condition output-configuration-error (simple-error)
  ((output :initarg :output :initform nil :reader output-error-name)))

(defconstant +wire-version+ 1)
(defconstant +native-abi-version+ 23)
(defparameter +context-keys+
  '(:windows :window-geometry :rules :data :services :outputs :connectors :output-config :output-errors :workareas :view :layout :stacking :focus :bindings :keyboard :layers :surfaces :key :button :grab :request :ipc :ui))
(defvar *definitions* :not-loading)
(defvar *source*)
(defvar *stop-requested* nil)
(defvar *backend-kind* :native
  "Which backend contract is loaded: :native for the wlroots shim, :lisp for the
pure-Lisp backend, which sets this when its sources load.")

(defun %finite-float-p (value)
  "True when VALUE is an IEEE float with a finite value."
  (and (floatp value)
       (not (sb-ext:float-nan-p value))
       (not (sb-ext:float-infinity-p value))))

(defun %finite-real-p (value)
  (and (realp value)
       (or (not (floatp value)) (%finite-float-p value))))

(defun %double-float (value)
  "Coerce a finite real to a finite double-float, or signal a type error."
  (unless (%finite-real-p value)
    (error "Expected a finite real number, got ~S." value))
  (let ((result (handler-case (coerce value 'double-float)
                  (error () nil))))
    (unless (and result (%finite-float-p result))
      (error "Real number cannot be represented as a finite double-float: ~S."
             value))
    result))

(defun %round-away-positive (value)
  "Round a non-negative REAL away from zero at an exact half."
  (floor (+ (coerce value 'double-float) 0.5d0)))

(defun copy-data (value)
  "Copy bounded Lisp data. No closures, host handles, shared strings, or cycles cross dispatch."
  (let ((nodes 0))
    (labels ((walk (item depth)
               (when (or (> (incf nodes) 32768) (> depth 64))
                 (error "Extension data exceeds the node/depth budget."))
               (typecase item
                 (null nil)
                 (cons
                  (let ((items nil) (tail item))
                    (loop while (consp tail) do
                      (when (> (incf nodes) 32768) (error "Extension data exceeds the node budget."))
                      (push (walk (car tail) (1+ depth)) items)
                      (setf tail (cdr tail)))
                    (nreconc items (walk tail (1+ depth)))))
                 (string
                  (when (> (length item) 65536) (error "String exceeds 65536 characters."))
                  (copy-seq item))
                 (integer
                  (unless (<= (- (expt 2 63)) item (1- (expt 2 64)))
                    (error "Integer outside the data range."))
                  item)
                 (ratio
                  (unless (and (<= (- (expt 2 63)) (numerator item) (1- (expt 2 64)))
                               (<= 1 (denominator item) (1- (expt 2 64))))
                    (error "Rational outside the data range."))
                  item)
                 (float
                  (unless (%finite-float-p item)
                    (error "Only finite floats are extension data."))
                  item)
                 (symbol
                  (unless (or (eq item t) (keywordp item))
                    (error "Only keywords, T, and NIL are data symbols: ~S" item))
                  item)
                 (t (error "Not extension data: ~S" (type-of item))))))
      (walk value 0))))

(defstruct (snapshot (:constructor make-snapshot (data reads &optional previous)))
  (data nil :read-only t) (reads nil :read-only t) (previous nil :read-only t))
(defun context (snapshot key)
  "Read a declared context key. Values belong to this invocation, not the host."
  (unless (member key (snapshot-reads snapshot))
    (error "Undeclared context dependency: ~S" key))
  (getf (snapshot-data snapshot) key))

(defun previous-context (snapshot key)
  "Read the context before this transaction. Shares CONTEXT's declared reads;
unlike CONTEXT, this value is stable while candidate dependencies settle."
  (unless (member key (snapshot-reads snapshot))
    (error "Undeclared context dependency: ~S" key))
  (getf (snapshot-previous snapshot) key))

(defstruct (effect (:constructor %effect (kind arguments &optional predicate application)))
  (kind nil :read-only t) (arguments nil :read-only t)
  (predicate nil :read-only t) (application nil :read-only t))
(defstruct (command (:constructor %command (kind arguments)))
  (kind nil :read-only t) (arguments nil :read-only t))

(defun ipc-name (name)
  (check-type name string)
  (unless (<= 1 (length name) 256) (error "IPC name must contain 1 through 256 characters."))
  (write-json name)
  (copy-seq name))

(defun ipc-value (value)
  (let* ((value (copy-data value)) (text (write-json value)))
    (when (> (length (sb-ext:string-to-octets text :external-format :utf-8)) 1044480)
      (error "IPC payload exceeds the frame budget."))
    (copy-data (list :json-object (list (cons "event" "") (cons "payload" value))))
    value))

(defun serve-state (method value)
  "Own a JSON method returning VALUE. Later owners replace the method."
  (%effect :method (list (ipc-name method) :state (ipc-value value))))

(defun serve-method (method command)
  "Own a JSON method that privately delivers :IPC to this reducer. COMMAND is
a keyword like a binding command; the event also carries :METHOD and :PARAMS.
Return IPC-REPLY among commands to answer; the default result is JSON null."
  (check-type command keyword)
  (%effect :method (list (ipc-name method) :command (string-downcase command))))

(defun announce (event value)
  "Own an event snapshot. Publish changes only after successful commit.
Replacing the winning source announces again; withdrawal restores the preceding
owner or announces JSON null when no owner remains."
  (%effect :announce (list (ipc-name event) (ipc-value value))))

(defun broadcast (event value)
  "Send one JSON event after successful publication. Subscription filters apply."
  (%command :broadcast (list (ipc-name event) (ipc-value value))))

(defun ipc-reply (value)
  "Answer this reducer's current :IPC request after successful publication."
  (%command :reply (list (ipc-value value))))

(defun publish-state (name value)
  "Own a named value in :DATA. Later owners replace the entire value, including
NIL. Omission or unmount exposes the preceding owner. Values are bounded data."
  (check-type name keyword)
  (unless (<= 1 (length (symbol-name name)) 80) (error "Invalid state name."))
  (%effect :data (list name (copy-data value))))

(defun state-value (snapshot name &optional default)
  "Read a named published value. Declare :DATA as a context dependency."
  (check-type name keyword)
  (let ((entry (assoc name (context snapshot :data))))
    (copy-data (if entry (cdr entry) default))))

(defun service-state (snapshot name &optional default)
  "Read copied session service facts. Declare :SERVICES as a dependency.
Producer lifetimes belong to the session; removing a consumer releases only its
own effects. :NOTIFICATIONS supplies :AVAILABLE and a :NOTIFICATIONS list."
  (check-type name keyword)
  (copy-data (getf (context snapshot :services) name default)))

(defun rule-reserved-property-p (key)
  (or (member key '(:app-id :app_id :title :match :apply))
      (and (stringp key) (member key '("app_id" "title" "match" "apply") :test #'string=))))

(defun window-rule (name &key app-id title match properties reads state apply)
  "Own a named window rule. APP-ID and TITLE are case-sensitive Lua patterns.
PROPERTIES is an alist of copied data; later matching rules replace equal keys.
MATCH, when present, receives (window snapshot). APPLY is a per-window reducer:
(window snapshot state event) -> state, owned effects, one-shot commands.
Each matching window has independent state/resources. New matches and source
reloads invoke APPLY with :MOUNT; losing the match removes the instance."
  (check-type name keyword)
  (unless (<= 1 (length (symbol-name name)) 80) (error "Invalid rule name."))
  (dolist (pattern (list app-id title))
    (check-type pattern (or null string))
    (when (and pattern (> (length pattern) 65536)) (error "Rule pattern exceeds 65536 characters.")))
  (check-type match (or null function))
  (check-type apply (or null function))
  (let ((properties (copy-data properties)) (reads (copy-data reads)))
    (unless (and (listp properties) (<= (length properties) 512))
      (error "Rule properties must be an alist of at most 512 entries."))
    (dolist (entry properties)
      (unless (consp entry) (error "Rule properties must be (key . value) pairs.")))
    (unless (and (listp reads) (every (lambda (key) (member key +context-keys+)) reads))
      (error "Invalid rule context dependencies: ~S" reads))
    (%effect :rule (list name (when app-id (copy-seq app-id)) (when title (copy-seq title))
                         (remove-if (lambda (entry) (rule-reserved-property-p (car entry))) properties)
                         (remove-duplicates reads) (copy-data state))
             match apply)))

(defun rules-for (snapshot window)
  "Return the copied property alist for WINDOW (an id or window snapshot).
Declare :RULES in the calling extension's context dependencies."
  (let ((id (if (listp window) (getf window :id) window)))
    (check-type id (integer 1 4294967295))
    (copy-data (getf (find id (context snapshot :rules)
                          :key (lambda (record) (getf record :id))) :properties))))

(defun window-geometry (snapshot window)
  "Read a visible window's physical rectangle, or NIL if hidden/unknown.
Declare :WINDOW-GEOMETRY. Size comes from committed client geometry; location,
visibility and scale follow the candidate layout/view/outputs. :LAYOUT instead
contains the requested, quantized placement size. WINDOW is an ID or window record."
  (let ((id (if (listp window) (getf window :id) window)))
    (check-type id (integer 1 4294967295))
    (let ((record (find id (context snapshot :window-geometry)
                        :key (lambda (record) (getf record :id)))))
      (when record
        (copy-data (loop for key in '(:x :y :width :height)
                         append (list key (getf record key))))))))

(defun once (name milliseconds)
  "Own a one-shot timer. Deliver (:TYPE :TIMER :NAME NAME) to this reducer.
An unchanged effect fires once per mount/source reload; omit it to cancel."
  (check-type name keyword)
  (check-type milliseconds (integer 0 2147483647))
  (%effect :timer (list name milliseconds nil)))

(defun interval (name milliseconds)
  "Own a repeating timer. Zero starts immediately, then repeats every millisecond.
An unchanged effect retains its schedule. Omit it to cancel."
  (check-type name keyword)
  (check-type milliseconds (integer 0 2147483647))
  (%effect :timer (list name milliseconds t)))

(defun watch-file (name path &key (content-limit 65536))
  "Own file-change notifications delivered privately as :WATCH events.
PATH is relative to the declaring source. CONTENT-LIMIT bounds UTF-8 bytes.
An unchanged declaration retains its watch. Omit it to cancel."
  (check-type name keyword)
  (check-type path string)
  (unless (and (<= 1 (length path) 65536) (not (find #\Null path)))
    (error "Invalid file watch path."))
  (check-type content-limit (integer 1 65536))
  (%effect :watch (list name (copy-seq path) content-limit)))

(defun exec-async (name command &key (timeout 30000) (output-limit 65536))
  "Own one shell command and receive a private :EXEC completion event.
An unchanged declaration runs once per source generation. Omitting it cancels.
TIMEOUT is milliseconds; OUTPUT-LIMIT bounds combined stdout/stderr bytes."
  (check-type name keyword)
  (check-type command string)
  (when (or (> (length command) 65536) (find #\Null command))
    (error "Invalid async command string."))
  (check-type timeout (integer 1 2147483647))
  (check-type output-limit (integer 1 65536))
  (%effect :exec (list name (copy-seq command) timeout output-limit)))

(defun process-options (command cwd env)
  "Validate and canonicalize a shell string or argv, directory and environment."
  (let ((size 0) (keys (make-hash-table :test #'equal)))
    (flet ((string-option (value)
             (check-type value string)
             (when (or (find #\Null value) (> (incf size (length value)) 65536))
               (error "Process options exceed 65536 characters or contain NUL."))
             (copy-seq value)))
      (let ((launch (if (stringp command) (string-option command)
                        (progn
                          (unless (and (listp command) (<= 1 (length command) 128))
                            (error "Process argv must contain 1 through 128 strings."))
                          (when (zerop (length (first command))) (error "Empty process executable."))
                          (mapcar #'string-option command))))
            (directory (when cwd (string-option cwd)))
            (environment nil))
        (when (and directory (zerop (length directory))) (error "Empty process directory."))
        (unless (and (listp env) (<= (length env) 64)) (error "More than 64 environment overrides."))
        (dolist (entry env)
          (unless (consp entry) (error "Environment entries must be (name . value) pairs."))
          (let ((name (string-option (car entry))) (value (string-option (cdr entry))))
            (when (or (zerop (length name)) (find #\= name) (gethash name keys))
              (error "Invalid or duplicate environment name: ~S" name))
            (setf (gethash name keys) t)
            (push (cons name value) environment)))
        (values launch directory (sort environment #'string< :key #'car))))))

(defun run-once (name command &key cwd env (run :once-per-session))
  "Declare a one-shot session launch. COMMAND is a shell string or argv list.
RUN is :ONCE-PER-SESSION or :ONCE-PER-CONFIG-VERSION. A started child survives unmount."
  (check-type name keyword)
  (check-type run (member :once-per-session :once-per-config-version))
  (multiple-value-bind (launch directory environment) (process-options command cwd env)
    (%effect :process (list :once name launch directory environment run nil nil))))

(defun service (name command &key cwd env (restart :on-exit) (reload :keep-if-unchanged))
  "Own a supervised process. Equal declarations can survive successful reload.
RESTART is :NEVER, :ON-FAILURE or :ON-EXIT; RELOAD can also be :ALWAYS-RESTART."
  (check-type name keyword)
  (check-type restart (member :never :on-failure :on-exit))
  (check-type reload (member :keep-if-unchanged :always-restart))
  (multiple-value-bind (launch directory environment) (process-options command cwd env)
    (%effect :process (list :service name launch directory environment nil restart reload))))

(defun place (id x y width height &optional (visible t))
  (check-type id (integer 1 4294967295))
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (check-type width (integer 1 16384))
  (check-type height (integer 1 16384))
  (check-type visible boolean)
  (%effect :place (list id x y width height visible)))

(defun set-view (x y &optional (zoom 1))
  "Own the camera over the physical window canvas.
X and Y are bounded physical offsets; ZOOM is finite, converted to a double,
and clamped to the native camera's range."
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (let* ((zoom (%double-float zoom))
         (zoom (max (/ 1d0 16d0) (min 16d0 zoom))))
    (%effect :view (list x y zoom))))

(defun %output-effect (name mode width height refresh scale x y positioned
                       &optional disabled mirror vrr)
  (check-type name string)
  (unless (and (<= 1 (length name) 128) (not (find #\Null name)))
    (error "Invalid output name: ~S" name))
  (check-type mode (member :preferred :max :exact))
  (if (eq mode :exact)
      (progn (check-type width (integer 1 16384)) (check-type height (integer 1 16384)))
      (unless (and (eql width 0) (eql height 0)) (error "Only exact modes take dimensions.")))
  (check-type refresh (integer 0 1000000))
  (check-type scale (integer 30 960))
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (check-type positioned boolean)
  (check-type disabled boolean)
  (check-type vrr boolean)
  (when mirror
    (check-type mirror string)
    (unless (and (<= 1 (length mirror) 128) (not (find #\Null mirror)))
      (error "Invalid mirror output name: ~S" mirror)))
  (%effect :output (list (copy-seq name) mode width height refresh scale x y positioned
                        disabled (and mirror (copy-seq mirror)) vrr)))

(defun configure-output (name &key (mode :preferred) (scale 1) position disabled mirror vrr)
  "Own an output's mode, scale, position, enablement, mirror target and VRR request.
Refresh is Hz; omit it for maximum. MIRROR names an active non-mirroring output.
Disabled connectors remain discoverable in :CONNECTORS, outside active :OUTPUTS."
  (check-type scale (real 1/4 8))
  (destructuring-bind (x y) (or position '(0 0))
    (if (member mode '(:preferred :max))
        (%output-effect name mode 0 0 0 (%round-away-positive (* scale 120))
                        x y (not (null position)) disabled mirror vrr)
        (destructuring-bind (width height &optional (refresh 0)) mode
          (check-type refresh (real 0 1000))
          (%output-effect name :exact width height (round (* refresh 1000))
                          (%round-away-positive (* scale 120)) x y (not (null position))
                          disabled mirror vrr)))))

(defun focus (id &key (raise t))
  "Own keyboard focus. RAISE also contributes this window's stacking order."
  (check-type id (or null (integer 1 4294967295)))
  (check-type raise boolean)
  (%effect :focus (list id raise)))

(defun raise-window (id)
  "Own a raise in the managed window stack without changing focus or geometry."
  (check-type id (integer 1 4294967295))
  (%effect :raise (list id)))

(defun show-window (id)
  "Own visibility without replacing another owner's geometry."
  (check-type id (integer 1 4294967295))
  (%effect :visible (list id t)))

(defun hide-window (id)
  "Own invisibility while retaining the window and its geometry contributions."
  (check-type id (integer 1 4294967295))
  (%effect :visible (list id nil)))

(defun configure-keyboard (&key (rules "") (model "") (layout "") (variant "") options
                                (repeat-rate 25) (repeat-delay 600))
  "Own the seat's XKB keymap and repeat policy, including future keyboards.
Empty names use XKB defaults. NIL options use XKB_DEFAULT_OPTIONS; an empty
options string disables those defaults. The last declaration wins as a whole; omission
restores the preceding owner, or the session defaults (25 Hz, 600 ms)."
  (check-type repeat-rate (integer 0 2147483647))
  (check-type repeat-delay (integer 1 2147483647))
  (let ((names (list rules model layout variant)))
    (check-type options (or null string))
    (when options (setf names (append names (list options))))
    (dolist (name names)
      (check-type name string)
      (when (or (> (length name) 1024) (find #\Null name))
        (error "Keyboard names must contain at most 1024 characters and no NUL.")))
    (%effect :keyboard (list (copy-seq rules) (copy-seq model) (copy-seq layout) (copy-seq variant)
                             (when options (copy-seq options)) repeat-rate repeat-delay))))

(defun default-keyboard-config ()
  (list :rules "" :model "" :layout "" :variant "" :options nil :repeat-rate 25 :repeat-delay 600))
(defun layer (id &key layer exclusive-zone keyboard (visible t))
  "Own a layer surface's stacking, exclusive zone, keyboard mode, and visibility.
NIL fields inherit earlier owners, then the client's request; :VISIBLE defaults to shown."
  (check-type id (integer 1 4294967295))
  (check-type layer (member nil :background :bottom :top :overlay))
  (check-type exclusive-zone (or null (integer 0 4096)))
  (check-type keyboard (member nil :none :exclusive :on-demand))
  (check-type visible boolean)
  (%effect :layer (list id layer exclusive-zone keyboard visible)))
(defun fullscreen (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :fullscreen (list id flag)))
(defun maximize (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :maximize (list id flag)))
(defun grab (id mode &key buffer-generation)
  "Own a pointer grab, optionally restricted to one window buffer lifetime."
  (check-type id (integer 1 4294967295))
  (check-type mode (member :move :resize))
  (check-type buffer-generation (or null (integer 1 *)))
  (%effect :grab (if buffer-generation (list id mode buffer-generation) (list id mode))))
(defun bind-key (modifiers keysym command &key release)
  "Own a shortcut, optionally with a command for its physical key release.
Release follows the original device/key even after modifiers change. Removing
or replacing the binding/source cancels that callback and still swallows key-up."
  (check-type keysym string)
  (check-type command keyword)
  (check-type release (or null keyword))
  (when (or (zerop (length keysym)) (find #\Null keysym)) (error "Invalid keysym name."))
  (let ((mask 0))
    (dolist (modifier modifiers)
      (setf mask (logior mask (ecase modifier (:shift 1) (:control 4) (:alt 8) (:super 64)))))
    (%effect :bind (list mask (copy-seq keysym) (string-downcase command)
                         (when release (string-downcase release))))))
(defun launch (&rest argv)
  "Launch literal argv once after acceptance; the session owns the child."
  (multiple-value-bind (command cwd env) (process-options argv nil nil)
    (declare (ignore cwd env))
    (%command :launch command)))
(defun spawn (command &key cwd env)
  "Launch a shell string or literal argv once after the current transaction.
CWD is relative to the declaring source. ENV overlays the child environment.
The session owns the child, and native launches receive an activation token."
  (multiple-value-bind (launch directory environment) (process-options command cwd env)
    (%command :spawn (list launch directory environment))))
(defun close-window (id)
  (check-type id (integer 1 4294967295))
  (%command :close (list id)))
(defun quit () (%command :quit nil))
(defun reload () (%command :reload nil))

(defvar *next-source-id* 0)
(defun allocate-source-id ()
  (when (>= *next-source-id* (1- (expt 2 64))) (error "Source identity space exhausted."))
  (incf *next-source-id*))

(defstruct (spec (:copier nil)) name reads initial update source admission (token (gensym "SOURCE-"))
  (id (allocate-source-id) :read-only t))
(defun copy-spec (spec)
  "Copy a declaration into a fresh source generation; COPY-MOUNTED retains one."
  (make-spec :name (spec-name spec) :reads (spec-reads spec) :initial (spec-initial spec)
             :update (spec-update spec) :source (spec-source spec) :admission (spec-admission spec)))
(defun register-spec (name reads initial update &optional admission)
  (when (eq *definitions* :not-loading) (error "Load declarations with MOUNT or RELOAD."))
  (check-type name string)
  (check-type admission boolean)
  (unless (and (<= 1 (length name) 80)
               (every (lambda (c) (or (find c "-_./") (alphanumericp c))) name))
    (error "Invalid extension name: ~S" name))
  (dolist (key reads)
    (unless (member key +context-keys+) (error "Unknown dependency: ~S" key)))
  (when (find name *definitions* :key #'spec-name :test #'equal)
    (error "Duplicate extension: ~A" name))
  (push (make-spec :name (copy-seq name) :reads (copy-list reads)
                   :initial (copy-data initial) :update update :source *source* :admission admission)
        *definitions*))

(defmacro define-extension (name (&key reads state admission) (snapshot state-name event) &body body)
  "Declare a pure reducer. Return state, owned effects, and one-shot commands.
With :ADMISSION T, evaluate after ordinary reducers/rules, recalculating from
the transaction's starting private state and event on every dependency round.
Only the final proposal's state, effects and commands are accepted."
  `(register-spec ,name ',reads ,state
                  (lambda (,snapshot ,state-name ,event) ,@body) ,admission))
