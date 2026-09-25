(defpackage #:tomoe
  (:use #:cl)
  (:export #:define-extension #:context #:previous-context #:place #:focus #:bind-key #:configure-output #:configure-keyboard #:screenshot #:screencast-answer
           #:window-rule #:rules-for #:raise-window #:show-window #:hide-window
           #:publish-state #:state-value #:service-state #:window-geometry
           #:ui #:shell-surface
           #:json-object #:json-array #:json-get #:+json-false+
           #:serve-state #:serve-method #:announce #:broadcast #:ipc-reply
           #:layer #:fullscreen #:maximize #:grab #:set-view #:settings #:setting
           #:bind-button #:bind-scroll #:window-properties #:keyboard-grab
           #:confirm-dialog #:menu-dialog #:menu-choice #:sheet-dialog #:toast
           #:once #:interval #:watch-file #:exec-async #:run-once #:service #:spawn #:launch #:close-window #:quit #:reload))
(defpackage #:tomoe-user (:use #:cl #:tomoe))
(in-package #:tomoe)

(define-condition output-configuration-error (simple-error)
  ((output :initarg :output :initform nil :reader output-error-name)))

(defconstant +wire-version+ 1)
(defconstant +native-abi-version+ 27)
(defparameter +context-keys+
  '(:windows :window-geometry :rules :data :services :outputs :connectors :output-config :output-errors :config-error :workareas :view :layout :stacking :focus :bindings :keyboard :settings :layers :surfaces :key :button :pointer :grab :request :screenshot :screencast :ipc :ui))
(defvar *definitions* :not-loading)
(defvar *source*)
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
  (check-type refresh (integer -1 1000000))
  (check-type scale (or null (integer 30 960)))
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

(defun configure-output (name &key (mode :preferred) refresh scale position disabled mirror vrr)
  "Own an output's mode, scale, position, enablement, mirror target and VRR request.
MODE is :PREFERRED, :MAX (largest progressive), or (WIDTH HEIGHT [HZ]). REFRESH
is :MAX or Hz, matched within 1 Hz. Without it :PREFERRED keeps the preferred
mode and other sizes take their highest rate. MIRROR names an active non-mirroring output.
Omitted SCALE inherits the :SCALE setting.
Disabled connectors remain discoverable in :CONNECTORS, outside active :OUTPUTS."
  (check-type scale (or null (real 1/4 8)))
  (flet ((millihertz (refresh)
           (cond ((null refresh) 0) ((eq refresh :max) -1)
                 (t (check-type refresh (real 1 1000)) (round (* refresh 1000))))))
    (let ((scale-120 (and scale (%round-away-positive (* scale 120)))))
      (destructuring-bind (x y) (or position '(0 0))
        (if (member mode '(:preferred :max))
            (%output-effect name mode 0 0 (millihertz refresh) scale-120
                            x y (not (null position)) disabled mirror vrr)
            (destructuring-bind (width height &optional hz) mode
              (%output-effect name :exact width height (millihertz (or hz refresh)) scale-120
                              x y (not (null position)) disabled mirror vrr)))))))

(defun focus (id &key (raise t))
  "Own keyboard focus. RAISE also contributes this window's stacking order."
  (check-type id (or null (integer 1 4294967295)))
  (check-type raise boolean)
  (%effect :focus (list id raise)))

(defun window-properties (id &rest properties &key radius tearing blur border)
  "Own rendering overrides for one window. Omitted keys fall back to the
settings; :TEARING and :BLUR take T or NIL, and a supplied NIL denies.
BORDER is (:FOCUSED color :UNFOCUSED color). Later owners replace each key."
  (declare (ignore radius tearing blur border))
  (check-type id (integer 1 4294967295))
  (%effect :window-properties
           (list id (%settings-plist properties
                                     '((:radius (:integer 0 4096)) (:tearing :boolean) (:blur :boolean)
                                       (:border (:group (:focused :color) (:unfocused :color))))))))

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

(defparameter +settings+
  '((:scale (:real 1/4 8) 1)
    (:mod (:member :super :alt :control :shift) :super)
    (:focus-follows-mouse :boolean nil)
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
  "Normalize NIL (off), T (DEFAULT), (:SPRING plist) or (:EASE plist)."
  (flet ((bad () (error "Invalid animation: ~S" value)))
    (cond ((null value) (list :kind :off))
          ((eq value t) (%animation-spec default default))
          ((and (consp value) (eq (first value) :kind)
                (member (second value) '(:off :spring :ease)))
           (copy-list value))
          ((not (and (consp value) (member (first value) '(:spring :ease)) (= 2 (length value)))) (bad))
          ((eq (first value) :spring)
           (destructuring-bind (&key (damping-ratio 1) (stiffness 800) (epsilon 1/10000))
               (%settings-plist (second value) '((:damping-ratio (:real 0 1000)) (:stiffness (:real 0 100000))
                                                 (:epsilon (:real 0 1))) :spring)
             (list :kind :spring :damping-ratio (%double-float damping-ratio)
                   :stiffness (%double-float stiffness) :epsilon (%double-float epsilon))))
          (t
           (destructuring-bind (&key (duration-ms 150) (curve :ease-out-cubic)) (second value)
             (unless (typep duration-ms '(integer 1 600000)) (bad))
             (unless (or (member curve '(:linear :ease-out-quad :ease-out-cubic :ease-out-expo))
                         (and (listp curve) (= 4 (length curve)) (every #'%finite-real-p curve)))
               (bad))
             (list :kind :ease :duration-ms duration-ms
                   :curve (if (keywordp curve) curve (mapcar #'%double-float curve))))))))

(defun %animation-settings (value)
  "Normalize :ANIMATIONS: T, NIL, or (:WINDOW-MOVE spec :WINDOW-OPEN spec)."
  (let ((defaults '(:window-move (:spring nil) :window-open (:ease (:duration-ms 150 :curve :ease-out-expo)))))
    (unless (or (member value '(t nil)) (and (listp value) (evenp (length value))))
      (error "Invalid animations: ~S" value))
    (loop for (key default) on defaults by #'cddr
          append (list key (%animation-spec (cond ((member value '(t nil)) value)
                                                   ((member key value) (getf value key))
                                                   (t t))
                                             default)))))

(defun settings (&rest plist)
  "Own compositor settings. Later owners replace individual keys; grouped keys
merge field by field. Omission restores the preceding owner or the default."
  (%effect :settings (%settings-plist plist +settings+)))

(defun %settings-defaults (table)
  (loop for (key type default) in table
        append (list key (cond ((and (consp type) (eq (first type) :group)) (%settings-defaults (rest type)))
                               ((eq type :animations) (%animation-settings default))
                               (t default)))))

(defun %settings-merge (settings plist table)
  (loop for (key value) on plist by #'cddr
        for type = (second (assoc key table))
        do (setf (getf settings key)
                 (if (and (consp type) (eq (first type) :group))
                     (%settings-merge (copy-list (getf settings key)) value (rest type))
                     value)))
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
(defun fullscreen (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :fullscreen (list id flag)))
(defun maximize (id flag)
  (check-type id (integer 1 4294967295))
  (check-type flag boolean)
  (%effect :maximize (list id flag)))
(defun grab (id mode &key buffer-generation)
  "Own a pointer grab, optionally restricted to one window buffer lifetime.
:POINTER takes a NIL id and routes world-space motion as :GRAB events."
  (check-type mode (member :move :resize :pointer))
  (if (eq mode :pointer) (setf id (or id 0)) (check-type id (integer 1 4294967295)))
  (check-type id (integer 0 4294967295))
  (check-type buffer-generation (or null (integer 1 *)))
  (%effect :grab (if buffer-generation (list id mode buffer-generation) (list id mode))))
(defun bind-key (modifiers keysym command &key release description)
  "Own a shortcut, optionally with a command for its physical key release.
Modifiers are :SHIFT :CONTROL :ALT :SUPER, or :MOD for the :MOD setting.
DESCRIPTION labels the binding in the hotkey overlay.
Release follows the original device/key even after modifiers change. Removing
or replacing the binding/source cancels that callback and still swallows key-up."
  (check-type keysym string)
  (check-type command keyword)
  (check-type release (or null keyword))
  (check-type description (or null string))
  (when (or (zerop (length keysym)) (find #\Null keysym)) (error "Invalid keysym name."))
  (when (and description (> (length description) 256)) (error "Binding description exceeds 256 characters."))
  (let ((mask 0))
    (dolist (modifier modifiers)
      (setf mask (logior mask (ecase modifier (:shift 1) (:control 4) (:alt 8) (:super 64) (:mod 128)))))
    (%effect :bind (list mask (copy-seq keysym) (string-downcase command)
                         (when release (string-downcase release))
                         (when description (copy-seq description))))))
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
modifiers, and no key reaches a client. OTHERWISE, a keyword, fires for other
non-modifier keys with :KEYSYM. The latest owner wins."
  (check-type otherwise (or null keyword))
  (%effect :keyboard-grab (list (when otherwise (string-downcase otherwise)))))
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
(defun screenshot (&optional mode)
  (check-type mode (member nil :screen))
  (%command :screenshot (list (eq mode :screen))))
(defun screencast-answer (token answer &optional value)
  "Answer a :SCREENCAST request TOKEN with :OUTPUT and an output name, :WINDOW and
a window id, or :DENY."
  (check-type token (integer 1 *))
  (ecase answer
    (:output (check-type value string))
    (:window (check-type value (integer 1 4294967295)))
    (:deny (check-type value null)))
  (%command :screencast (list token answer value)))
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
