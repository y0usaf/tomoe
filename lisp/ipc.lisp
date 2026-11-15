(defparameter +json-wire-version+ 3)

(defun json-object-p (value)
  (and (consp value) (eq (first value) :json-object)))

(defun json-boolean (value) (if value t +json-false+))

(defun ipc-rectangle (record)
  (json-object (cons "x" (getf record :x)) (cons "y" (getf record :y))
               (cons "w" (getf record :width)) (cons "h" (getf record :height))))

(defun ipc-window-record (window context focus)
  (let* ((id (getf window :id))
         (box (find id (getf context :window-geometry) :key (lambda (entry) (getf entry :id)))))
    (list :id id :identifier (getf window :identifier) :box box
          :json (json-object
                 (cons "id" id) (cons "identifier" (getf window :identifier))
                 (cons "app_id" (or (getf window :app-id) ""))
                 (cons "title" (or (getf window :title) ""))
                 (cons "geometry" (and box (ipc-rectangle box)))
                 (cons "mapped" (json-boolean box))
                 (cons "focused" (json-boolean (eql id focus)))
                 (cons "fullscreen" (json-boolean (getf window :fullscreen)))
                 (cons "maximized" (json-boolean (getf window :maximize)))))))

(defun ipc-window-records (context)
  (let ((focus (focused-window)))
    (loop for window in (sort (copy-list (runtime-windows)) #'<
                              :key (lambda (window) (getf window :id)))
          collect (ipc-window-record window context focus))))

(defun ipc-windows (context)
  (apply #'json-array (mapcar (lambda (record) (getf record :json))
                              (ipc-window-records context))))

