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

(defun ipc-method-state (method)
  (let ((record (ipc-method-record method)))
    (when (and record (eq (second (third record)) :state))
      (third (third record)))))

(defun ipc-invoke-method (method params)
  "Run the extension method METHOD for PARAMS, or its state value. Signals for a
method no mount owns."
  (let ((record (ipc-method-record method)))
    (unless record (error "Unknown method: ~A." method))
    (destructuring-bind (name spec mode-value) record
      (declare (ignore name))
      (ecase (second mode-value)
        (:state (third mode-value))
        (:command
         (when (runtime-ipc-call) (error "Nested IPC dispatch is not supported."))
         (when (>= (runtime-ipc-sequence) 18446744073709551615)
           (error "IPC request identity space exhausted."))
         (let* ((token (1+ (runtime-ipc-sequence)))
                (call (list :token token :result nil :replied nil)))
           (%r! :ipc-sequence token)
           (unwind-protect
                (progn
                  (%r! :ipc-call call)
                  (dispatch-event (list :type :ipc :owner (spec-name spec)
                                        :method method
                                        :command (third mode-value) :params params
                                        :call token))
                  (getf call :result))
             (%r! :ipc-call nil))))))))

(defun ipc-window-record (window context focus)
  (let* ((id (getf window :id))
         (box (find id (getf context :window-geometry) :key (lambda (entry) (getf entry :id)))))
    (list :id id :identifier (getf window :identifier)
          :app-id (or (getf window :app-id) "")
          :title (or (getf window :title) "")
          :mapped (and box t)
          :x (getf box :x) :y (getf box :y)
          :width (getf box :width) :height (getf box :height)
          :focused (and (eql id focus) t)
          :fullscreen (and (getf window :fullscreen) t)
          :maximized (and (getf window :maximize) t))))

(defun ipc-window-json (record)
  (json-object (cons "id" (getf record :id))
               (cons "identifier" (getf record :identifier))
               (cons "app_id" (getf record :app-id))
               (cons "title" (getf record :title))
               (cons "geometry" (if (getf record :mapped)
                                    (json-object (cons "x" (getf record :x)) (cons "y" (getf record :y))
                                                 (cons "w" (getf record :width))
                                                 (cons "h" (getf record :height)))
                                    +json-false+))
               (cons "mapped" (if (getf record :mapped) t +json-false+))
               (cons "focused" (if (getf record :focused) t +json-false+))
               (cons "fullscreen" (if (getf record :fullscreen) t +json-false+))
               (cons "maximized" (if (getf record :maximized) t +json-false+))))

(defun ipc-outputs-json (context)
  (apply #'json-array
         (loop for output in (getf context :outputs)
               collect (json-object
                        (cons "name" (getf output :name))
                        (cons "x" (getf output :x)) (cons "y" (getf output :y))
                        (cons "width" (getf output :width)) (cons "height" (getf output :height))
                        (cons "scale" (/ (getf output :scale-120 120) 120d0))
                        (cons "transform" (getf output :transform 0))))))

(defun publish-json-context (context mounts)
  "Emit the events between the last published context and CONTEXT, at commit."
  (let* ((windows (loop for window in (sort (copy-list (runtime-windows)) #'<
                                            :key (lambda (window) (getf window :id)))
                        collect (ipc-window-record window context (focused-window))))
         (outputs (ipc-outputs-json context))
         (focus (focused-window))
         (old (runtime-ipc-published-context))
         (announcements (resolved-ipc-effects mounts :announce)))
    (dolist (window windows)
      (unless (find (getf window :id) (getf old :windows)
                    :key (lambda (entry) (getf entry :id)))
        (%json-broadcast "window_open" (ipc-window-json window))))
    (dolist (window (getf old :windows))
      (unless (find (getf window :id) windows :key (lambda (entry) (getf entry :id)))
        (%json-broadcast "window_close" (json-object (cons "id" (getf window :id))))))
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
        (unless (and (consp json) (eq (first json) :json-object))
          (error "command arguments must be an object."))
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
