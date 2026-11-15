(defparameter +patch-definitions+ '(defun defmacro defparameter defvar)
  "Top-level forms whose definitions a patch owns and undo restores.")

(defun patch-name-p (name)
  (and (stringp name)
       (<= 1 (length name) 40)
       (every (lambda (character) (or (%alpha-p character) (digit-char-p character)
                                      (find character "-_")))
              name)))

(defun patch-directory ()
  (concatenate 'string (%getenv "XDG_RUNTIME_DIR") "/tomoe-patches"))

(defun write-patch-source (name text)
  "TEXT as the source file of patch NAME, readable only by this session."
  (%write-patch-file name text))

(defun patch-text (path)
  "The UTF-8 text of the patch source at PATH."
  (let ((text (%read-file path *patch-limit*)))
    (unless text (error "Cannot read the patch source ~A." path))
    text))

(defun patch-defined-symbols (text package)
  "The symbols TEXT defines at top level. Read in PACKAGE, as the loader will."
  (let ((symbols nil) (forms 0))
    (dolist (form (%read-source text package))
      (when (> (incf forms) 4096)
        (error "A patch has more than 4096 top-level forms."))
      (when (and (consp form) (member (first form) +patch-definitions+ :test #'eq))
        (let ((name (second form)))
          (when (and (symbolp name) name)
            (unless (member name symbols :test #'eq) (push name symbols))))))
    (nreverse symbols)))

(defun patch-definition-snapshot (symbol)
  "SYMBOL's definitions now, for undo to restore over the patch's own."
  (list :symbol symbol
        :name (symbol-name symbol)
        :definition (%symbol-definition symbol)))

(defun restore-patch-definition (snapshot)
  (incf *definitions-generation*)
  (%set-symbol-definition (getf snapshot :symbol) (getf snapshot :definition)))

(defun patch-shared-definitions-p (snapshots)
  "True when the patch replaced a definition that already existed."
  (not (null (some (lambda (snapshot)
                     (let ((definition (getf snapshot :definition)))
                       (or (getf definition :function) (getf definition :macro)
                           (getf definition :boundp))))
                   snapshots))))

(defun later-definition-patch (patch)
  "The patch applied after PATCH that replaced one of the same definitions.
Undoing a redefinition out of order would restore a generation the newer patch
was written against, so such an undo is refused instead."
  (let ((symbols (loop for snapshot in (getf patch :snapshots)
                       collect (getf snapshot :symbol))))
    (find-if (lambda (other)
               (intersection symbols (loop for snapshot in (getf other :snapshots)
                                           collect (getf snapshot :symbol)) :test #'eq))
             (cdr (member patch (runtime-patches) :test #'eq)))))

(defun refresh-mounts ()
  "Re-invoke every root reducer, so changed definitions reach the next commit."
  (let ((roots (root-mounts (runtime-mounts))))
    (transact roots (list :type :change :keys nil) nil
              (mapcar (lambda (mounted) (spec-name (mounted-spec mounted))) roots))))

(defun remove-patch-mounts (extensions &optional refresh)
  "Drop the mounts named EXTENSIONS. REFRESH also re-invokes every other mount."
  (let ((remaining (remove-if (lambda (mounted)
                                (member (spec-name (mounted-spec mounted)) extensions
                                        :test #'equal))
                              (runtime-mounts))))
    (transact remaining (list :type :unmount) nil
              (when refresh
                (mapcar (lambda (mounted) (spec-name (mounted-spec mounted)))
                        (root-mounts remaining))))))

(defun mount-patch-specs (specs)
  "Mount SPECS as one patch, after every name is known to be free."
  (dolist (spec specs)
    (when (find (spec-name spec) (root-mounts (runtime-mounts)) :test #'equal
                :key (lambda (mounted) (spec-name (mounted-spec mounted))))
      (error "Duplicate extension: ~A" (spec-name spec)))
    (setf (nth 10 spec) t))
  (let ((mounts (loop for spec in specs
                      collect (make-mounted :spec spec
                                            :state (copy-data (spec-initial spec))))))
    (when mounts
      (transact (append (root-mounts (runtime-mounts)) mounts)
                (list :type :mount) nil (mapcar #'spec-name specs)))
    (mapcar (lambda (spec) (spec-name spec)) specs)))

(defun roll-back-patch (extensions snapshots refresh)
  "Return to the state before a patch that did not finish applying."
  (dolist (snapshot snapshots) (ignore-errors (restore-patch-definition snapshot)))
  (when extensions (ignore-errors (remove-patch-mounts extensions refresh)))
  (ignore-errors (sweep-source-packages)))

(defun load-policy (source &key name only refresh)
  "Load SOURCE, a config path or an embedded builtin name, and mount its extensions.
ONLY restricts the mount to those extension names; REFRESH re-invokes mounts."
  (let* ((specs (load-specs source))
         (specs (if only
                    (remove-if-not (lambda (spec) (member (spec-name spec) only :test #'equal))
                                   specs)
                    specs))
         (names (mapcar #'spec-name specs)))
    (unless (or specs (null only))
      (error "No extension named ~{~A~^, ~} in ~A." only source))
    (dolist (spec specs)
      (when (find (spec-name spec) (root-mounts (runtime-mounts)) :test #'equal
                  :key (lambda (mounted) (spec-name (mounted-spec mounted))))
        (error "Duplicate extension: ~A" (spec-name spec))))
    (let ((mounts (loop for spec in specs
                        collect (make-mounted :spec spec
                                              :state (copy-data (spec-initial spec))))))
      (when mounts
        (transact (append (root-mounts (runtime-mounts)) mounts)
                  (list :type :mount) nil names)))
    (when refresh (refresh-mounts))
    names))

(defun unload-policy (name)
  "Remove the mounted extension NAME and release its code when nothing pins it."
  (unmount name)
  (sweep-source-packages)
  name)

(defun %reload-policy ()
  (configure (runtime-sources)))

(defun %mount-policy (path)
  (configure (append (runtime-sources) (list path))))

(defun apply-patch (&key file forms name)
  "Apply FORMS or the source FILE as one named patch to the running session.
The patch's extensions mount like any source, and the definitions it makes are
recorded so UNDO-PATCH can restore them. A patch that fails anywhere leaves the
compositor as it was and signals its error. Returns a report plist."
  (cond ((and file forms) (error "A patch takes a file or forms, not both."))
        ((and (null file) (null forms)) (error "A patch needs a file or forms.")))
  (when (and name (not (patch-name-p name)))
    (error "Invalid patch name ~S: use letters, digits, hyphens and underscores." name))
  (let* ((id (1+ (runtime-patch-sequence)))
         (name (or name (format nil "patch-~D" id))))
    (when (find name (runtime-patches) :test #'equal
                :key (lambda (patch) (getf patch :name)))
      (error "Patch ~A is already applied; undo it first." name))
    (let* ((generated (null file))
           (path (if file file (write-patch-source name forms)))
           (package (format nil "TOMOE-USER/PATCH-~A" (string-upcase name)))
           (snapshots nil) (mounted nil) (shared nil))
      (%r! :source-packages (cons package (runtime-source-packages)))
      (%r! :patch-sequence id)
      (let ((failure nil))
        (%catch
         (lambda ()
           (let ((text (patch-text path)))
             (setf snapshots (loop for symbol in (patch-defined-symbols text package)
                                   collect (patch-definition-snapshot symbol))
                   shared (patch-shared-definitions-p snapshots))
             (setf mounted (mount-patch-specs (load-specs path package)))
             (when shared (refresh-mounts))
             (let ((patch (list :id id :name name :file path :generated generated
                                :refresh shared :snapshots snapshots
                                :extensions mounted
                                :definitions (loop for snapshot in snapshots
                                                   collect (getf snapshot :name)))))
               (%r! :patches (append (runtime-patches) (list patch)))
               (incf (runtime-generation)))))
         (lambda (text) (setf failure text)))
        (if failure
            (progn
              (roll-back-patch mounted snapshots shared)
              (when generated (ignore-errors (%delete-file path)))
              (error "~A" failure))
            (let ((patch (first (last (runtime-patches)))))
              (list :id id :name name :file path :refresh shared
                    :extensions (getf patch :extensions)
                    :definitions (getf patch :definitions)))))))))

(defun undo-patch (&key name id)
  "Withdraw the patch named NAME, numbered ID, or the most recent one."
  (when (and name id) (error "Undo a patch by name or id, not both."))
  (let ((patch (cond (id (or (find id (runtime-patches)
                                   :key (lambda (patch) (getf patch :id)))
                             (error "No patch ~D." id)))
                     (name (or (find name (runtime-patches) :test #'equal
                                     :key (lambda (patch) (getf patch :name)))
                               (error "No patch named ~A." name)))
                     (t (or (first (last (runtime-patches)))
                            (error "No patch to undo."))))))
    (let ((later (later-definition-patch patch))
          (extensions (getf patch :extensions)) (refresh (getf patch :refresh)))
      (when later
        (error "Patch ~A is older than ~A, which replaced the same definitions; undo ~A first."
               (getf patch :name) (getf later :name) (getf later :name)))
      (when extensions (remove-patch-mounts extensions nil))
      (dolist (snapshot (getf patch :snapshots)) (restore-patch-definition snapshot))
      (%r! :patches (remove patch (runtime-patches) :test #'eq))
      (sweep-source-packages)
      (when (getf patch :generated) (ignore-errors (%delete-file (getf patch :file))))
      (incf (runtime-generation))
      (let ((warning nil))
        (when refresh
          (%catch (lambda () (refresh-mounts))
                  (lambda (text) (setf warning text))))
        (list :id (getf patch :id) :name (getf patch :name) :warning warning
              :extensions (copy-list extensions)
              :definitions (copy-list (getf patch :definitions)))))))

(defun describe-patches ()
  (loop for patch in (runtime-patches)
        collect (list :id (getf patch :id) :name (getf patch :name)
                      :file (getf patch :file) :refresh (getf patch :refresh)
                      :extensions (copy-list (getf patch :extensions))
                      :definitions (copy-list (getf patch :definitions)))))

(defun patch-command (arguments)
  (apply-patch :file (getf arguments :file) :forms (getf arguments :forms)
               :name (getf arguments :name)))

(defun undo-patch-command (arguments)
  (undo-patch :name (getf arguments :name) :id (getf arguments :id)))

(define-host-command "patch"
  :description "Apply a patch file or forms to the running session."
  :arguments '((:file :string :description "A patch source file.")
               (:forms :string :description "Lisp forms instead of a file, up to 64 KiB.")
               (:name :string :description "Patch name, by default patch-N."))
  :call 'patch-command)

(define-host-command "undo-patch"
  :description "Undo a patch, restoring its definitions and removing its effects."
  :arguments '((:name :string :description "Patch name, by default the most recent patch.")
               (:id :integer :description "Patch number, when the name is unknown."))
  :call 'undo-patch-command)