(defun ipc-outputs (context)
  (apply #'json-array
         (loop for output in (getf context :outputs)
               for area = (or (find (getf output :name) (getf context :workareas)
                                    :test #'equal :key (lambda (entry) (getf entry :name)))
                              output)
               collect (json-object (cons "name" (getf output :name))
                                    (cons "geometry" (ipc-rectangle output))
                                    (cons "usable" (ipc-rectangle area))
                                    (cons "scale" (/ (getf output :scale-120 120) 120d0))
                                    (cons "transform" (getf output :transform 0))))))

(defun ipc-view (context)
  (let ((view (getf context :view)))
    (json-object (cons "x" (getf view :x 0)) (cons "y" (getf view :y 0))
                 (cons "zoom" (getf view :zoom 1d0)))))

(defun ipc-cursor-geometry (params)
  "The source rectangle a recorder asked about, in global physical pixels.
PARAMS carries either \"output\" (an output name) or \"window\" (a window
identifier)."
  (let ((output-name (and params (json-get params "output")))
        (identifier (and params (json-get params "window"))))
    (cond
      ((stringp output-name)
       (let ((output (find output-name (runtime-outputs)
                           :test #'equal :key (lambda (entry) (getf entry :name)))))
         (and output
              (list :x (getf output :x) :y (getf output :y)
                    :width (getf output :width) :height (getf output :height)
                    :transform (getf output :transform 0)))))
      ((stringp identifier)
       (let ((window (find identifier (runtime-windows)
                           :test #'equal :key (lambda (entry) (getf entry :identifier)))))
         (and window
              (find (getf window :id) (getf (runtime-effective) :window-geometry)
                    :key (lambda (box) (getf box :id)))))))))

(defun %cursor-image-chunks (image)
  (apply #'json-array
         (loop for start from 0 below (length image) by 60000
               collect (subseq image start (min (length image) (+ start 60000))))))

(defun ipc-cursor (params)
  "Cursor position and image for ScreenCast METADATA mode. Coordinates are
relative to the requested output or window so the portal need not know the
compositor's layout. The image is sent only when GENERATION changes."
  (let* ((have (or (and params (json-get params "generation")) 0))
         (state (or (%cursor-state have) (error "Cannot serialize cursor state.")))
         (geometry (ipc-cursor-geometry params))
         (hidden (getf state :hidden))
         (x (getf state :x 0))
         (y (getf state :y 0))
         (local-x (if geometry (round (- x (getf geometry :x))) (round x)))
         (local-y (if geometry (round (- y (getf geometry :y))) (round y)))
         (visible (and geometry (not hidden)
                       (zerop (getf geometry :transform 0))
                       (<= (getf geometry :x) x) (< x (+ (getf geometry :x) (getf geometry :width)))
                       (<= (getf geometry :y) y) (< y (+ (getf geometry :y) (getf geometry :height))))))
    (apply #'json-object
           (append
            (list (cons "visible" (json-boolean visible))
                  (cons "hidden" (json-boolean hidden))
                  (cons "generation" (getf state :generation 0))
                  (cons "x" local-x) (cons "y" local-y)
                  (cons "hotspot_x" (getf state :hotspot-x 0))
                  (cons "hotspot_y" (getf state :hotspot-y 0))
                  (cons "width" (getf state :width 0))
                  (cons "height" (getf state :height 0))
                  (cons "stride" (getf state :stride 0))
                  (cons "scale" (/ (getf state :scale-120 120) 120d0)))
            (when (getf state :image)
              (list (cons "image" (%cursor-image-chunks (getf state :image)))
                    (cons "format" (getf state :format 0))))))))

(defun resolved-ipc-effects (mounts kind)
  "Records retain their winning source identity as well as their copied value."
  (let ((records nil))
    (dolist (mounted mounts records)
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) kind)
          (let* ((args (effect-arguments effect)) (name (first args))
                 (record (list name (mounted-spec mounted) (rest args)))
                 (old (assoc name records :test #'equal)))
            (if old (setf (cdr old) (cdr record))
                (setf records (nconc records (list record))))))))))

(defun ipc-method-record (method)
  (find method (resolved-ipc-effects (runtime-mounts) :method)
        :test #'equal :key #'first))

(defun ipc-invoke-method (method params)
  "Run the extension method METHOD for PARAMS, or its state value. Signals for a
method no mount owns."
  (let ((record (ipc-method-record method)))
    (unless record (error "unknown method: ~A" method))
    (destructuring-bind (name spec mode-value) record
      (declare (ignore name))
      (ecase (first mode-value)
        (:state (second mode-value))
        (:command
         (when (runtime-ipc-call) (error "Nested IPC dispatch is not supported."))
         (when (>= (runtime-ipc-sequence) most-positive-fixnum)
           (error "IPC request identity space exhausted."))
         (let* ((token (1+ (runtime-ipc-sequence)))
                (call (list :token token :result nil :replied nil)))
           (%r! :ipc-sequence token)
           (unwind-protect
                (progn
                  (%r! :ipc-call call)
                  (let ((failure nil))
                    (%catch (lambda ()
                              (dispatch-event (list :type :ipc :owner (spec-name spec)
                                                    :method method
                                                    :command (second mode-value) :params params
                                                    :call token)))
                            (lambda (text) (setf failure text)))
                    (when failure
                      (record-error failure)
                      (error "~A" failure)))
                  (getf call :result))
             (%r! :ipc-call nil))))))))

(defun ipc-command-argument (value)
  "A JSON argument value as the Lisp value the command takes."
  (cond ((eq value +json-false+) nil)
        ((and (consp value) (eq (first value) :json-array)) (second value))
        (t value)))

(defun ipc-command-arguments (json)
  "A JSON object of command arguments as a plist of keyword/value pairs."
  (if (null json)
      nil
      (progn
        (unless (json-object-p json) (error "command arguments must be an object."))
        (loop for (key . value) in (second json)
              collect (intern (string-upcase key) :keyword)
              collect (ipc-command-argument value)))))

(defun ipc-command-value (value)
  "A resolved command argument as JSON data."
  (cond ((keywordp value) (string-downcase value))
        ((consp value) (apply #'json-array
                              (loop for item in value
                                    collect (if (stringp item) item (princ-to-string item)))))
        (t value)))

(defun ipc-command-report (report)
  (when report
    (apply #'json-object
           (loop for (key value) on report by #'cddr
                 collect (cons (string-downcase key) (ipc-command-value value))))))

(defun ipc-command-result (result)
  "The JSON form of a ran command: its name and the values it ran with, plus the
report a session command returned."
  (apply #'json-object
         (append (list (cons "name" (getf result :name))
                       (cons "arguments"
                             (apply #'json-object
                                    (loop for (key value) on (getf result :arguments) by #'cddr
                                          collect (cons (string-downcase key)
                                                        (ipc-command-value value))))))
                 (when (getf result :result)
                   (list (cons "result" (ipc-command-report (getf result :result))))))))

(defun ipc-command-argument-json (spec)
  (apply #'json-object
         (append (list (cons "name" (string-downcase (getf spec :name)))
                       (cons "type" (string-downcase (getf spec :type)))
                       (cons "required" (json-boolean (and (getf spec :required) t)))
                       (cons "description" (getf spec :description)))
                 (when (getf spec :values)
                   (list (cons "values"
                               (apply #'json-array
                                      (loop for value in (getf spec :values)
                                            collect (string-downcase value))))))
                 (when (getf spec :maximum)
                   (list (cons "maximum" (getf spec :maximum))))
                 (when (getf spec :default-p)
                   (list (cons "default" (ipc-command-value (getf spec :default))))))))

(defun ipc-commands (context)
  "Every registered command, its owner, description and typed arguments."
  (apply #'json-array
         (loop for entry in (getf context :commands)
               collect (json-object
                        (cons "name" (getf entry :name))
                        (cons "owner" (getf entry :owner))
                        (cons "description" (getf entry :description))
                        (cons "arguments"
                              (apply #'json-array
                                     (loop for spec in (getf entry :arguments)
                                           collect (ipc-command-argument-json spec))))))))

(defun ipc-command (params)
  (unless (json-object-p params)
    (error "command needs {~S: NAME, ~S: {...}}." "name" "arguments"))
  (let ((name (json-get params "name")))
    (unless (stringp name) (error "command needs a name string."))
    (ipc-command-result
     (run-command name (ipc-command-arguments (json-get params "arguments"))))))

(defun publish-json-context (context mounts)
  "Emit the events between the last published context and CONTEXT, at commit."
  (let* ((windows (ipc-window-records context))
         (outputs (ipc-outputs context))
         (focus (focused-window))
         (old (runtime-ipc-published-context))
         (announcements (resolved-ipc-effects mounts :announce)))
    (dolist (record windows)
      (unless (find (getf record :id) (getf old :windows)
                    :key (lambda (entry) (getf entry :id)))
        (%json-broadcast "window_open" (getf record :json))))
    (dolist (record (getf old :windows))
      (unless (find (getf record :id) windows :key (lambda (entry) (getf entry :id)))
        (%json-broadcast "window_close" (json-object (cons "id" (getf record :id))))))
    (unless (eql focus (getf old :focus))
      (%json-broadcast "focus_change" (json-object (cons "id" focus))))
    (unless (equal outputs (getf old :outputs))
      (%json-broadcast "outputs_changed" outputs))
    (dolist (record announcements)
      (let ((previous (assoc (first record) (runtime-ipc-published-announcements) :test #'equal)))
        (unless (and previous (eq (second record) (second previous))
                     (equal (third record) (third previous)))
          (%json-broadcast (first record) (first (third record))))))
    (dolist (previous (runtime-ipc-published-announcements))
      (unless (assoc (first previous) announcements :test #'equal)
        (%json-broadcast (first previous) nil)))
    (%r! :ipc-published-context (list :windows windows :outputs outputs :focus focus))
    (%r! :ipc-published-announcements announcements)))

(defun ipc-publish-baseline (context mounts)
  "Replay the present state as events, so a first subscriber starts from it."
  (let ((windows (ipc-window-records context))
        (outputs (ipc-outputs context))
        (focus (focused-window))
        (announcements (resolved-ipc-effects mounts :announce)))
    (dolist (record windows) (%json-broadcast "window_open" (getf record :json)))
    (%json-broadcast "focus_change" (json-object (cons "id" focus)))
    (%json-broadcast "outputs_changed" outputs)
    (dolist (record announcements)
      (%json-broadcast (first record) (first (third record))))
    (%r! :ipc-published-context (list :windows windows :outputs outputs :focus focus))
    (%r! :ipc-published-announcements announcements)))

(defparameter *input-events* '("pointer_motion" "pointer_button" "key")
  "Event names that need the core's input stream, which is off until asked for.")

(defun ipc-subscribe (params)
  "Validate a subscription and replay the present state to the first subscriber.
The reply's \"events\" lets the core record the filter."
  (let* ((events (and (json-object-p params) (json-get params "events" :absent)))
         (all (or (null params) (eq events :absent))))
    (unless (or all (and (consp events) (eq (first events) :json-array)
                         (every #'stringp (second events))))
      (error "invalid subscribe params (expected {~S: [~S]})" "events" "..."))
    (let ((names (unless all (second events))))
      (when (or all (some (lambda (name) (member name *input-events* :test #'equal)) names))
        (%input-stream 1))
      (unless (%r :ipc-baseline-sent)
        (%r! :ipc-baseline-sent t)
        (ipc-publish-baseline (runtime-effective) (runtime-mounts)))
      (json-object (cons "events" (if names (apply #'json-array names) "all"))))))

(defun ipc-screencast-select (params)
  "Answer a ScreenCast selection request: the fallback when no mount reads
:screencast, otherwise a pending token the selected mount answers later."
  (if (notany (lambda (mounted) (member :screencast (mount-context-reads mounted)))
              (runtime-mounts))
      (json-object (cons "action" "fallback"))
      (let* ((token (1+ (runtime-ipc-sequence)))
             (types (and params (json-get params "types"))))
        (%r! :ipc-sequence token)
        (dispatch-event (list :type :screencast :token token
                              :app-id (or (and params (json-get params "app_id")) "")
                              :monitor (and (member "monitor" (second types) :test #'equal) t)
                              :window (and (member "window" (second types) :test #'equal) t)))
        (json-object (cons "action" "pending") (cons "token" token)))))

(defun ipc-request (method params)
  "The reply payload for one JSON method: the built-in methods and the methods
mounted extensions own."
  (cond
    ((equal method "version")
     (json-object (cons "wire" +json-wire-version+)
                  (cons "version" (or (%getenv "TOMOE_VERSION") "dev"))))
    ((equal method "windows") (ipc-windows (runtime-effective)))
    ((equal method "outputs") (ipc-outputs (runtime-effective)))
    ((equal method "cursor") (ipc-cursor params))
    ((equal method "view") (ipc-view (runtime-effective)))
    ((equal method "commands") (ipc-commands (runtime-effective)))
    ((equal method "command") (ipc-command params))
    ((equal method "subscribe") (ipc-subscribe params))
    ((equal method "quit") (%quit) t)
    ((equal method "screencast_select") (ipc-screencast-select params))
    (t (ipc-invoke-method method params))))
