(in-package #:tomoe)


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

(defun prune-rule-mounts (runtime mounts)
  "Prune external lifetimes without evaluating any extension code.
Recovery must still release dead window scopes when a predicate or reducer
has failed. Retain only descendants whose parent and declaration still exist."
  (let ((result (copy-list (root-mounts mounts))))
    (dolist (instance (rule-instances mounts) result)
      (let ((parent (find (mounted-rule-parent instance) result :key #'mounted-spec)))
        (when (and parent
                   (find-rule-declaration parent (mounted-rule-name instance))
                   (find (mounted-rule-window instance) (runtime-windows runtime)
                         :key (lambda (window) (getf window :id))))
          (setf result (nconc result (list instance))))))))

(defun rule-instance-key (instance)
  (list (mounted-rule-parent instance) (mounted-rule-name instance)
        (mounted-rule-window instance)))

(defun rule-instance-identity (parent name window-id)
  (format nil "~A#~D:~A#~D" (spec-name parent)
          (length (symbol-name name)) (symbol-name name) window-id))

(defun rule-snapshot (context reads &optional previous)
  (make-snapshot (loop for key in reads append (list key (copy-data (getf context key))))
                 (copy-list reads)
                 (loop for key in reads append (list key (copy-data (getf previous key))))))

(defun rule-matches-p (definition window context &optional previous)
  (destructuring-bind (name app-id title properties reads state) (effect-arguments definition)
    (declare (ignore name properties state))
    (and (or (null app-id) (string= app-id (or (getf window :app-id) "")))
         (or (null title) (search title (or (getf window :title) "")))
         (or (null (effect-predicate definition))
             (funcall (effect-predicate definition) (copy-data window)
                      (rule-snapshot context reads previous))))))

(defun rule-evaluation-error (runtime parent condition)
  (let ((live (find (spec-name (mounted-spec parent)) (runtime-mounts runtime)
                    :test #'equal :key (lambda (mount) (spec-name (mounted-spec mount))))))
    (when live
      (incf (mounted-failures live))
      (setf (mounted-last-error live) (princ-to-string condition))))
  (error 'extension-error :unit (spec-name (mounted-spec parent)) :cause condition))

(defun reconcile-rule-mounts (runtime mounts context)
  "Match a bounded snapshot of declarations. New nested declarations are seen
by the next reconciliation pass, so a callback never extends its own traversal.
Return complete mounts, new instance names, and changed callback definitions."
  (let* ((mounts (prune-rule-mounts runtime mounts))
         (previous (make-hash-table :test #'equal))
         (result (copy-list (root-mounts mounts)))
         (new nil) (changed nil) (count 0))
    (dolist (instance (rule-instances mounts))
      (setf (gethash (rule-instance-key instance) previous) instance))
    (dolist (parent mounts)
      (when (find (mounted-spec parent) result :key #'mounted-spec)
        (dolist (definition (mounted-effects parent))
          (when (eq (effect-kind definition) :rule)
            (handler-case
                (sb-ext:with-timeout 0.025
                  (dolist (window (runtime-windows runtime))
                    (when (rule-matches-p definition window context (runtime-effective runtime))
                      (when (> (incf count) 4096) (error "More than 4096 matching rule instances."))
                      (let* ((name (first (effect-arguments definition)))
                             (id (getf window :id))
                             (owner (mounted-spec parent))
                             (key (list owner name id))
                             (old (gethash key previous))
                             (instance
                               (or old
                                   (make-mounted
                                    :spec (make-spec :name (rule-instance-identity owner name id)
                                                     :source (spec-source owner))
                                    :state (copy-data (sixth (effect-arguments definition)))
                                    :rule-parent owner :rule-name name :rule-window id :rule-pending t))))
                        (cond ((null old) (push (spec-name (mounted-spec instance)) new))
                              ((not (eq (mounted-rule-definition old) definition))
                               (push (spec-name (mounted-spec instance)) changed)))
                        (setf (mounted-rule-definition instance) definition)
                        (setf result (nconc result (list instance)))))))
              (serious-condition (condition)
                (rule-evaluation-error runtime parent condition)))))))
    (values result new changed)))

(defun resolved-rule-properties (runtime mounts)
  "Merge copied arbitrary properties in declaration order, including NIL."
  (let ((result (loop for window in (runtime-windows runtime)
                      collect (list :id (getf window :id) :properties nil))))
    (dolist (instance (rule-instances (prune-rule-mounts runtime mounts)) result)
      (let ((record (find (mounted-rule-window instance) result
                          :key (lambda (window) (getf window :id)))))
        (dolist (entry (fourth (effect-arguments (mounted-rule-definition instance))))
          (let ((old (assoc (car entry) (getf record :properties) :test #'equal)))
            (if old (setf (cdr old) (copy-data (cdr entry)))
                (setf (getf record :properties)
                      (nconc (getf record :properties) (list (copy-data entry)))))))))))

(defun invoke-rule-application (runtime instance snapshot state event)
  (let ((application (effect-application (mounted-rule-definition instance))))
    (if application
        (let ((window (find (mounted-rule-window instance) (runtime-windows runtime)
                            :key (lambda (record) (getf record :id)))))
          (unless window (error "Rule instance target no longer exists."))
          (funcall application
                   (append (copy-data window) (list :properties (rules-for snapshot window)))
                   snapshot state
                   (list* :rule (mounted-rule-name instance) :window (mounted-rule-window instance)
                          :parent (copy-seq (spec-name (mounted-rule-parent instance))) event)))
        (values state nil nil))))

(defun describe-rule-instances (runtime)
  (loop for instance in (rule-instances (runtime-mounts runtime))
        collect (list :owner (spec-name (mounted-rule-parent instance))
                      :name (mounted-rule-name instance) :window (mounted-rule-window instance)
                      :identity (spec-name (mounted-spec instance))
                      :source-id (spec-id (mounted-spec instance))
                      :state (copy-data (mounted-state instance))
                      :dispatches (mounted-dispatches instance)
                      :failures (mounted-failures instance)
                      :last-error (mounted-last-error instance))))
