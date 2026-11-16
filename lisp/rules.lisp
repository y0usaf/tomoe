(defun root-mounts (mounts)
  (remove-if #'mounted-rule-parent mounts))

(defun rule-instances (mounts)
  (remove-if-not #'mounted-rule-parent mounts))

(defun admission-scope-p (mounted mounts)
  "Rule descendants share their admission owner's proposal lifetime."
  (loop for scope = mounted then (and (mounted-rule-parent scope)
                                      (find (mounted-rule-parent scope) mounts :key #'mounted-spec))
        while scope
        thereis (spec-admission (mounted-spec scope))))

(defun mount-context-reads (mounted)
  (if (mounted-rule-parent mounted)
      (union '(:windows :rules) (fifth (effect-arguments (mounted-rule-definition mounted))))
      (spec-reads (mounted-spec mounted))))

(defun find-rule-declaration (mounted name)
  (find-if (lambda (effect)
             (and (eq (effect-kind effect) :rule)
                  (eq (first (effect-arguments effect)) name)))
           (mounted-effects mounted)))

(defun prune-rule-mounts (mounts)
  "Prune external lifetimes without evaluating any extension code. Recovery must
still release dead window scopes when a predicate or reducer has failed."
  (let ((result (copy-list (root-mounts mounts))))
    (dolist (instance (rule-instances mounts) result)
      (let ((parent (find (mounted-rule-parent instance) result :key #'mounted-spec)))
        (when (and parent
                   (find-rule-declaration parent (mounted-rule-name instance))
                   (find (mounted-rule-window instance) (runtime-windows)
                         :key (lambda (window) (getf window :id))))
          (setf result (nconc result (list instance))))))))

(defun rule-instance-key (instance)
  (list (mounted-rule-parent instance) (mounted-rule-name instance)
        (mounted-rule-window instance)))

(defun rule-instance-identity (parent name window-id)
  (format nil "~A#~D:~A#~D" (spec-name parent)
          (length (symbol-name name)) (symbol-name name) window-id))

(defun rule-snapshot (context reads &optional previous)
  (make-snapshot context (copy-list reads) previous))

(defun rule-matches-p (definition window context &optional previous)
  (destructuring-bind (name app-id title properties reads state) (effect-arguments definition)
    (and (or (null app-id) (string= app-id (or (getf window :app-id) "")))
         (or (null title) (search title (or (getf window :title) "")))
         (or (null (effect-predicate definition))
             (funcall (effect-predicate definition) (copy-data window)
                      (rule-snapshot context reads previous))))))


(defun reconcile-rule-mounts (mounts context)
  "Match a bounded snapshot of declarations. New nested declarations are seen
by the next reconciliation pass. Return complete mounts, new instance names, and
changed callback definitions."
  (let* ((mounts (prune-rule-mounts mounts))
         (previous nil)
         (result (copy-list (root-mounts mounts)))
         (new nil) (changed nil) (count 0))
    (dolist (instance (rule-instances mounts))
      (setf previous (%alist-set (rule-instance-key instance) instance previous)))
(dolist (parent mounts)
      (when (find (mounted-spec parent) result :key #'mounted-spec)
        (dolist (definition (%effects-of-kind (mounted-effects parent) :rule))
          (let ((message nil) (found nil) (found-new nil) (found-changed nil)
                (owner (mounted-spec parent))
                (name (first (effect-arguments definition))))
            (%catch
             (lambda ()
               (%with-limits nil 25
                 (lambda ()
                   (dolist (window (runtime-windows))
                     (when (rule-matches-p definition window context (runtime-effective))
                       (when (> (incf count) 4096) (error "More than 4096 matching rule instances."))
                       (let* ((id (getf window :id))
                              (key (list owner name id))
                              (entry (assoc key previous :test #'equal))
                              (old (cdr entry))
                              (instance
                                (if old
                                    old
                                    (make-mounted
                                     :spec (make-spec :name (rule-instance-identity owner name id)
                                                      :source (spec-source owner)
                                                      :directory (spec-directory owner))
                                     :state (copy-data (sixth (effect-arguments definition)))
                                     :rule-parent owner :rule-name name :rule-window id
                                     :rule-pending t))))
                         (cond ((null old) (push (spec-name (mounted-spec instance)) found-new))
                               ((not (eq (mounted-rule-definition old) definition))
                                (push (spec-name (mounted-spec instance)) found-changed)))
                         (unless (eq (mounted-rule-definition instance) definition)
                           (%invocations+))
                         (setf (nth 10 instance) definition)
                         (push instance found)))))))
             (lambda (text) (setf message text)))
            (if message
                (progn
                  (extension-failed (list owner name) parent message)
                  (setf found (loop for (key . instance) in previous
                                    when (and (eq (first key) owner) (eq (second key) name)
                                              (find (third key) (runtime-windows)
                                                    :key (lambda (window) (getf window :id))))
                                      collect instance)
                        found-new nil found-changed nil))
                (setf found (nreverse found)))
            (setf result (nconc result found)
                  new (nconc found-new new)
                  changed (nconc found-changed changed))))))
    (values result new changed)))

(defun resolved-rule-properties (mounts)
  "Merge copied arbitrary properties in declaration order, including NIL. A window
of a launch that asked to stay in the background carries (:FOCUS . NIL) ahead of
every declared property, so no rule can focus it back."
  (let ((result (loop for window in (runtime-windows)
                      collect (list :id (getf window :id) :properties nil))))
    (dolist (instance (rule-instances (prune-rule-mounts mounts)))
      (let ((record (find (mounted-rule-window instance) result
                          :key (lambda (window) (getf window :id)))))
        (dolist (entry (fourth (effect-arguments (mounted-rule-definition instance))))
          (let ((old (assoc (car entry) (getf record :properties) :test #'equal)))
            (if old (setf (cdr old) (copy-data (cdr entry)))
                (setf (getf record :properties)
                      (nconc (getf record :properties) (list (copy-data entry)))))))))
    (dolist (window (runtime-windows) result)
      (when (null (getf window :focus t))
        (let ((record (find (getf window :id) result
                            :key (lambda (entry) (getf entry :id)))))
          (setf (getf record :properties) (acons :focus nil (getf record :properties))))))))

(defun invoke-rule-application (instance snapshot state event)
  (let ((application (effect-application (mounted-rule-definition instance))))
    (if application
        (let ((window (find (mounted-rule-window instance) (runtime-windows)
                            :key (lambda (record) (getf record :id)))))
          (unless window (error "Rule instance target no longer exists."))
          (funcall application
                   (append (copy-data window) (list :properties (rules-for snapshot window)))
                   snapshot state
                   (list* :rule (mounted-rule-name instance) :window (mounted-rule-window instance)
                          :parent (copy-seq (spec-name (mounted-rule-parent instance))) event)))
        (values state nil nil))))

(defun describe-rule-instances ()
  (loop for instance in (rule-instances (runtime-mounts))
        collect (list :owner (spec-name (mounted-rule-parent instance))
                      :name (mounted-rule-name instance)
                      :window (mounted-rule-window instance)
                      :identity (spec-name (mounted-spec instance))
                      :source-id (spec-id (mounted-spec instance))
                      :state (copy-data (mounted-state instance))
                      :dispatches (mounted-dispatches instance)
                      :failures (mounted-failures instance)
                      :last-error (mounted-last-error instance))))
