(in-package #:tomoe)

(defstruct mounted spec state effects (dispatches 0) (failures 0) last-error)
(defstruct runtime backend socket sources mounts windows outputs effective layers grab
  (running t) processes last-error (generation 0) (watch t) source-stamps)

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
      (:place (destructuring-bind (id x y width height visible) args
                (place id x y width height visible)))
      (:output (apply #'%output-effect args))
      (:focus (destructuring-bind (id) args (focus id)))
      (:layer
       (destructuring-bind (id layer exclusive-zone keyboard visible) args
         (layer id :layer layer :exclusive-zone exclusive-zone
                :keyboard keyboard :visible visible)))
      (:fullscreen (destructuring-bind (id flag) args (fullscreen id flag)))
      (:maximize (destructuring-bind (id flag) args (maximize id flag)))
      (:grab (destructuring-bind (id mode) args (grab id mode)))
      (:bind
       (destructuring-bind (mask keysym command) args
         ;; SHIFT|CTRL|ALT|LOGO, the set the native backend reports.
         (check-type mask (integer 0 77))
         (unless (zerop (logandc2 mask 77)) (error "Unsupported modifier mask."))
         (check-type keysym string)
         (check-type command string)
         (when (or (find #\Null keysym) (find #\Null command)
                   (zerop (length command)) (zerop (%keysym keysym)))
           (error "Invalid key binding: ~S" args))
         (%effect :bind args))))))

(defun canonical-command (command)
  (check-type command command)
  (let ((args (copy-data (command-arguments command))))
    (ecase (command-kind command)
      (:launch (apply #'launch args))
      (:close (destructuring-bind (id) args (close-window id)))
      (:quit (unless (null args) (error "QUIT takes no arguments.")) (quit))
      (:reload (unless (null args) (error "RELOAD takes no arguments.")) (reload)))))

(defun effect-key (effect)
  (ecase (effect-kind effect)
    (:place (list :place (first (effect-arguments effect))))
    (:output (list :output (first (effect-arguments effect))))
    (:focus '(:focus))
    (:layer (list :layer (first (effect-arguments effect))))
    (:fullscreen (list :fullscreen (first (effect-arguments effect))))
    (:maximize (list :maximize (first (effect-arguments effect))))
    (:grab (list :grab (first (effect-arguments effect))))
    (:bind (list :bind (first (effect-arguments effect))
                 (%keysym (second (effect-arguments effect)))))))

(defun invoke-extension (runtime mounted context event)
  ;; No native handle or runtime object crosses this boundary. Copy before calling
  ;; and before retaining results. Time limits cover copying and result validation.
  (let ((spec (mounted-spec mounted)))
    (handler-case
        (sb-ext:with-timeout 0.025
          (multiple-value-bind (state effects commands)
              (funcall (spec-update spec)
                       (make-snapshot
                        (loop for key in (spec-reads spec)
                              append (list key (copy-data (getf context key))))
                        (copy-list (spec-reads spec)))
                       (copy-data (mounted-state mounted)) (copy-data event))
            (when (> (length effects) 512) (error "More than 512 effects."))
            (when (> (length commands) 32) (error "More than 32 commands."))
            (when (and commands (not (member (getf event :type) '(:key :button))))
              (error "One-shot commands require a key, button, or IPC command event."))
            (let ((effects (mapcar #'canonical-effect effects))
                  (commands (mapcar #'canonical-command commands))
                  (keys (make-hash-table :test #'equal)))
              (dolist (effect effects)
                (let ((key (effect-key effect)))
                  (when (gethash key keys) (error "Duplicate effect: ~S" key))
                  (setf (gethash key keys) t)))
              (setf (mounted-state mounted) (copy-data state)
                    (mounted-effects mounted) effects)
              (incf (mounted-dispatches mounted))
              commands)))
      ;; SB-EXT:TIMEOUT is a SERIOUS-CONDITION, not an ERROR.
      (serious-condition (condition)
        ;; Charge the live record by name: the candidate this ran on is thrown
        ;; away by rollback, and the unit must keep its identity and count.
        (let ((live (find (spec-name spec) (runtime-mounts runtime)
                          :test #'equal :key (lambda (m) (spec-name (mounted-spec m))))))
          (when live
            (incf (mounted-failures live))
            (setf (mounted-last-error live) (princ-to-string condition))))
        ;; The original text survives in the report, so no failure is opaque.
        (error 'extension-error :unit (spec-name spec) :cause condition)))))

(defun resolved-grab (mounts)
  "The single active pointer grab, from the last mount that owns one."
  (let ((grab nil))
    (dolist (mounted mounts grab)
      (dolist (effect (mounted-effects mounted))
        (when (eq (effect-kind effect) :grab)
          (setf grab (effect-arguments effect)))))))

(defun materialize (runtime mounts)
  "Reconstruct owned context. Preserve live clients, output facts, and map order.
Unmount removes a contribution, exposing the preceding owner or these defaults.
The single grab is not context data; RESOLVED-GRAB derives it from the mounts."
  (let ((layout (loop for window in (runtime-windows runtime)
                      collect (list :id (getf window :id) :x 0 :y 0
                                    :width (max 1 (getf window :width))
                                    :height (max 1 (getf window :height)) :visible t
                                    :fullscreen nil :maximize nil)))
        (layers (copy-data (runtime-layers runtime)))
        (focused nil)
        (outputs nil)
        (bindings nil))
    (dolist (mounted mounts)
      (dolist (effect (mounted-effects mounted))
        (let ((args (effect-arguments effect)))
          (ecase (effect-kind effect)
            (:place
             (destructuring-bind (id x y width height visible) args
               (let ((window (find id layout :key (lambda (w) (getf w :id)))))
                 (when window
                   (setf (getf window :x) x (getf window :y) y
                         (getf window :width) width (getf window :height) height
                         (getf window :visible) visible)))))
            (:output
             (destructuring-bind (name mode width height refresh scale x y positioned) args
               (setf outputs (delete name outputs :test #'equal :key (lambda (o) (getf o :name))))
               (push (list :name name :mode mode :width width :height height
                           :refresh-mhz refresh :scale-120 scale :x x :y y :positioned positioned)
                     outputs)))
            (:focus (setf focused (first args)))
            (:layer
             ;; An override for an id without a live surface is dropped silently:
             ;; the client decides when it exists.
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
            (:grab nil)
            (:bind
             (destructuring-bind (mask keysym command) args
               (let ((code (%keysym keysym)))
                 (setf bindings
                       (delete-if (lambda (b) (and (= mask (getf b :modifiers))
                                                   (= code (getf b :code)))) bindings))
                 (push (list :modifiers mask :code code :keysym keysym
                             :owner (spec-name (mounted-spec mounted)) :command command)
                       bindings))))))))
    (unless (find focused layout :key (lambda (w) (getf w :id)))
      (setf focused nil))
    (list :windows (runtime-windows runtime) :outputs (runtime-outputs runtime)
          :output-config (sort outputs #'string< :key (lambda (o) (getf o :name)))
          :layout layout :layers layers :focus focused
          :bindings (sort bindings
                          (lambda (a b) (or (< (getf a :modifiers) (getf b :modifiers))
                                            (and (= (getf a :modifiers) (getf b :modifiers))
                                                 (< (getf a :code) (getf b :code)))))))))

(defun changed-keys (before after)
  (remove-if (lambda (key) (equal (getf before key) (getf after key))) +context-keys+))

(defun connected-output-config (context)
  (remove-if-not (lambda (config)
                   (find (getf config :name) (getf context :outputs)
                         :test #'equal :key (lambda (o) (getf o :name))))
                 (getf context :output-config)))

(defun commit-context (runtime mounts context)
  "The only extension-to-native write path. All reducers have returned and validated."
  (let ((old (runtime-effective runtime)) (backend (runtime-backend runtime)))
    ;; Check after dependencies settle. A layout may react to a new focus by
    ;; revealing that window in the following round, as the monocle layout does.
    (when (and (getf context :focus)
               (not (getf (find (getf context :focus) (getf context :layout)
                                :key (lambda (w) (getf w :id))) :visible)))
      (error "Cannot focus a hidden window: ~D" (getf context :focus)))
    (let ((outputs (connected-output-config context)))
      (unless (equal (connected-output-config old) outputs)
        (configure-native-outputs backend outputs)))
    (unless (equal (getf old :bindings) (getf context :bindings))
      (%clear-bindings backend)
      (dolist (binding (getf context :bindings))
        (unless (= 1 (%bind backend (getf binding :modifiers) (getf binding :code)
                            (getf binding :owner) (getf binding :command)))
          (setf (runtime-running runtime) nil)
          (error "Native binding allocation failed; stopping the compositor."))))
    (dolist (window (getf context :layout))
      (let* ((id (getf window :id))
             (previous (find id (getf old :layout) :key (lambda (w) (getf w :id)))))
        ;; States are acknowledged, so only a resolved change reaches the backend.
        (unless (and (equal (getf previous :fullscreen) (getf window :fullscreen))
                     (equal (getf previous :maximize) (getf window :maximize)))
          (%window-state backend id (if (getf window :fullscreen) 1 0)
                                 (if (getf window :maximize) 1 0)))))
    (dolist (record (getf context :layers))
      (let* ((id (getf record :id))
             (previous (find id (getf old :layers) :key (lambda (l) (getf l :id)))))
        ;; An id with no previous record is new, so its override lands on map.
        (unless (equal previous record)
          (%layer backend id
                  (ecase (getf record :layer)
                    (:background 0) (:bottom 1) (:top 2) (:overlay 3) ((nil) -1))
                  (or (getf record :exclusive-zone) -1)
                  (ecase (getf record :keyboard)
                    (:none 0) (:exclusive 1) (:on-demand 2) ((nil) -1))
                  (if (getf record :visible) 1 0)))))
    (let ((grab (resolved-grab mounts)))
      (unless (equal grab (runtime-grab runtime))
        (if grab
            (%grab backend (first grab) (ecase (second grab) (:move 1) (:resize 2)))
            (%grab backend 0 0))
        (setf (runtime-grab runtime) grab)))
    (unless (and (equal (getf old :layout) (getf context :layout))
                 (eql (getf old :focus) (getf context :focus)))
      ;; Rebuild stacking in preserved map order, then raise the resolved focus.
      (dolist (window (getf context :layout))
        (%place backend (getf window :id) (getf window :x) (getf window :y)
                (getf window :width) (getf window :height)
                (if (getf window :visible) 1 0)))
      (%focus backend (or (getf context :focus) 0)))
    (setf (runtime-mounts runtime) mounts (runtime-effective runtime) context)))

(defun transact (runtime mounts event changed &optional force)
  "Stage reducers and dependency propagation before touching the native scene.
A bad callback or a dependency cycle discards the entire candidate transaction."
  (let* ((candidate (mapcar #'copy-mounted mounts))
         (context (materialize runtime candidate))
         (dirty (union changed (changed-keys (runtime-effective runtime) context)))
         (commands nil))
    (loop for round below 16 do
      (dolist (mounted candidate)
        (when (or (member (spec-name (mounted-spec mounted)) force :test #'equal)
                  (intersection dirty (spec-reads (mounted-spec mounted))))
          (setf commands (nconc commands (invoke-extension runtime mounted context event)))))
      (let* ((next (materialize runtime candidate)) (changes (changed-keys context next)))
        (setf context next dirty changes force nil
              event (list :type :change :keys changes))
        (unless changes
          (commit-context runtime candidate context)
          (dolist (command commands)
            (when (runtime-running runtime) (execute-command runtime command)))
          (return-from transact t))))
    (error "Extension dependencies did not settle within 16 rounds.")))

(defun source-stamp (path)
  "Stamp PATH as (write-date length digest), or nil when it is missing or unreadable.
The write date resolves to one second, so a same-length edit inside the second a
source was loaded would otherwise be invisible. Extension files are small and
stay in the page cache, so the digest decides."
  (handler-case
      (with-open-file (stream path :if-does-not-exist nil)
        (when stream
          (let* ((size (file-length stream))
                 (text (make-string (min size 1048576))))
            (list (file-write-date stream) size (read-sequence text stream) (sxhash text)))))
    (serious-condition () nil)))

(defun load-specs (path)
  (let* ((*source* (namestring (truename path)))
         (*definitions* nil) (*package* (find-package :tomoe-user)) (*read-eval* nil))
    (sb-ext:with-timeout 1
      (with-open-file (stream *source*)
        (when (> (file-length stream) 1048576) (error "Extension file exceeds 1 MiB."))
        (load stream :verbose nil :print nil)))
    (nreverse *definitions*)))

(defun configure (runtime sources &optional changed-source)
  (let* ((specs (loop for path in sources
                      when (or (null changed-source) (equal path changed-source)) append (load-specs path)))
         (retained (when changed-source
                     (remove changed-source (runtime-mounts runtime) :test #'equal
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
      (transact runtime (append retained mounts) '(:type :mount) nil (mapcar #'spec-name specs)))
    (setf (runtime-sources runtime) (copy-list sources) (runtime-last-error runtime) nil
          ;; Stamp every source with the content this call loaded or retained, so
          ;; a file watcher can tell an edit made before its first sweep from the
          ;; content the running policy actually came from.
          (runtime-source-stamps runtime)
          (loop for path in sources collect (cons path (source-stamp path))))
    (incf (runtime-generation runtime))))

(defun unmount (runtime name)
  (unless (find name (runtime-mounts runtime) :test #'equal
                :key (lambda (m) (spec-name (mounted-spec m))))
    (error "No mounted extension named ~A." name))
  (transact runtime
            (remove name (runtime-mounts runtime) :test #'equal
                    :key (lambda (m) (spec-name (mounted-spec m))))
            (list :type :unmount :name name) nil)
  (incf (runtime-generation runtime)))

(defun execute-command (runtime command)
  (ecase (command-kind command)
    (:launch
     (let* ((argv (command-arguments command))
            (process (sb-ext:run-program (first argv) (rest argv) :search t :wait nil
                                         :input nil :output *error-output* :error *error-output*)))
       ;; Applications belong to the session, not to an extension. Unmount never
       ;; kills a user-launched application. Session shutdown owns their cleanup.
       (push process (runtime-processes runtime))))
    (:close (%close (runtime-backend runtime) (first (command-arguments command))))
    (:quit (setf (runtime-running runtime) nil))
    (:reload (configure runtime (runtime-sources runtime)))))

(defun reap-processes (runtime)
  (setf (runtime-processes runtime)
        (delete-if (lambda (process)
                     (when (member (sb-ext:process-status process) '(:exited :signaled))
                       (sb-ext:process-close process) t))
                   (runtime-processes runtime))))

(defun stop-processes (runtime)
  (reap-processes runtime)
  (dolist (process (runtime-processes runtime)) (sb-ext:process-kill process sb-posix:sigterm))
  (loop repeat 20 while (runtime-processes runtime) do (sleep 0.01) (reap-processes runtime))
  (dolist (process (runtime-processes runtime))
    (sb-ext:process-kill process sb-posix:sigkill)
    (sb-ext:process-wait process)
    (sb-ext:process-close process)))

(defun record-error (runtime condition)
  ;; The failing unit was already charged where it failed, so an EXTENSION-ERROR
  ;; only has to reach the runtime-level message and the server log here.
  (setf (runtime-last-error runtime) (princ-to-string condition))
  (format *error-output* "tomoe: ~A~%" condition))

(defun dispatch-event (runtime event)
  (let ((changed nil))
    (ecase (getf event :type)
      ((:map :metadata)
       (let* ((id (getf event :id))
              (old (find id (runtime-windows runtime) :key (lambda (w) (getf w :id))))
              (new (list :id id :title (getf event :title) :app-id (getf event :app-id)
                         :width (getf event :width) :height (getf event :height)
                         :fullscreen (and (getf event :fullscreen) t)
                         :maximize (and (getf event :maximize) t))))
         (if (equal old new)
             ;; A denied request leaves the record alone but the policy still
             ;; decides what to do about it.
             (when (and (eq (getf event :type) :metadata) (getf event :request))
               (push :windows changed))
             (progn
               (setf (runtime-windows runtime)
                     (if old (substitute new old (runtime-windows runtime))
                         (append (runtime-windows runtime) (list new))))
               (push :windows changed)))))
      (:layer
       (let* ((id (getf event :id))
              (record (list :id id :namespace (getf event :namespace) :layer (getf event :layer)
                            :anchors (getf event :anchors)
                            :exclusive-zone (getf event :exclusive-zone)
                            :margin (getf event :margin) :width (getf event :width)
                            :height (getf event :height) :keyboard (getf event :keyboard)
                            :visible t))
              (old (find id (runtime-layers runtime) :key (lambda (l) (getf l :id)))))
         ;; A mapped layer surface starts shown; only an owner can hide it.
         (setf (runtime-layers runtime)
               (if old (substitute record old (runtime-layers runtime))
                   (append (runtime-layers runtime) (list record))))
         (push :layers changed)))
      (:unmap
       (let ((id (getf event :id)))
         (setf (runtime-windows runtime)
               (remove id (runtime-windows runtime) :key (lambda (w) (getf w :id)))
               (runtime-layers runtime)
               (remove id (runtime-layers runtime) :key (lambda (l) (getf l :id))))
         (push :windows changed)
         (push :layers changed)))
      (:outputs
       (unless (equal (runtime-outputs runtime) (getf event :outputs))
         (setf (runtime-outputs runtime) (getf event :outputs))
         (push :outputs changed)))
      (:key (push :key changed))
      (:button (push :button changed))
      (:grab (push :grab changed)))
    (when changed
      (handler-case (transact runtime (runtime-mounts runtime) event changed)
        (serious-condition (condition)
          (record-error runtime condition)
          ;; External client/output lifetimes cannot be rolled back. Prune dead
          ;; IDs from the last good policy even when a reducer failed.
          (when (runtime-running runtime)
            (commit-context runtime (runtime-mounts runtime)
                            (materialize runtime (runtime-mounts runtime)))))))))

(defun describe-runtime (runtime)
  (let ((grab (resolved-grab (runtime-mounts runtime))))
    (append (copy-data (runtime-effective runtime))
            (list :socket (runtime-socket runtime) :generation (runtime-generation runtime)
                  :last-error (runtime-last-error runtime)
                  :grab (when grab (list :id (first grab) :mode (second grab)))
                  :watch (runtime-watch runtime)
                  :extensions
                  (loop for m in (runtime-mounts runtime) for spec = (mounted-spec m)
                        collect (list :name (spec-name spec) :source (spec-source spec)
                                      :reads (spec-reads spec) :state (mounted-state m)
                                      :dispatches (mounted-dispatches m)
                                      :failures (mounted-failures m)
                                      :last-error (mounted-last-error m)
                                      :effects (loop for e in (mounted-effects m)
                                                     collect (cons (effect-kind e)
                                                                   (effect-arguments e)))))))))
