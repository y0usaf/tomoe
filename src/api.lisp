(defpackage #:tomoe
  (:use #:cl)
  (:export #:define-extension #:context #:previous-context #:place #:focus #:bind-key #:configure-output #:virtual-output #:configure-keyboard #:screenshot #:screencast-answer #:media-control #:adjust-brightness #:clipboard-copy
           #:window-rule #:rules-for #:raise-window #:show-window #:hide-window
           #:publish-state #:state-value #:service-state #:window-geometry
           #:ui #:shell-surface
           #:json-object #:json-array #:json-get #:+json-false+
           #:serve-state #:serve-method #:announce #:broadcast #:ipc-reply
           #:layer #:fullscreen #:maximize #:grab #:set-view #:settings #:setting #:motion-contract
           #:bind-button #:bind-scroll #:window-properties #:keyboard-grab #:sound
           #:define-command #:invoke
           #:confirm-dialog #:menu-dialog #:menu-choice #:sheet-dialog #:toast
           #:theme #:bar-layout #:workspaces-widget #:clock-text #:+theme+ #:+theme-presets+
           #:frame-ms #:clock-boundary-ms #:once #:interval #:watch-file #:exec-async #:run-once #:service #:spawn #:launch #:close-window #:output-power #:quit #:reload))
(defpackage #:tomoe-user (:use #:cl #:tomoe))
(in-package #:tomoe)

(declaim (optimize (speed 3) (safety 1) (debug 1)))

(define-condition output-configuration-error (simple-error)
  ((output :initarg :output :initform nil :reader output-error-name)))

(defconstant +wire-version+ 2)
(defconstant +native-abi-version+ 42)
(defparameter +context-keys+
  '(:windows :window-geometry :rules :data :services :outputs :connectors :output-config :output-errors :config-error :workareas :view :layout :stacking :focus :bindings :commands :keyboard :settings :layers :surfaces :command :button :pointer :grab :request :screenshot :screencast :ipc :ui :activity :system))
(defvar *definitions* :not-loading)
(defvar *source*)
(defvar *source-directory*)
(defvar *stop-requested* nil)

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
    (declare (type fixnum nodes))
    (labels ((walk (item depth)
               (declare (type fixnum depth))
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
  (data nil :read-only t) (reads nil :read-only t) (previous nil :read-only t)
  (copies nil) (previous-copies nil))
(defun context (snapshot key)
  "Read a declared context key. Values belong to this invocation, not the host:
a key is copied on its first read, and later reads return that copy."
  (unless (member key (snapshot-reads snapshot))
    (error "Undeclared context dependency: ~S" key))
  (let ((entry (assoc key (snapshot-copies snapshot))))
    (if entry
        (cdr entry)
        (cdar (push (cons key (copy-data (getf (snapshot-data snapshot) key)))
                    (snapshot-copies snapshot))))))

(defun previous-context (snapshot key)
  "Read the context before this transaction. Shares CONTEXT's declared reads;
unlike CONTEXT, this value is stable while candidate dependencies settle."
  (unless (member key (snapshot-reads snapshot))
    (error "Undeclared context dependency: ~S" key))
  (let ((entry (assoc key (snapshot-previous-copies snapshot))))
    (if entry
        (cdr entry)
        (cdar (push (cons key (copy-data (getf (snapshot-previous snapshot) key)))
                    (snapshot-previous-copies snapshot))))))

(defstruct (effect (:constructor %effect (kind arguments &optional predicate application)))
  (kind nil :read-only t) (arguments nil :read-only t)
  (predicate nil :read-only t) (application nil :read-only t))
(defstruct (command (:constructor %command (kind arguments)))
  (kind nil :read-only t) (arguments nil :read-only t))

(defvar *effect-definitions* (make-hash-table :test #'eq))

(defstruct (effect-definition (:constructor %make-effect-definition (canonical key reduce phase)))
  canonical key reduce phase)

(defmacro define-effect (kind &key canonical key reduce (phase 1))
  "Define effect KIND once. CANONICAL rebuilds the effect from its copied ARGS,
and EFFECT, through the public constructor; KEY names the slot it owns,
by default KIND and the first argument; REDUCE folds ARGS from MOUNTED into
the materialization M. Reducers run in PHASE order, then mount order."
  `(setf (gethash ,kind *effect-definitions*)
         (%make-effect-definition
          (lambda (args effect) (declare (ignorable args effect)) ,canonical)
          ,(when key `(lambda (args) (declare (ignorable args)) ,key))
          ,(when reduce `(lambda (m args mounted) (declare (ignorable m args mounted)) ,reduce))
          ,phase)))

(defun %effect-definition (kind)
  (or (gethash kind *effect-definitions*) (error "Unknown effect kind: ~S" kind)))

(defstruct materialization
  layout stacking layers focused data surfaces keyboard-grab outputs bindings commands
  (binding-order 0) keyboard settings view mod-bit sounds virtual-outputs)

(defun %materialized-window (m id)
  (find id (materialization-layout m) :key (lambda (window) (getf window :id))))

(defun %materialized-raise (m id)
  (setf (materialization-stacking m) (append (remove id (materialization-stacking m)) (list id))))

(defun ipc-name (name)
  (check-type name string)
  (unless (<= 1 (length name) 256) (error "IPC name must contain 1 through 256 characters."))
  (write-json name)
  (copy-seq name))

(defun ipc-value (value)
  (let ((value (copy-data value)))
    (%json-validate-value value)
    (when (> (%json-output-size value 0 nil) 1044480)
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

(define-effect :method
  :canonical (destructuring-bind (name mode value) args
               (ecase mode
                 (:state (serve-state name value))
                 (:command
                  (check-type value string)
                  (unless (<= 1 (length value) 256) (error "Invalid IPC command."))
                  (%effect :method (list (ipc-name name) :command value))))))

(defun announce (event value)
  "Own an event snapshot. Publish changes only after successful commit.
Replacing the winning source announces again; withdrawal restores the preceding
owner or announces JSON null when no owner remains."
  (%effect :announce (list (ipc-name event) (ipc-value value))))

(define-effect :announce
  :canonical (destructuring-bind (name value) args (announce name value)))

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

(define-effect :data
  :canonical (destructuring-bind (name value) args (publish-state name value))
  :reduce (destructuring-bind (name value) args
            (let ((entry (assoc name (materialization-data m))))
              (if entry (setf (cdr entry) (copy-data value))
                  (setf (materialization-data m)
                        (nconc (materialization-data m) (list (cons name (copy-data value)))))))))

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

(defun shader-path (path)
  "Absolute window shader path, or one relative to the declaring source."
  (check-type path string)
  (when (or (zerop (length path)) (> (length path) 4096) (find #\Null path))
    (error "Invalid window shader path."))
  (cond ((char= (char path 0) #\/) path)
        ((and (stringp *source-directory*) (plusp (length *source-directory*))
              (char= (char *source-directory* 0) #\/))
         (concatenate 'string *source-directory* path))
        (t (error "Relative window shader paths require a declaring source."))))

(defun shader-entry (entry)
  "Validate a GLSL function name, since the native renderer splices it in."
  (check-type entry string)
  (unless (and (<= 1 (length entry) 63)
               (alpha-char-p (char entry 0))
               (every (lambda (character)
                        (or (alpha-char-p character) (digit-char-p character)
                            (char= character #\_)))
                      entry))
    (error "Invalid window shader entry ~S." entry))
  entry)

(defun rule-reserved-property-p (key)
  (or (member key '(:app-id :app_id :title :match :apply))
      (and (stringp key) (member key '("app_id" "title" "match" "apply") :test #'string=))))

(defun window-rule (name &key app-id title match properties reads state apply)
  "Own a named window rule. APP-ID must equal the app id; TITLE must occur in the title.
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

(define-effect :rule
  :canonical (destructuring-bind (name app-id title properties reads state) args
               (window-rule name :app-id app-id :title title :properties properties :reads reads :state state
                            :match (effect-predicate effect) :apply (effect-application effect))))

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

(defvar *frame-ms* 16
  "Milliseconds between display refreshes on the fastest active output.
The runtime keeps it current; read it with FRAME-MS.")

(defun frame-ms ()
  "Milliseconds between display refreshes, following the monitor.
240 Hz reads 4 and 60 Hz reads 16. Hand it to INTERVAL, or pass :REFRESH,
and the animation that timer advances steps once per refresh instead of on a
fixed clock."
  *frame-ms*)

(defun frame-period-ms (outputs)
  "Milliseconds per refresh of the fastest enabled output in OUTPUTS."
  (let ((fastest 0))
    (dolist (output outputs)
      (let ((refresh (getf output :refresh-mhz)))
        (when (and (integerp refresh) (plusp refresh) (> refresh fastest))
          (setf fastest refresh))))
    (if (plusp fastest) (max 1 (floor 1000000 fastest)) 16)))

(defun %timer-milliseconds (declared)
  "Resolve a timer's declared period.
:REFRESH follows the display, :SECOND and :MINUTE follow the wall clock."
  (case declared
    (:refresh (frame-ms))
    (:second 1000)
    (:minute 60000)
    (t declared)))

(defun %timer-clock-step (declared)
  "The wall-clock step of a declared period, in seconds, or NIL."
  (case declared (:second 1) (:minute 60) (t nil)))

(defun clock-boundary-ms (step)
  "Milliseconds from now to the next multiple of STEP seconds on the wall clock.
A timer that ticks on it changes its output at the same instant the clock does."
  (check-type step (integer 1 86400))
  (let* ((units internal-time-units-per-second)
         (fraction (mod (get-internal-real-time) units))
         (elapsed (floor (* 1000 fraction) units))
         (left (- step (mod (get-universal-time) step))))
    (max 1 (- (* left 1000) elapsed))))

(defun once (name milliseconds)
  "Own a one-shot timer. Deliver (:TYPE :TIMER :NAME NAME) to this reducer.
MILLISECONDS may be :REFRESH (the next display refresh), :SECOND or :MINUTE
(the next wall-clock boundary of that length).
An unchanged effect fires once per mount/source reload; omit it to cancel."
  (check-type name keyword)
  (check-type milliseconds (or (integer 0 2147483647) (member :refresh :second :minute)))
  (%effect :timer (list name milliseconds nil)))

(defun interval (name milliseconds)
  "Own a repeating timer. Zero starts immediately, then repeats every millisecond.
MILLISECONDS may instead name a clock: :REFRESH repeats once per display refresh
and follows the monitor's rate, while :SECOND and :MINUTE repeat on the
wall-clock boundary, so a clock display updates when the time actually changes.
An unchanged effect retains its schedule. Omit it to cancel."
  (check-type name keyword)
  (check-type milliseconds (or (integer 0 2147483647) (member :refresh :second :minute)))
  (%effect :timer (list name milliseconds t)))

(define-effect :timer
  :canonical (destructuring-bind (name milliseconds repeat) args
               (check-type repeat boolean)
               (funcall (if repeat #'interval #'once) name milliseconds)))

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

(define-effect :watch
  :canonical (destructuring-bind (name path limit) args
               (watch-file name path :content-limit limit)))

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

(define-effect :exec
  :canonical (destructuring-bind (name command timeout limit) args
               (exec-async name command :timeout timeout :output-limit limit)))

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

(defun %process-command (name arguments)
  (check-type name keyword)
  (if (oddp (length arguments))
      (values (first arguments) (rest arguments))
      (values (list (string-downcase name)) arguments)))

(defun run-once (name &rest arguments)
  "Declare a one-shot session launch: (run-once name [command] &key cwd env run).
COMMAND is a shell string or argv list and defaults to NAME as the program.
RUN is :ONCE-PER-SESSION or :ONCE-PER-CONFIG-VERSION. A started child survives unmount."
  (multiple-value-bind (command options) (%process-command name arguments)
    (destructuring-bind (&key cwd env (run :once-per-session)) options
      (check-type run (member :once-per-session :once-per-config-version))
      (multiple-value-bind (launch directory environment) (process-options command cwd env)
        (%effect :process (list :once name launch directory environment run nil nil))))))

(defun service (name &rest arguments)
  "Own a supervised process: (service name [command] &key cwd env restart reload).
COMMAND defaults to NAME as the program. Equal declarations can survive successful reload.
RESTART is :NEVER, :ON-FAILURE or :ON-EXIT; RELOAD can also be :ALWAYS-RESTART."
  (multiple-value-bind (command options) (%process-command name arguments)
    (destructuring-bind (&key cwd env (restart :on-exit) (reload :keep-if-unchanged)) options
      (check-type restart (member :never :on-failure :on-exit))
      (check-type reload (member :keep-if-unchanged :always-restart))
      (multiple-value-bind (launch directory environment) (process-options command cwd env)
        (%effect :process (list :service name launch directory environment nil restart reload))))))

(define-effect :process
  :canonical (destructuring-bind (kind name command cwd env run restart reload) args
               (ecase kind
                 (:once (unless (and (null restart) (null reload)) (error "Invalid once process policies."))
                        (run-once name command :cwd cwd :env env :run run))
                 (:service (unless (null run) (error "Invalid service run policy."))
                           (service name command :cwd cwd :env env :restart restart :reload reload))))
  :key (list :process (second args)))

(defun place (id x y width height &optional (visible t))
  (check-type id (integer 1 4294967295))
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (check-type width (integer 1 16384))
  (check-type height (integer 1 16384))
  (check-type visible boolean)
  (%effect :place (list id x y width height visible)))

(define-effect :place
  :canonical (destructuring-bind (id x y width height visible) args
               (place id x y width height visible))
  :reduce (destructuring-bind (id x y width height visible) args
            (let ((window (%materialized-window m id)))
              (when window
                (when (and visible (not (getf window :visible)))
                  (%materialized-raise m id))
                (setf (getf window :x) x (getf window :y) y
                      (getf window :width) width (getf window :height) height
                      (getf window :visible) visible)))))

(defun set-view (x y &optional (zoom 1))
  "Own the camera over the physical window canvas.
X and Y are bounded physical offsets; ZOOM is finite, converted to a double,
and clamped to the native camera's range."
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (let* ((zoom (%double-float zoom))
         (zoom (max (/ 1d0 16d0) (min 16d0 zoom))))
    (%effect :view (list x y zoom))))

(define-effect :view
  :canonical (destructuring-bind (x y zoom) args (set-view x y zoom))
  :key '(:view)
  :reduce (destructuring-bind (x y zoom) args
            (setf (materialization-view m) (list :x x :y y :zoom zoom))))

(defparameter +output-transforms+
  '(:normal :90 :180 :270 :flipped :flipped-90 :flipped-180 :flipped-270))

(defun %output-effect (name mode width height refresh scale x y positioned
                       &optional disabled mirror vrr icc transform tearing)
  (check-type name string)
  (unless (and (<= 1 (length name) 128) (not (find #\Null name)))
    (error "Invalid output name: ~S" name))
  (check-type mode (member :preferred :max :exact))
  (if (eq mode :exact)
      (progn (check-type width (integer 1 16384)) (check-type height (integer 1 16384)))
      (unless (and (eql width 0) (eql height 0)) (error "Only exact modes take dimensions.")))
  (check-type refresh (integer -1 1000000))
  (check-type scale (or null (integer 30 960)))
  (check-type x (integer -1048576 1048576))
  (check-type y (integer -1048576 1048576))
  (check-type positioned boolean)
  (check-type disabled boolean)
  (check-type vrr boolean)
  (check-type tearing boolean)
  (check-type transform (or null (integer 0 7)))
  (when mirror
    (check-type mirror string)
    (unless (and (<= 1 (length mirror) 128) (not (find #\Null mirror)))
      (error "Invalid mirror output name: ~S" mirror)))
  (when icc
    (check-type icc string)
    (unless (and (<= 1 (length icc) 4096) (not (find #\Null icc)))
      (error "Invalid ICC profile path: ~S" icc)))
  (%effect :output (list (copy-seq name) mode width height refresh scale x y positioned
                        disabled (and mirror (copy-seq mirror)) vrr (and icc (copy-seq icc)) transform
                        tearing)))

(defun configure-output (name &key (mode :preferred) refresh scale position disabled mirror vrr icc transform
                                tearing)
  "Own an output's mode, scale, position, transform, enablement, mirror target, VRR request, ICC profile
and tearing.
MODE is :PREFERRED, :MAX (largest progressive), or (WIDTH HEIGHT [HZ]). REFRESH
is :MAX or Hz, matched within 1 Hz. Without it :PREFERRED keeps the preferred
mode and other sizes take their highest rate. MIRROR names an active non-mirroring output.
Omitted SCALE inherits the :SCALE setting. ICC names an RGB display profile that
maps sRGB content to the panel's colors. TRANSFORM is one of +OUTPUT-TRANSFORMS+.
TEARING shows each composited frame as soon as it is drawn, with an async page flip
instead of waiting for the vblank, where the driver accepts one; frames may tear.
Tomoe's own animations still draw at most once a refresh.
Disabled connectors remain discoverable in :CONNECTORS, outside active :OUTPUTS."
  (check-type scale (or null (real 1/4 8)))
  (flet ((millihertz (refresh)
           (cond ((null refresh) 0) ((eq refresh :max) -1)
                 (t (check-type refresh (real 1 1000)) (round (* refresh 1000))))))
    (let ((scale-120 (and scale (%round-away-positive (* scale 120))))
          (transform (and transform (or (position transform +output-transforms+)
                                        (error "Unknown output transform: ~S" transform)))))
      (destructuring-bind (x y) (or position '(0 0))
        (if (member mode '(:preferred :max))
            (%output-effect name mode 0 0 (millihertz refresh) scale-120
                            x y (not (null position)) disabled mirror vrr icc transform tearing)
            (destructuring-bind (width height &optional hz) mode
              (%output-effect name :exact width height (millihertz (or hz refresh)) scale-120
                              x y (not (null position)) disabled mirror vrr icc transform tearing)))))))

(define-effect :output
  :canonical (apply #'%output-effect args)
  :reduce (destructuring-bind (name mode width height refresh scale x y positioned
                                    &optional disabled mirror vrr icc transform tearing) args
            (setf (materialization-outputs m)
                  (delete name (materialization-outputs m) :test #'equal :key (lambda (o) (getf o :name))))
            (push (list :name name :mode mode :width width :height height
                        :refresh-mhz refresh :scale-120 scale :x x :y y :positioned positioned
                        :disabled disabled :mirror mirror :vrr vrr :icc icc :transform transform
                        :tearing tearing)
                  (materialization-outputs m))))

(defun virtual-output (name &key (mode '(1920 1080 60)))
  "Own an offscreen output NAME showing MODE, (WIDTH HEIGHT [HZ]) at 60 Hz by
default. It joins :CONNECTORS and :OUTPUTS like a plugged-in monitor, and
CONFIGURE-OUTPUT sets its scale, position, transform or mode. Removal unplugs it."
  (check-type name string)
  (unless (and (<= 1 (length name) 128) (not (find #\Null name)))
    (error "Invalid output name: ~S" name))
  (destructuring-bind (width height &optional (hz 60)) mode
    (check-type width (integer 1 16384))
    (check-type height (integer 1 16384))
    (check-type hz (real 1 1000))
    (%effect :virtual-output (list (copy-seq name) width height (round (* hz 1000))))))

(define-effect :virtual-output
  :canonical (destructuring-bind (name width height refresh) args
               (virtual-output name :mode (list width height (/ refresh 1000))))
  :reduce (destructuring-bind (name width height refresh) args
            (setf (materialization-virtual-outputs m)
                  (cons (list :name name :width width :height height :refresh-mhz refresh)
                        (remove name (materialization-virtual-outputs m)
                                :test #'equal :key (lambda (o) (getf o :name)))))))

(defun focus (id &key (raise t))
  "Own keyboard focus. RAISE also contributes this window's stacking order."
  (check-type id (or null (integer 1 4294967295)))
  (check-type raise boolean)
  (%effect :focus (list id raise)))

(define-effect :focus
  :canonical (destructuring-bind (id &optional (raise t)) args (focus id :raise raise))
  :key '(:focus)
  :reduce (progn
            (setf (materialization-focused m) (first args))
            (when (and (second args) (member (materialization-focused m) (materialization-stacking m)))
              (%materialized-raise m (materialization-focused m)))))

(defun window-properties (id &rest properties &key radius tearing blur border alpha tint scale
                          rotation translate shader shader-grid shader-margin shader-settle
                          shader-entry mesh mesh-grid)
  "Own rendering and compositing overrides for one window. Omitted keys fall back
to the settings; :TEARING and :BLUR take T or NIL, and a supplied NIL denies.
BORDER is (:FOCUSED color :UNFOCUSED color). ALPHA multiplies the whole window,
TINT mixes a color (its alpha is the strength) over its contents, SCALE and
ROTATION (degrees) transform it about its centre, and TRANSLATE is (:X n :Y n)
offset from where the layout puts it. Each of those animates towards its new
value with the matching :ANIMATIONS setting. SHADER is a GLSL file drawn over the
window's own texture on a :SHADER-GRID mesh; :SHADER-MARGIN is the extra canvas
around the window in pixels, :SHADER-SETTLE how long it keeps animating after the
window moves, and :SHADER-ENTRY names the shader function to call (default
mainImage). MESH deforms the window's draw grid instead of drawing it as a
rectangle, and takes over the grid a SHADER would otherwise use: :MESH-GRID is
(COLUMNS . ROWS), and MESH is COLUMNS+1 times ROWS+1 X Y pixel pairs relative to
the window's placed corner with row 0 on its top edge, so column 0 and row 0 hold
the corners that show the window's own top-left corner. Pointer input follows the
same mesh, so a click lands on the pixel a warp moved. Later owners replace each
key."
  (declare (ignore radius tearing blur border alpha tint scale rotation translate
                   shader shader-grid shader-margin shader-settle shader-entry
                   mesh mesh-grid))
  (check-type id (integer 1 4294967295))
  (%effect :window-properties
           (list id (%settings-plist properties
                                     '((:radius (:integer 0 4096)) (:tearing :boolean) (:blur :boolean)
                                       (:border (:group (:focused :color) (:unfocused :color)))
                                       (:alpha (:real 0 1)) (:tint :color)
                                       (:scale (:real 1/100 16)) (:rotation (:real -360 360))
                                       (:translate (:group (:x (:integer -16384 16384) 0)
                                                           (:y (:integer -16384 16384) 0)))
                                       (:shader :path) (:shader-grid (:pair 1 32))
                                       (:shader-margin (:integer 0 4096))
                                       (:shader-settle (:real 0 10))
                                       (:shader-entry :identifier)
                                       (:mesh-grid (:pair 1 32))
                                       (:mesh (:floats 0 8192)))))))

(define-effect :window-properties
  :canonical (destructuring-bind (id properties) args
               (apply #'window-properties id properties))
  :reduce (destructuring-bind (id properties) args
            (let ((window (%materialized-window m id)))
              (when window
                (setf (getf window :properties)
                      (%settings-merge (copy-list (getf window :properties)) properties
                                       '((:border (:group)) (:translate (:group)))))))))

(defun raise-window (id)
  "Own a raise in the managed window stack without changing focus or geometry."
  (check-type id (integer 1 4294967295))
  (%effect :raise (list id)))

(define-effect :raise
  :canonical (destructuring-bind (id) args (raise-window id))
  :reduce (let ((window (%materialized-window m (first args))))
            (when (and window (getf window :visible))
              (%materialized-raise m (first args)))))

(defun show-window (id)
  "Own visibility without replacing another owner's geometry."
  (check-type id (integer 1 4294967295))
  (%effect :visible (list id t)))

(defun hide-window (id)
  "Own invisibility while retaining the window and its geometry contributions."
  (check-type id (integer 1 4294967295))
  (%effect :visible (list id nil)))

(define-effect :visible
  :canonical (destructuring-bind (id visible) args
               (check-type visible boolean)
               (funcall (if visible #'show-window #'hide-window) id))
  :reduce (destructuring-bind (id visible) args
            (let ((window (%materialized-window m id)))
              (when window
                (when (and visible (not (getf window :visible)))
                  (%materialized-raise m id))
                (setf (getf window :visible) visible)))))

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

(define-effect :keyboard
  :canonical (destructuring-bind (rules model layout variant options rate delay) args
               (configure-keyboard :rules rules :model model :layout layout :variant variant :options options
                                   :repeat-rate rate :repeat-delay delay))
  :key '(:keyboard)
  :reduce (destructuring-bind (rules model layout variant options rate delay) args
            (setf (materialization-keyboard m)
                  (list :rules rules :model model :layout layout :variant variant :options options
                        :repeat-rate rate :repeat-delay delay))))

(defparameter +settings+
  '((:scale (:real 1/4 8) 1)
    (:mod (:member :super :alt :control :shift) :super)
    (:focus-follows-mouse :boolean nil)
    (:pointer-follows-focus :boolean nil)
    (:tearing :boolean nil)
    (:wait-for-frame-completion :boolean nil)
    (:nested-size (:group (:width (:integer 1 16384) 1280) (:height (:integer 1 16384) 800)))
    (:touchpad :device nil)
    (:mouse :device nil)
    (:devices :devices nil)
    (:border (:group (:width (:integer 0 1024) 2) (:focused :color "#7aa2f7")
                     (:unfocused :color "#3b4261") (:radius (:integer 0 4096) 0)))
    (:shadow (:group (:range (:integer 0 4096) 12) (:color :color "#00000099") (:power (:real 1 4) 3)))
    (:blur (:group (:enabled :boolean nil) (:passes (:integer 1 31) 3) (:offset (:real 0 1000) 1)
                   (:anti-artifact-margin (:integer 0 4096) 96) (:layer-namespaces :strings nil)))
    (:animations :animations t)
    (:screenshot-freeze :boolean t)
    (:screenshot-colors (:group (:accent :color "#7aa1f7") (:background :color "#1c1c21")
                                (:text :color "#ffffff") (:chip :color "#45454f")))
    (:watchdog-ms (:integer 0 60000) 1000)
    (:force-server-side-decorations :boolean nil)
    (:honor-xdg-activation-with-invalid-serial :boolean nil))
  "Compositor settings: (key type default). A :group type holds its own table.")

(defparameter +input-device-settings+
  '((:disabled :boolean) (:disabled-on-external-mouse :boolean) (:tap :boolean)
    (:tap-drag :boolean) (:tap-drag-lock :boolean) (:natural-scroll :boolean)
    (:accel-speed (:real -1 1)) (:accel-profile (:member :flat :adaptive)) (:dwt :boolean)
    (:left-handed :boolean) (:middle-emulation :boolean)
    (:scroll-method (:member :none :two-finger :edge :on-button-down))
    (:scroll-button (:integer 0 4294967295)) (:click-method (:member :button-areas :clickfinger)))
  "libinput device fields. Unset fields keep the device's libinput default.")

(defun %setting-value (type value key)
  (flet ((bad () (error "Invalid setting ~S: ~S" key value)))
    (ecase (if (consp type) (first type) type)
      (:boolean (unless (typep value 'boolean) (bad)) value)
      (:integer (unless (typep value `(integer ,(second type) ,(third type))) (bad)) value)
      (:real (unless (and (%finite-real-p value) (<= (second type) value (third type))) (bad))
       (%double-float value))
      (:member (unless (member value (rest type)) (bad)) value)
      (:path (if (null value) "" (shader-path value)))
      (:pair (unless (and (consp value) (typep (car value) `(integer ,(second type) ,(third type)))
                          (typep (cdr value) `(integer ,(second type) ,(third type))))
               (bad))
       (cons (car value) (cdr value)))
      (:identifier (shader-entry value))
      (:floats (unless (and (listp value) (<= (second type) (length value) (third type))
                            (every #'%finite-real-p value))
                 (bad))
       (mapcar #'%double-float value))
      (:color (%ui-color value) (copy-seq value))
      (:strings (unless (and (listp value) (<= (length value) 64) (every #'stringp value)) (bad))
       (mapcar #'copy-seq value))
      (:group (%settings-plist value (rest type) key))
      (:animations (%animation-settings value))
      (:device (%settings-plist value +input-device-settings+ key))
      (:devices
       (unless (and (listp value) (<= (length value) 64)) (bad))
       (loop for entry in value
             do (unless (and (consp entry) (stringp (car entry)) (<= 1 (length (car entry)) 256)) (bad))
             collect (cons (copy-seq (car entry))
                           (%settings-plist (cdr entry) +input-device-settings+ key)))))))

(defun %settings-plist (plist table &optional context)
  (unless (and (listp plist) (evenp (length plist)))
    (error "Settings~@[ for ~S~] need key/value pairs: ~S" context plist))
  (let ((seen nil))
    (loop for (key value) on plist by #'cddr
          for entry = (or (assoc key table) (error "Unknown setting ~S~@[ in ~S~]." key context))
          do (when (member key seen) (error "Duplicate setting ~S." key))
             (push key seen)
          append (list key (%setting-value (second entry) value key)))))

(defun %animation-spec (value default)
  "Normalize NIL (off), T (DEFAULT), (:SPRING plist) or (:EASE plist).
A spring settles when it stays within EPSILON of its target; the old
1/10000 epsilon made the 800 stiffness spring run for 326 ms. The shipped
defaults are calibrated by settle time: 16000 stiffness at a 1/1000 epsilon
settles in 73 ms (within 1% of the travel at 52 ms), and the open ease is
90 ms."
  (flet ((bad () (error "Invalid animation: ~S" value)))
    (cond ((null value) (list :kind :off))
          ((eq value t) (%animation-spec default default))
          ((and (consp value) (eq (first value) :kind)
                (member (second value) '(:off :spring :ease)))
           (copy-list value))
          ((not (and (consp value) (member (first value) '(:spring :ease)) (= 2 (length value)))) (bad))
          ((eq (first value) :spring)
           (destructuring-bind (&key (damping-ratio 1) (stiffness 16000) (epsilon 1/1000))
               (%settings-plist (second value) '((:damping-ratio (:real 0 1000)) (:stiffness (:real 0 100000))
                                                 (:epsilon (:real 0 1))) :spring)
             (list :kind :spring :damping-ratio (%double-float damping-ratio)
                   :stiffness (%double-float stiffness) :epsilon (%double-float epsilon))))
          (t
           (destructuring-bind (&key (duration-ms 90) (curve :ease-out-expo)) (second value)
             (unless (typep duration-ms '(integer 1 600000)) (bad))
             (unless (or (member curve '(:linear :ease-out-quad :ease-out-cubic :ease-out-expo))
                         (and (listp curve) (= 4 (length curve)) (every #'%finite-real-p curve)))
               (bad))
             (list :kind :ease :duration-ms duration-ms
                   :curve (if (keywordp curve) curve (mapcar #'%double-float curve))))))))

(defparameter +animation-defaults+
  '(:window-move (:spring (:stiffness 16000 :epsilon 1/1000))
    :window-open (:ease (:duration-ms 90 :curve :ease-out-expo))
    :window-close nil :window-resize nil :window-switch nil
    :window-view (:spring (:stiffness 16000 :epsilon 1/1000))
    :window-alpha nil :window-tint nil :window-scale nil
    :window-rotation nil :window-translate nil)
  "Each animation event's default, when a configuration does not name one.
Every animation advances once per display refresh, at the instant the frame
reaches the screen; there is no tick to tune. :WINDOW-MOVE settles in 73 ms
and :WINDOW-VIEW (workspace and deck scrolling) does too; :WINDOW-OPEN lasts
90 ms.")

(defun %animation-settings (value)
  "Normalize :ANIMATIONS: T, NIL, or a plist of per-event specs.
T and NIL speak for every event; a plist leaves the events it omits to the
preceding owner or the default."
  (unless (or (member value '(t nil)) (and (listp value) (evenp (length value))))
    (error "Invalid animations: ~S" value))
  (if (member value '(t nil))
      (loop for (key default) on +animation-defaults+ by #'cddr
            append (list key (%animation-spec value default)))
      (loop for (key spec) on value by #'cddr
            do (unless (member key +animation-defaults+)
                 (error "Unknown animation: ~S" key))
            append (list key (%animation-spec spec (getf +animation-defaults+ key))))))

(defun settings (&rest plist)
  "Own compositor settings. Later owners replace individual keys; grouped keys
merge field by field. Omission restores the preceding owner or the default."
  (%effect :settings (%settings-plist plist +settings+)))

(define-effect :settings
  :canonical (apply #'settings args)
  :key '(:settings)
  :reduce (setf (materialization-settings m) (%settings-merge (materialization-settings m) args +settings+))
  :phase 0)

(defun %settings-defaults (table)
  (loop for (key type default) in table
        append (list key (cond ((and (consp type) (eq (first type) :group)) (%settings-defaults (rest type)))
                               ((eq type :animations) (%animation-settings default))
                               (t default)))))

(defun %settings-merge (settings plist table)
  (loop for (key value) on plist by #'cddr
        for type = (second (assoc key table))
        do (setf (getf settings key)
                 (cond ((and (consp type) (eq (first type) :group))
                        (%settings-merge (copy-list (getf settings key)) value (rest type)))
                       ((eq type :animations)
                        (let ((merged (copy-list (getf settings key))))
                          (loop for (event spec) on value by #'cddr
                                do (setf (getf merged event) spec))
                          merged))
                       (t value))))
  settings)

(defun setting (snapshot &rest path)
  "Read a resolved compositor setting. Declare :SETTINGS as a dependency."
  (let ((value (context snapshot :settings)))
    (dolist (key path (copy-data value)) (setf value (getf value key)))))

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

(define-effect :layer
  :canonical (destructuring-bind (id layer exclusive-zone keyboard visible) args
               (layer id :layer layer :exclusive-zone exclusive-zone
                      :keyboard keyboard :visible visible))
  :reduce (destructuring-bind (id layer exclusive-zone keyboard visible) args
            (let ((record (find id (materialization-layers m) :key (lambda (l) (getf l :id)))))
              (when record
                (when layer (setf (getf record :layer) layer))
                (when exclusive-zone (setf (getf record :exclusive-zone) exclusive-zone))
                (when keyboard (setf (getf record :keyboard) keyboard))
                (setf (getf record :visible) (and visible t))))))
(defun fullscreen (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :fullscreen (list id flag)))

(define-effect :fullscreen
  :canonical (destructuring-bind (id flag) args (fullscreen id flag))
  :reduce (destructuring-bind (id flag) args
            (let ((window (%materialized-window m id)))
              (when window (setf (getf window :fullscreen) (and flag t))))))
(defun maximize (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :maximize (list id flag)))

(define-effect :maximize
  :canonical (destructuring-bind (id flag) args (maximize id flag))
  :reduce (destructuring-bind (id flag) args
            (let ((window (%materialized-window m id)))
              (when window (setf (getf window :maximize) (and flag t))))))
(defun grab (id mode &key buffer-generation)
  "Own a pointer grab, optionally restricted to one window buffer lifetime.
:POINTER takes a NIL id and routes world-space motion as :GRAB events."
  (check-type mode (member :move :resize :pointer))
  (if (eq mode :pointer) (setf id (or id 0)) (check-type id (integer 1 4294967295)))
  (check-type id (integer 0 4294967295))
  (check-type buffer-generation (or null (integer 1 *)))
  (%effect :grab (if buffer-generation (list id mode buffer-generation) (list id mode))))

(define-effect :grab
  :canonical (destructuring-bind (id mode &optional buffer-generation) args
               (grab id mode :buffer-generation buffer-generation))
  :key '(:grab))
(defun motion-contract (&key hover regions edges (distance 0) drag)
  "Publish a native motion contract: this extension's result for pointer motion
depends on nothing but these conditions, so the native core may move the
pointer, the cursor and a drag without waking Lisp.

HOVER wakes it when the window under the pointer changes, with the same
:POINTER enter and leave events as always. REGIONS is a list of
\(NAME X Y WIDTH HEIGHT) screen rectangles; entering or leaving one wakes it
with a :STATE :MOTION pointer event naming the regions that changed. EDGES is a
list of \(SIDE THICKNESS), SIDE one of :TOP :BOTTOM :LEFT :RIGHT: crossing the
strip along that side of the output under the pointer wakes it the same way.
DISTANCE wakes it once the pointer has travelled that many pixels since the last
delivery. DRAG, a plist of :BOUND for a move and :MIN-SIZE and :MAX-SIZE for a
resize, lets the compositor carry a grab it owns on its own: the window follows
the pointer as \(PLACE ID ...) would place it, and Lisp hears about the drag
once it ends or the moment another event needs policy.

Without this effect an extension that reads :POINTER or :GRAB keeps every
pointer event on the Lisp path, exactly as before."
  (check-type hover boolean)
  (check-type distance (integer 0 1048576))
  (check-type regions list)
  (check-type edges list)
  (check-type drag (or null list))
  (let ((regions
          (loop for region in regions
                collect (destructuring-bind (name x y width height) region
                          (check-type name string)
                          (check-type x integer) (check-type y integer)
                          (check-type width (integer 1 1048576))
                          (check-type height (integer 1 1048576))
                          (list name x y width height))))
        (edges
          (loop for edge in edges
                collect (destructuring-bind (side thickness) edge
                          (check-type side (member :top :bottom :left :right))
                          (check-type thickness (integer 1 1048576))
                          (list side thickness))))
        (drag
          (when drag
            (let ((bound (getf drag :bound)) (min-size (getf drag :min-size))
                  (max-size (getf drag :max-size)))
              (check-type bound (integer 0 1048576))
              (check-type min-size (integer 1 1048576))
              (check-type max-size (integer 1 1048576))
              (unless (<= min-size max-size) (error "Drag sizes must not cross."))
              (list :bound bound :min-size min-size :max-size max-size)))))
    (%effect :motion (list :hover (and hover t) :regions regions :edges edges
                           :distance distance :drag drag))))

(define-effect :motion
  :canonical (apply #'motion-contract args)
  :key '(:motion))
(defparameter +command-argument-types+
  '(:window :workspace :output :integer :string :argv :boolean :keyword)
  "Command argument types. A command line string coerces into every one of them.")

(defun %command-name (name)
  (check-type name string)
  (unless (and (<= 1 (length name) 64)
               (alpha-char-p (char name 0))
               (alpha-char-p (char name (1- (length name))))
               (every (lambda (character)
                        (or (and (alpha-char-p character) (lower-case-p character))
                            (digit-char-p character) (char= character #\-)))
                      name))
    (error "Invalid command name ~S: use lowercase letters, digits and hyphens." name))
  (copy-seq name))

(defun %command-type-description (spec)
  (ecase (getf spec :type)
    (:window "a window id") (:workspace (format nil "a workspace number from 1 through ~D" (getf spec :maximum 128)))
    (:output "an output name") (:integer "an integer from -1048576 through 1048576")
    (:string "a string") (:argv "a program and its arguments")
    (:boolean "true or false") (:keyword (format nil "one of ~{~(~A~)~^, ~}" (getf spec :values)))))

(defun %check-command-value (spec value)
  (let ((type (getf spec :type)))
    (flet ((bad () (error "Command argument ~(~A~) takes ~A, not ~S."
                          (getf spec :name) (%command-type-description spec) value)))
      (ecase type
        (:window (unless (typep value '(integer 1 4294967295)) (bad)))
        (:workspace (unless (and (integerp value) (<= 1 value (getf spec :maximum 128)))
                      (error "No workspace ~A." value)))
        (:output (unless (and (stringp value) (<= 1 (length value) 128) (not (find #\Null value)))
                   (bad)))
        (:integer (unless (typep value '(integer -1048576 1048576)) (bad)))
        (:string (unless (and (stringp value) (<= (length value) 65536) (not (find #\Null value)))
                   (bad)))
        (:argv (unless (and (listp value) (<= 1 (length value) 128)
                            (every (lambda (item)
                                     (and (stringp item) (plusp (length item)) (not (find #\Null item))))
                                   value))
                 (bad)))
        (:boolean (unless (typep value 'boolean) (bad)))
        (:keyword (unless (member value (getf spec :values)) (bad))))
      value)))

(defun %command-argument-spec (spec)
  (if (and (listp spec) (eq (first spec) :name) (eq (third spec) :type))
      (copy-data spec)
      (%command-argument spec)))

(defun %command-argument (spec)
  (unless (and (listp spec) (>= (length spec) 2) (keywordp (first spec)))
    (error "A command argument needs (NAME TYPE &KEY ...): ~S" spec))
  (destructuring-bind (name type &key required (default nil default-p) description values maximum) spec
    (unless (member type +command-argument-types+)
      (error "Unknown type ~S for command argument ~S; use ~{~(~A~)~^, ~}."
             type name +command-argument-types+))
    (check-type required boolean)
    (check-type description (or null string))
    (when (and description (> (length description) 256))
      (error "Command argument description exceeds 256 characters."))
    (when (eq type :keyword)
      (unless (and (listp values) (plusp (length values)) (every #'keywordp values))
        (error "A :KEYWORD command argument needs :VALUES: ~S" spec)))
    (when (and values (not (eq type :keyword)))
      (error "Only :KEYWORD command arguments take :VALUES: ~S" spec))
    (when maximum
      (unless (eq type :workspace)
        (error "Only :WORKSPACE command arguments take :MAXIMUM: ~S" spec))
      (unless (and (integerp maximum) (<= 1 maximum 128))
        (error "A :WORKSPACE :MAXIMUM is a workspace number from 1 through 128: ~S" spec)))
    (let ((spec (list :name name :type type :required required :default default
                      :default-p (and default-p t) :description (or description "")
                      :values (copy-list values)
                      :maximum (and (eq type :workspace) (or maximum 128)))))
      (when default-p
        (ecase type
          (:window (unless (or (eql default :focused) (typep default '(integer 1 4294967295)))
                     (error "A :WINDOW default is :FOCUSED or a window id: ~S" spec)))
          (:output (unless (or (eql default :focused) (stringp default))
                     (error "An :OUTPUT default is :FOCUSED or an output name: ~S" spec)))
          ((:workspace :integer :string :argv :boolean :keyword) (%check-command-value spec default))))
      spec)))

(defun %define-command (name description arguments)
  (let ((name (%command-name name)) (seen nil) (specs nil))
    (check-type description string)
    (when (> (length description) 1024) (error "A command description exceeds 1024 characters."))
    (unless (listp arguments) (error "Command arguments are a list of specs, not ~S." arguments))
    (dolist (spec arguments)
      (let ((spec (%command-argument-spec spec)))
        (when (member (getf spec :name) seen)
          (error "Duplicate argument ~(~A~) of command ~A." (getf spec :name) name))
        (push (getf spec :name) seen)
        (push spec specs)))
    (%effect :command (list name (copy-seq description) (nreverse specs)))))

(defun define-command (name &key description arguments)
  "Own the command NAME: one name the command line, the JSON socket and key
bindings all invoke, so nothing this extension does is reachable by a key alone.
DESCRIPTION is one line. ARGUMENTS is a list of (NAME TYPE &KEY REQUIRED DEFAULT
DESCRIPTION VALUES MAXIMUM) in call order. Types are :WINDOW (a window id, the
focused window by default), :WORKSPACE (a workspace number from 1 through
MAXIMUM, 128 by default, so a window manager declares its real workspace count),
:OUTPUT (an output name, the focused window's output by default), :INTEGER,
:STRING, :ARGV (a program and its arguments), :BOOLEAN, and :KEYWORD (one of
VALUES). The declaring extension
receives a :COMMAND event whose :NAME is NAME and whose :ARGUMENTS carry every
supplied or defaulted argument."
  (%define-command name (or description "") arguments))

(define-effect :command
  :canonical (destructuring-bind (name description arguments) args
               (%define-command name description arguments))
  :key (list :command (first args))
  :reduce (destructuring-bind (name description arguments) args
            (setf (materialization-commands m)
                  (cons (list :name name :description description :arguments arguments
                              :owner (spec-name (mounted-spec mounted)))
                        (remove name (materialization-commands m)
                                :test #'equal :key (lambda (entry) (getf entry :name)))))))

(defun registered-command (commands name)
  (find name commands :test #'equal :key (lambda (entry) (getf entry :name))))

(defun invoke (name &rest arguments)
  "A command call for BIND-KEY, BIND-BUTTON, BIND-SCROLL and KEYBOARD-GRAB:
NAME with keyword arguments, whose declared defaults fill the rest."
  (unless (evenp (length arguments))
    (error "Command arguments are keyword/value pairs: ~S" arguments))
  (loop for (key) on arguments by #'cddr do (check-type key keyword))
  (copy-data (list* (%command-name name) arguments)))

(defun %callback-call (callback)
  (check-type callback keyword)
  (list :callback (string-downcase callback)))

(defun %call (command)
  "COMMAND as a call: a command name, an INVOKE list, or a session dialog callback."
  (cond ((stringp command) (list (%command-name command)))
        ((and (consp command) (stringp (first command))) (apply #'invoke command))
        ((and (consp command) (eq (first command) :callback)) (copy-data command))
        (t (error "A binding command is a command name or (INVOKE ...), not ~S." command))))

(defun %call-wire-value (value)
  "A binding argument as one quote-free token, so a wire string stays data."
  (if (or (stringp value) (consp value)) (format nil "~A" value) (format nil "~S" value)))

(defun %call-wire (call)
  (cond ((eq (first call) :callback) (second call))
        ((null (rest call)) (first call))
        (t (with-output-to-string (out)
             (format out "~A" (first call))
             (loop for (key value) on (rest call) by #'cddr
                   do (format out " ~(~A~)=~A" key (%call-wire-value value)))))))

(defun %command-number (name value)
  (if (stringp value)
      (multiple-value-bind (parsed end) (ignore-errors (parse-integer value :junk-allowed t))
        (unless (and parsed (= end (length value)))
          (error "Command argument ~(~A~) takes a number, not ~S." name value))
        parsed)
      value))

(defun %command-boolean (name value)
  (cond ((member value '("true" "1" "yes" "on") :test #'string-equal) t)
        ((member value '("false" "0" "no" "off") :test #'string-equal) nil)
        (t (error "Command argument ~(~A~) takes true or false, not ~S." name value))))

(defun %command-argv (text)
  (if (and (plusp (length text)) (char= (char text 0) #\[))
      (let ((value (parse-json text)))
        (unless (and (consp value) (eq (first value) :json-array))
          (error "The argv argument takes a JSON array or a space-separated command line, not ~S."
                 text))
        (second value))
      (let ((items nil) (start 0))
        (loop for space = (position #\Space text :start start)
              do (let ((item (subseq text start space)))
                   (unless (zerop (length item)) (push item items)))
                 (if space (setf start (1+ space)) (return)))
        (nreverse items))))

(defun %coerce-command-value (spec value)
  "VALUE as the argument SPEC declares it: a command line string, a JSON value,
or a Lisp value."
  (let ((type (getf spec :type)) (name (getf spec :name)))
    (ecase type
      ((:integer :workspace) (%command-number name value))
      (:window (if (and (stringp value) (string-equal value "focused"))
                   :focused
                   (%command-number name value)))
      (:output (if (and (stringp value) (string-equal value "focused")) :focused value))
      (:string value)
      (:boolean (if (stringp value) (%command-boolean name value) value))
      (:keyword (if (stringp value) (intern (string-upcase value) :keyword) value))
      (:argv (if (stringp value) (%command-argv value) value)))))

(defun %resolve-call (commands call)
  "CALL against COMMANDS: a known name, declared arguments, and values of their types."
  (when call
    (if (eq (first call) :callback)
        call
        (let ((entry (or (registered-command commands (first call))
                         (error "No command named ~A." (first call))))
              (arguments (rest call)))
          (loop for (key value) on arguments by #'cddr
                for spec = (find key (getf entry :arguments) :key (lambda (item) (getf item :name)))
                do (unless spec (error "~A has no ~(~A~) argument." (first call) key))
                   (%check-command-value spec (%coerce-command-value spec value)))
          (dolist (spec (getf entry :arguments))
            (when (and (getf spec :required) (not (member (getf spec :name) arguments)))
              (error "~A needs a ~(~A~) argument." (first call) (getf spec :name))))
          (copy-data call)))))

(defun resolve-binding-call (commands retired call)
  "CALL checked against COMMANDS: as is for a dialog callback, resolved for a
registered command, :RETIRED for a command whose owner went away, and an error
for a name that was never a command."
  (cond ((or (null call) (eq (first call) :callback)) call)
        ((registered-command commands (first call)) (%resolve-call commands call))
        ((member (first call) retired :test #'equal) :retired)
        (t (%resolve-call commands call))))

(defun resolve-context-calls (context retired)
  "Check every binding and the keyboard grab against CONTEXT's commands. A name
that was a command and is not one now drops its bindings with the owner that went
away; a name that was never a command is an error, so a typo in a config fails
its load rather than leaving a key that does nothing."
  (let ((commands (getf context :commands)) (bindings nil))
    (dolist (binding (getf context :bindings))
      (let ((call (resolve-binding-call commands retired (getf binding :call)))
            (release (resolve-binding-call commands retired (getf binding :release-call))))
        (unless (or (eq call :retired) (eq release :retired))
          (setf (getf binding :call) call (getf binding :release-call) release)
          (push binding bindings))))
    (setf (getf context :bindings) (nreverse bindings))
    (let ((grab (getf context :keyboard-grab)))
      (when grab
        (let ((call (resolve-binding-call commands retired (getf grab :call))))
          (if (eq call :retired)
              (setf (getf context :keyboard-grab) nil)
              (setf (getf grab :call) call))))))
  context)

(defun bind-key (modifiers keysym command &key release description)
  "Own a shortcut that runs a registered COMMAND, optionally with one for its
physical key release. MODIFIERS are :SHIFT :CONTROL :ALT :SUPER, or :MOD for the
:MOD setting. COMMAND is a command name, or (INVOKE NAME :ARGUMENT VALUE ...).
DESCRIPTION labels the binding in the hotkey overlay.
Release follows the original device/key even after modifiers change. Removing
or replacing the binding/source cancels that callback and still swallows key-up."
  (%bind modifiers keysym (%call command) (and release (%call release)) description))

(defun %bind-callback (modifiers keysym callback)
  (%bind modifiers keysym (%callback-call callback) nil nil))

(defun %bind (modifiers keysym call release description)
  (check-type keysym string)
  (check-type description (or null string))
  (when (or (zerop (length keysym)) (find #\Null keysym)) (error "Invalid keysym name."))
  (when (and description (> (length description) 256))
    (error "Binding description exceeds 256 characters."))
  (let ((mask 0))
    (dolist (modifier modifiers)
      (setf mask (logior mask (ecase modifier (:shift 1) (:control 4) (:alt 8) (:super 64) (:mod 128)))))
    (%bind-effect mask keysym call release description)))

(defun %bind-effect (mask keysym call release description)
  (check-type mask (integer 0 205))
  (unless (zerop (logandc2 mask 205)) (error "Unsupported modifier mask."))
  (check-type keysym string)
  (check-type call cons)
  (check-type release (or null cons))
  (check-type description (or null string))
  (dolist (item (list call release))
    (when item
      (unless (if (eq (first item) :callback)
                  (and (= 2 (length item)) (stringp (second item)))
                  (and (stringp (first item)) (evenp (length (rest item)))
                       (loop for (key) on (rest item) by #'cddr always (keywordp key))))
        (error "Invalid binding call: ~S" item))))
  (when (or (find #\Null keysym) (zerop (keysym-code keysym))) (error "Invalid keysym name."))
  (when (and description (> (length description) 256))
    (error "Binding description exceeds 256 characters."))
  (%effect :bind (list mask (copy-seq keysym) (copy-data call) (copy-data release)
                       (when description (copy-seq description)))))

(defvar *keysym-codes* (make-hash-table :test #'equal))

(defun keysym-code (name)
  "The native keysym for NAME. Resolution is pure and every transaction resolves
every declared binding three times, so each name crosses into C once."
  (or (gethash name *keysym-codes*)
      (progn (when (>= (hash-table-count *keysym-codes*) 4096) (clrhash *keysym-codes*))
             (setf (gethash (copy-seq name) *keysym-codes*) (%keysym name)))))

(define-effect :bind
  :canonical (apply #'%bind-effect args)
  :key (list :bind (first args) (keysym-code (second args)))
  :reduce (destructuring-bind (mask keysym call release &optional description) args
            (let ((code (keysym-code keysym)) (declared mask)
                  (mask (if (logtest 128 mask)
                            (logior (logandc2 mask 128) (materialization-mod-bit m))
                            mask)))
              (setf (materialization-bindings m)
                    (delete-if (lambda (b) (and (= mask (getf b :modifiers))
                                                (= code (getf b :code))))
                               (materialization-bindings m)))
              (push (list :modifiers mask :code code :keysym keysym
                          :owner (spec-name (mounted-spec mounted))
                          :call call :release-call release
                          :command (%call-wire call) :release (and release (%call-wire release))
                          :source-id (spec-id (mounted-spec mounted))
                          :description description :declared declared
                          :order (incf (materialization-binding-order m)))
                    (materialization-bindings m)))))
(defun bind-button (modifiers button command &key release)
  "Own a pointer button shortcut. BUTTON is :LEFT, :RIGHT, :MIDDLE, :SIDE, :EXTRA,
:FORWARD, :BACK or a kernel code. The press never reaches clients; the event
carries :BUTTON, :WINDOW under the pointer, world :X :Y and screen :SX :SY."
  (check-type button (or (member :left :right :middle :side :extra :forward :back)
                         (integer 1 65535)))
  (bind-key modifiers (format nil "button-~(~A~)" button) command :release release))
(defun bind-scroll (modifiers direction command)
  "Own scrolling in DIRECTION (:UP :DOWN :LEFT :RIGHT); the event carries :DELTA."
  (check-type direction (member :up :down :left :right))
  (bind-key modifiers (format nil "scroll-~(~A~)" direction) command))
(defun keyboard-grab (&key otherwise)
  "Own the keyboard: only this extension's bindings fire, whatever the held
modifiers, and no key reaches a client. OTHERWISE, a command name or (INVOKE ...),
runs for other non-modifier keys with :KEYSYM. The latest owner wins."
  (%keyboard-grab (when otherwise (%call otherwise))))

(defun %keyboard-grab (call)
  (%effect :keyboard-grab (list (copy-data call))))

(defun %keyboard-grab-callback (callback)
  (%keyboard-grab (%callback-call callback)))

(define-effect :keyboard-grab
  :canonical (destructuring-bind (call) args (%keyboard-grab call))
  :key '(:keyboard-grab)
  :reduce (let ((call (first args)))
            (setf (materialization-keyboard-grab m)
                  (list :owner (spec-name (mounted-spec mounted))
                        :source-id (spec-id (mounted-spec mounted))
                        :call call :otherwise (and call (%call-wire call))))))
(defparameter +sound-events+ '(:key :button :open :close :bell))
(defun sound (event files &key (gain 0))
  "Own the sound for EVENT: :KEY plays on every key press, :BUTTON on every
pointer button press, :OPEN when a window first maps, :CLOSE when it closes,
and :BELL when a Wayland client requests the system bell.
FILES is a WAV path, or a list of up to 16 played in turn: 16-bit PCM or 32-bit
float, mono or stereo, at 48000 Hz. Relative paths resolve beside the declaring
source. GAIN is in decibels, from -60 to 12. Later owners replace an event's
sound; omission restores the preceding owner."
  (unless (member event +sound-events+) (error "Unknown sound event ~S." event))
  (let ((files (if (listp files) files (list files))))
    (unless (and (<= 1 (length files) 16)
                 (every (lambda (file) (and (stringp file) (<= 1 (length file) 4096)
                                            (not (find #\Null file))))
                        files))
      (error "A sound needs 1 through 16 file paths."))
    (unless (and (%finite-real-p gain) (<= -60 gain 12)) (error "Sound gain must lie within -60 and 12 dB."))
    (%effect :sound (list event (mapcar #'copy-seq files) (%double-float gain)))))

(define-effect :sound
  :canonical (destructuring-bind (event files gain) args
               (sound event files :gain gain))
  :reduce (destructuring-bind (event files gain) args
            (let ((directory (spec-directory (mounted-spec mounted))))
              (setf (getf (materialization-sounds m) event)
                    (list :files (loop for file in files collect
                                   (cond ((char= (char file 0) #\/) file)
                                         ((and (stringp directory) (plusp (length directory))
                                               (char= (char directory 0) #\/))
                                          (concatenate 'string directory file))
                                         (t (error "Relative sound paths require a declaring source."))))
                          :gain gain)))))
(defun launch (&rest argv)
  "Launch literal argv once after acceptance; the session owns the child."
  (multiple-value-bind (command cwd env) (process-options argv nil nil)
    (declare (ignore cwd env))
    (%command :launch command)))
(defun spawn (command &key cwd env (focus t))
  "Launch a shell string or literal argv once after the current transaction.
CWD is relative to the declaring source. ENV overlays the child environment.
The session owns the child. With FOCUS NIL the launch stays in the background:
windows of the launched process tree open unfocused and unraised, and the
launch carries no activation token."
  (check-type focus boolean)
  (multiple-value-bind (launch directory environment) (process-options command cwd env)
    (%command :spawn (list launch directory environment focus))))
(defun close-window (id)
  (check-type id (integer 1 4294967295))
  (%command :close (list id)))
(defun screenshot (&optional mode)
  (check-type mode (member nil :screen))
  (%command :screenshot (list (eq mode :screen))))
(defun clipboard-copy (path type &key delete)
  "Own the clipboard with the bytes of the absolute PATH offered as the MIME TYPE.
:DELETE removes PATH once it has been read."
  (check-type path (and string (satisfies %absolute-path-p)))
  (check-type type string)
  (%command :clipboard (list path type (and delete t))))
(defun %absolute-path-p (path)
  (and (plusp (length path)) (char= (char path 0) #\/)))
(defun media-control (action)
  "Ask the selected MPRIS player to :PLAY-PAUSE, go to the :NEXT or :PREVIOUS track."
  (check-type action (member :play-pause :next :previous))
  (%command :media (list action)))
(defun adjust-brightness (percent)
  "Step the backlight by PERCENT of its range, up when positive, down when negative."
  (check-type percent (integer -100 100))
  (%command :brightness (list percent)))
(defun screencast-answer (token answer &optional value)
  "Answer a :SCREENCAST request TOKEN with :OUTPUT and an output name, :WINDOW and
a window id, or :DENY."
  (check-type token (integer 1 *))
  (ecase answer
    (:output (check-type value string))
    (:window (check-type value (integer 1 4294967295)))
    (:deny (check-type value null)))
  (%command :screencast (list token answer value)))
(defun output-power (mode &optional name)
  "Turn displays :ON, :OFF or :TOGGLE while their outputs keep their place in the
layout. NAME picks one connector; without it every output follows, and :TOGGLE
turns them all off while any is lit."
  (check-type mode (member :on :off :toggle))
  (check-type name (or null string))
  (%command :power (list mode name)))
(defun quit () (%command :quit nil))
(defun reload () (%command :reload nil))

(defvar *next-source-id* 0)
(defun allocate-source-id ()
  (when (>= *next-source-id* (1- (expt 2 64))) (error "Source identity space exhausted."))
  (incf *next-source-id*))

(defstruct (spec (:copier nil)) name reads initial update source directory admission package (token (gensym "SOURCE-"))
  (id (allocate-source-id) :read-only t) patch)
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

(defparameter *host-commands* nil
  "Session commands the compositor owns. Keys, the command line and JSON all reach them.")

(defvar *host-command-context* nil
  "The host commands as copied context data, rebuilt when one is registered.")

(defun define-host-command (name &key description arguments call)
  "Register command NAME for the session itself, not for an extension.
CALL receives (runtime arguments), a plist of the declared argument values, and
returns a report plist, published as the command's JSON result."
  (check-type call symbol)
  (let ((entry (list :name (%command-name name) :owner "session" :host t
                     :description (or description "")
                     :arguments (mapcar #'%command-argument-spec arguments)
                     :call call)))
    (setf *host-commands*
          (cons entry (remove (getf entry :name) *host-commands* :test #'equal
                              :key (lambda (item) (getf item :name))))
          *host-command-context* nil)
    entry))

(defun host-command-entries ()
  "The host commands as context data, shared by every materialization."
  (or *host-command-context*
      (setf *host-command-context*
            (loop for entry in *host-commands*
                  collect (list :name (getf entry :name) :owner (getf entry :owner)
                                :host t :description (getf entry :description)
                                :arguments (copy-data (getf entry :arguments)))))))

(defun host-command-implementation (name)
  (or (getf (find name *host-commands* :test #'equal :key (lambda (item) (getf item :name)))
            :call)
      (error "No session command named ~A." name)))

(defun host-command-call (runtime entry arguments)
  "Run host command ENTRY for ARGUMENTS, filling declared defaults.
The report it returns becomes the command's JSON result."
  (let ((resolved (resolve-command-arguments runtime entry arguments)))
    (list :name (getf entry :name) :arguments resolved
          :result (funcall (host-command-implementation (getf entry :name)) runtime resolved))))
