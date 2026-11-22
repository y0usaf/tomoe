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
  (let ((text (%read-file path (%limit :patch))))
    (unless text (error "Cannot read the patch source ~A." path))
    text))

(defun patch-forms (path package)
  "Every top-level form of the source at PATH, read in PACKAGE without evaluation."
  (let ((text (patch-text path)))
    (when (> (length text) (%limit :patch)) (error "A patch source exceeds 1 MiB."))
    (%read-source text package)))

(defun patch-defined-symbols (path package)
  "The symbols the source at PATH, read in PACKAGE, defines at top level."
  (let ((symbols nil) (forms 0))
    (dolist (form (patch-forms path package))
      (when (> (incf forms) 4096)
        (error "A patch has more than 4096 top-level forms."))
      (when (and (consp form) (member (first form) +patch-definitions+ :test #'eq))
        (let ((name (second form)))
          (when (and (symbolp name) name)
            (unless (member name symbols :test #'eq) (push name symbols))))))
    (nreverse symbols)))

(defun patch-definition-snapshot (symbol patch-name index)
  "SYMBOL's definitions now, for undo to restore over the patch's own. The
definitions stay in the cells of a holder symbol, because the runtime state keeps
only copies and a restored definition must be the original object."
  (let ((holder (intern (format nil "%SNAPSHOT/~A/~D" patch-name index) 'tomoe)))
    (multiple-value-bind (value boundp) (%symbol-value symbol)
      (%set-symbol-definition holder (%symbol-definition symbol))
      (if boundp (%set-symbol-value holder value) (%unbind-symbol holder))
      (list :symbol symbol
            :name (let ((package (%symbol-package-name symbol)))
                    (if package
                        (format nil "~A::~A" package (symbol-name symbol))
                        (symbol-name symbol)))
            :holder holder
            :boundp boundp))))

(defun restore-patch-definition (snapshot)
  (%bump :definitions-generation)
  (let ((symbol (getf snapshot :symbol)) (holder (getf snapshot :holder)))
    (%set-symbol-definition symbol (%symbol-definition holder))
    (if (getf snapshot :boundp)
        (%set-symbol-value symbol (%symbol-value holder))
        (%unbind-symbol symbol))))

(defun refresh-extensions (&rest names)
  "What a patch declares: the extensions it refreshes, read before it loads."
  names)

(defun patch-refresh-list (path package)
  "The extension names the source at PATH declares with (REFRESH-EXTENSIONS ...)."
  (let ((names nil))
    (dolist (form (patch-forms path package) (%dedupe (nreverse names)))
      (when (and (consp form) (symbolp (first form))
                 (string= "REFRESH-EXTENSIONS" (symbol-name (first form))))
        (dolist (name (rest form))
          (push (if (stringp name) name (symbol-name name)) names))))))

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

(defun refresh-mounts (names)
  "Re-invoke the root reducers named NAMES, so changed definitions reach the next
commit."
  (let ((roots (root-mounts (runtime-mounts))))
    (transact roots (list :type :change :keys nil) nil
              (loop for mounted in roots
                    for name = (spec-name (mounted-spec mounted))
                    when (member name names :test #'equal) collect name))))

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
    (let ((failed (find-if (lambda (mounted)
                             (and (member (mounted-spec mounted) specs :test #'eq)
                                  (plusp (mounted-failures mounted))))
                           (root-mounts (runtime-mounts)))))
      (when failed
        (remove-patch-mounts (mapcar #'spec-name specs))
        (error "Extension ~A: ~A" (spec-name (mounted-spec failed)) (mounted-last-error failed))))
    (mapcar (lambda (spec) (spec-name spec)) specs)))

(defun roll-back-patch (mounts packages patches sequence snapshots)
  "Return the session to the MOUNTS, source PACKAGES, PATCHES, patch SEQUENCE and
definitions it held before a patch that did not finish applying. MOUNTS, rule
instances included, are committed as they were, without running any reducer."
  (dolist (snapshot snapshots) (restore-patch-definition snapshot))
  (unless (eq mounts (runtime-mounts))
    (let ((*materialized* (list nil -1 nil nil)))
      (%with-held-clock (lambda () (commit-context mounts (materialize mounts)))))
    (%r! :pending-context nil))
  (dolist (package (runtime-source-packages))
    (when (and (stringp package) (not (member package packages :test #'equal)))
      (%drop-package package)))
  (%r! :source-packages packages)
  (%r! :patches patches)
  (%r! :patch-sequence sequence))

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
    (unless (eq *library-specs* :outside)
      (setf *library-specs* (append *library-specs* specs))
      (return-from load-policy names))
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
    (when refresh (refresh-mounts refresh))
    names))

(defun unload-policy (name)
  "Remove the mounted extension NAME and release its code when nothing pins it."
  (unmount name)
  (sweep-source-packages)
  name)

(defun %reload-policy ()
  (configure (runtime-sources)))

(defun %mount-policy (path)
  (configure (if (member path (runtime-sources) :test #'equal)
                 (runtime-sources)
                 (append (runtime-sources) (list path)))
             path))

(defun apply-patch (&key file forms name)
  "Apply FORMS or the source FILE as one named patch to the running session.
The patch's extensions mount like any source, and the definitions it makes are
recorded so UNDO-PATCH can restore them. A patch that fails anywhere leaves the
compositor as it was and signals its error. Returns a report plist."
  (cond ((and file forms) (error "A patch takes a file or forms, not both."))
        ((and (null file) (null forms)) (error "A patch needs a file or forms.")))
  (when (and name (not (patch-name-p name)))
    (error "Invalid patch name ~S: use letters, digits, hyphens and underscores." name))
  (let* ((sequence (runtime-patch-sequence))
         (id (1+ sequence))
         (name (or name (format nil "patch-~D" id))))
    (when (find name (runtime-patches) :test #'equal
                :key (lambda (patch) (getf patch :name)))
      (error "Patch ~A is already applied; undo it first." name))
    (let* ((generated (null file))
           (path (if file file (write-patch-source name forms)))
           (package (format nil "TOMOE-USER/PATCH-~A" (string-upcase name)))
           (mounts (runtime-mounts))
           (packages (runtime-source-packages))
           (patches (runtime-patches))
           (snapshots nil) (failure nil))
      (without-deadline
       (lambda ()
         (%r! :source-packages (cons package packages))
         (%r! :patch-sequence id)
         (%catch
          (lambda ()
            (patch-text path)
            (setf snapshots (loop for symbol in (patch-defined-symbols path package)
                                  for index from 0
                                  collect (patch-definition-snapshot symbol name index)))
            (let* ((refresh (patch-refresh-list path package))
                   (mounted (mount-patch-specs (load-specs path package))))
              (refresh-mounts refresh)
              (%r! :patches (append patches
                                    (list (list :id id :name name :file path :generated generated
                                                :refresh refresh :snapshots snapshots
                                                :extensions mounted
                                                :definitions (loop for snapshot in snapshots
                                                                   collect (getf snapshot :name))))))
              (bump-generation)))
          (lambda (text) (setf failure text)))
         (when failure
           (%catch (lambda () (roll-back-patch mounts packages patches sequence snapshots))
                   (lambda (text)
                     (setf failure (format nil "~A; restoring the session before the patch also failed: ~A"
                                           failure text))))
           (when generated (ignore-errors (%delete-file path))))))
      (when failure
        (record-error (format nil "Patch ~A was refused and rolled back: ~A" name failure))
        (error "~A" failure))
      (let ((patch (first (last (runtime-patches)))))
        (list :id id :name name :file path :refresh (getf patch :refresh)
              :extensions (getf patch :extensions)
              :definitions (getf patch :definitions))))))

(defun undo-patch (&key name id)
  "Withdraw the patch named NAME, numbered ID, or the most recent one."
  (when (and name id) (error "Undo a patch by name or id, not both."))
  (without-deadline (lambda () (%undo-patch name id))))

(defun %undo-patch (name id)
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
      (bump-generation)
      (let ((warning nil))
        (%catch (lambda () (refresh-mounts refresh))
                (lambda (text) (setf warning text)))
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
