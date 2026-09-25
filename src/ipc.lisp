(in-package #:tomoe)

(defconstant +json-wire-version+ 2)

(defun json-object-p (value)
  (and (consp value) (eq (first value) :json-object)))

(defun json-boolean (value) (if value t +json-false+))

(defun ipc-rectangle (record)
  (json-object (cons "x" (getf record :x)) (cons "y" (getf record :y))
               (cons "w" (getf record :width)) (cons "h" (getf record :height))))

(defun ipc-focused-window (runtime context)
  (if (runtime-backend runtime)
      (let ((id (%keyboard-focus (runtime-backend runtime)))) (unless (zerop id) id))
      (let* ((id (getf context :focus))
             (window (find id (runtime-windows runtime) :key (lambda (window) (getf window :id)))))
        (when (and window (getf window :buffered t)) id))))

(defun ipc-windows (runtime context)
  (let ((focus (ipc-focused-window runtime context)))
    (apply #'json-array
           (loop for window in (sort (copy-list (runtime-windows runtime)) #'<
                                    :key (lambda (window) (getf window :id)))
                 for id = (getf window :id)
                 for box = (find id (getf context :window-geometry) :key (lambda (box) (getf box :id)))
                 for visible = (not (null box))
                 collect (json-object
                          (cons "id" id) (cons "app_id" (or (getf window :app-id) ""))
                          (cons "title" (or (getf window :title) ""))
                          (cons "geometry" (when visible (ipc-rectangle box)))
                          (cons "mapped" (json-boolean visible))
                          (cons "focused" (json-boolean (eql id focus)))
                          (cons "fullscreen" (json-boolean (getf window :fullscreen)))
                          (cons "maximized" (json-boolean (getf window :maximize))))))))

(defun ipc-outputs (context)
  (apply #'json-array
         (loop for output in (getf context :outputs)
               for area = (or (find (getf output :name) (getf context :workareas)
                                    :test #'equal :key (lambda (area) (getf area :name))) output)
               collect (json-object (cons "name" (getf output :name))
                                    (cons "geometry" (ipc-rectangle output))
                                    (cons "usable" (ipc-rectangle area))
                                    (cons "scale" (/ (getf output :scale-120 120) 120d0))))))

(defun ipc-view (context)
  (let ((view (getf context :view)))
    (json-object (cons "x" (getf view :x 0)) (cons "y" (getf view :y 0))
                 (cons "zoom" (getf view :zoom 1d0)))))

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

(defun publish-json-context (runtime context mounts)
  (handler-case
      (let* ((server (runtime-json-server runtime))
             (windows (second (ipc-windows runtime context)))
             (outputs (ipc-outputs context)) (focus (ipc-focused-window runtime context))
             (old (runtime-ipc-published-context runtime))
             (announcements (resolved-ipc-effects mounts :announce)))
        (flet ((emit (name value) (json-broadcast server name value)))
          (dolist (window windows)
            (unless (find (json-get window "id") (getf old :windows)
                          :key (lambda (window) (json-get window "id")))
              (emit "window_open" window)))
          (dolist (window (getf old :windows))
            (unless (find (json-get window "id") windows :key (lambda (window) (json-get window "id")))
              (emit "window_close" (json-object (cons "id" (json-get window "id"))))))
          (unless (eql focus (getf old :focus))
            (emit "focus_change" (json-object (cons "id" focus))))
          (unless (equal outputs (getf old :outputs)) (emit "outputs_changed" outputs))
          (dolist (record announcements)
            (let ((previous (assoc (first record) (runtime-ipc-published-announcements runtime) :test #'equal)))
              (unless (and previous (eq (second record) (second previous))
                           (equal (third record) (third previous)))
                (emit (first record) (first (third record))))))
          (dolist (previous (runtime-ipc-published-announcements runtime))
            (unless (assoc (first previous) announcements :test #'equal)
              (emit (first previous) nil))))
        (setf (runtime-ipc-published-context runtime) (list :windows windows :outputs outputs :focus focus)
              (runtime-ipc-published-announcements runtime) announcements))
    (serious-condition (condition) (json-transport-log "event publication failed" condition))))

(defun ipc-subscribe (client params)
  (let* ((events (and (json-object-p params) (json-get params "events" :absent)))
         (all (or (null params) (eq events :absent))))
    (unless (or all (and (consp events) (eq (first events) :json-array)
                         (every #'stringp (second events))))
      (error "invalid subscribe params (expected {~S: [~S]})" "events" "..."))
    (let ((names (unless all (second events))))
      (setf (json-client-subscribed client) t (json-client-events client) names)
      (json-object (cons "events" (if names (apply #'json-array names) "all"))))))

(defun poll-json-focus (runtime)
  (let ((context (runtime-effective runtime)))
    (unless (eql (ipc-focused-window runtime context)
                 (getf (runtime-ipc-published-context runtime) :focus))
      (publish-json-context runtime context (runtime-mounts runtime)))))

(defun ipc-invoke-method (runtime record method params)
  (destructuring-bind (name spec (mode value)) record
    (declare (ignore name))
    (ecase mode
      (:state value)
      (:command
       (when (runtime-ipc-call runtime) (error "Nested IPC dispatch is not supported."))
       (when (>= (runtime-ipc-sequence runtime) 18446744073709551615)
         (error "IPC request identity space exhausted."))
       (let* ((token (incf (runtime-ipc-sequence runtime)))
              (call (list :token token :result nil :replied nil)))
         (unwind-protect
              (progn
                (setf (runtime-ipc-call runtime) call)
                (handler-case
                    (transact runtime (runtime-mounts runtime)
                              (list :type :ipc :owner (spec-name spec) :method method
                                    :command value :params params :call token)
                              '(:ipc) (list (spec-name spec)))
                  (serious-condition (condition)
                    (record-error runtime condition)
                    (error condition)))
                (getf call :result))
           (setf (runtime-ipc-call runtime) nil)))))))

(defun answer-screencast (runtime token answer value)
  (let ((pending (assoc token (runtime-screencasts runtime))))
    (when pending
      (setf (runtime-screencasts runtime) (remove pending (runtime-screencasts runtime)))
      (destructuring-bind (client id) (rest pending)
        (let ((identifier (and (eq answer :window) (%window-identifier (runtime-backend runtime) value))))
          (json-send (runtime-json-server runtime) client
                     (json-object
                      (cons "id" id)
                      (cons "result"
                            (cond ((eq answer :output)
                                   (json-object "action" "resolve" "type" "output" "output" value))
                                  (identifier
                                   (json-object "action" "resolve" "type" "window" "identifier" identifier))
                                  (t (json-object "action" "deny")))))))))))

(defun handle-json-request (runtime server client request)
  (reconcile-backend-observations runtime)
  (unless (and (runtime-running runtime) (not *stop-requested*))
    (return-from handle-json-request))
  (let* ((fields (and (json-object-p request) (second request)))
         (id (cdr (assoc "id" fields :test #'equal)))
         (method (cdr (assoc "method" fields :test #'equal)))
         (params (cdr (assoc "params" fields :test #'equal))))
    (unless (and (stringp method) (or (null id) (typep id '(integer 0 18446744073709551615))))
      (json-transport-log "ignoring a request with an invalid method or ID")
      (return-from handle-json-request))
    (handler-case
        (let ((result
                (cond
                  ((equal method "version")
                   (json-object (cons "wire" +json-wire-version+) (cons "version" "0.1.0")))
                  ((equal method "windows") (ipc-windows runtime (runtime-effective runtime)))
                  ((equal method "outputs") (ipc-outputs (runtime-effective runtime)))
                  ((equal method "view") (ipc-view (runtime-effective runtime)))
                  ((equal method "subscribe") (ipc-subscribe client params))
                  ((equal method "quit") (setf (runtime-running runtime) nil) t)
                  ((equal method "screencast_select")
                   (unless id (return-from handle-json-request))
                   (if (notany (lambda (mounted) (member :screencast (mount-context-reads mounted)))
                               (runtime-mounts runtime))
                       (json-object (cons "action" "fallback"))
                       (let ((token (incf (runtime-ipc-sequence runtime)))
                             (types (and params (json-get params "types"))))
                         (push (list token client id) (runtime-screencasts runtime))
                         (dispatch-event runtime
                                         (list :type :screencast :token token
                                               :app-id (or (and params (json-get params "app_id")) "")
                                               :monitor (and (member "monitor" (second types) :test #'equal) t)
                                               :window (and (member "window" (second types) :test #'equal) t)))
                         (return-from handle-json-request))))
                  (t (let ((record (assoc method (resolved-ipc-effects (runtime-mounts runtime) :method)
                                         :test #'equal)))
                       (unless record (error "unknown method: ~A" method))
                       (ipc-invoke-method runtime record method params))))))
          (when id (json-send server client (json-object (cons "id" id) (cons "result" result)))))
      (serious-condition (condition)
        (when id
          (let ((message (princ-to-string condition)))
            (json-send server client
                       (json-object (cons "id" id) (cons "error" (subseq message 0 (min 4096 (length message))))))))))))
