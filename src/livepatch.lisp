(in-package #:tomoe)

(defparameter +patch-limit+ 1048576)
(defparameter +patch-definitions+ '(defun defmacro defparameter defvar)
  "Top-level forms whose definitions a patch owns and undo restores.")

(defun patch-name-p (name)
  (and (stringp name)
       (<= 1 (length name) 40)
       (every (lambda (character) (or (alphanumericp character) (find character "-_")))
              name)))

(defun patch-directory ()
  (format nil "~A/tomoe-patches" (runtime-directory)))

(defun ensure-patch-directory ()
  (let ((path (patch-directory)))
    (unless (probe-file path) (sb-posix:mkdir path #o700))
    path))

(defun patch-text (path)
  "The UTF-8 text of the patch source at PATH."
  (with-open-file (stream path :element-type '(unsigned-byte 8))
    (when (> (file-length stream) +patch-limit+)
      (error "A patch source exceeds 1 MiB."))
    (let ((bytes (make-array (file-length stream) :element-type '(unsigned-byte 8))))
      (unless (= (read-sequence bytes stream) (length bytes))
        (error "Cannot read the whole patch source ~A." path))
      (sb-ext:octets-to-string bytes :external-format :utf-8))))

(defun write-patch-source (name text)
  "TEXT as the source file of patch NAME, readable only by this session."
  (let ((path (format nil "~A/~A.lisp" (ensure-patch-directory) name)))
    (with-open-file (stream path :direction :output :if-exists :supersede
                                 :if-does-not-exist :create
                                 :element-type 'character :external-format :utf-8)
      (write-string text stream))
    (sb-posix:chmod path #o600)
    path))

(defun patch-defined-symbols (text package)
  "The symbols TEXT defines at top level. Read in PACKAGE, as the loader will."
  (let ((*package* package) (*read-eval* nil) (*read-base* 10)
        (*read-default-float-format* 'double-float)
        (symbols nil) (forms 0))
    (with-input-from-string (stream text)
      (loop for form = (read stream nil :end)
            until (eq form :end)
            do (progn
                 (when (> (incf forms) 4096)
                   (error "A patch has more than 4096 top-level forms."))
                 (when (and (consp form) (member (first form) +patch-definitions+))
                   (let ((name (second form)))
                     (when (and (symbolp name) name) (pushnew name symbols))))))
      (nreverse symbols))))

(defun patch-definition-snapshot (symbol)
  "SYMBOL's definitions now, for undo to restore over the patch's own."
  (let ((value-p (boundp symbol)))
    (list :symbol symbol
          :name (let ((package (symbol-package symbol)))
                  (if package (format nil "~A::~A" (package-name package) (symbol-name symbol))
                      (symbol-name symbol)))
          :macro (macro-function symbol)
          :function (and (fboundp symbol) (symbol-function symbol))
          :boundp value-p
          :value (and value-p (symbol-value symbol)))))

(defun restore-patch-definition (snapshot)
  (incf *definitions-generation*)
  (let ((symbol (getf snapshot :symbol)) (macro (getf snapshot :macro))
        (function (getf snapshot :function)))
    (cond (macro (setf (macro-function symbol) macro))
          (function (setf (symbol-function symbol) function))
          ((fboundp symbol) (fmakunbound symbol)))
    (if (getf snapshot :boundp)
        (setf (symbol-value symbol) (getf snapshot :value))
        (when (boundp symbol) (makunbound symbol)))))

(defun patch-shared-definitions-p (snapshots package)
  "True when the patch redefines something that existed outside its own package."
  (not (null (some (lambda (snapshot)
                     (and (not (eq (symbol-package (getf snapshot :symbol)) package))
                          (or (getf snapshot :function) (getf snapshot :boundp))))
                   snapshots))))

(defun later-definition-patch (runtime patch)
  "The patch applied after PATCH that replaced one of the same definitions.
Undoing a redefinition out of order would restore a generation the newer patch
was written against, so such an undo is refused instead."
  (let ((symbols (loop for snapshot in (getf patch :snapshots)
                       collect (getf snapshot :symbol))))
    (find-if (lambda (other)
               (intersection symbols (loop for snapshot in (getf other :snapshots)
                                           collect (getf snapshot :symbol))))
             (cdr (member patch (runtime-patches runtime) :test #'eq)))))

(defun refresh-mounts (runtime)
  "Re-invoke every root reducer, so changed definitions reach the next commit."
  (let ((roots (root-mounts (runtime-mounts runtime))))
    (transact runtime roots (list :type :change :keys nil) nil
              (mapcar (lambda (mounted) (spec-name (mounted-spec mounted))) roots))))

(defun remove-patch-mounts (runtime extensions &optional refresh)
  "Drop the mounts named EXTENSIONS. REFRESH also re-invokes every other mount."
  (let ((remaining (remove-if (lambda (mounted)
                                (member (spec-name (mounted-spec mounted)) extensions
                                        :test #'equal))
                              (runtime-mounts runtime))))
    (transact runtime remaining (list :type :unmount) nil
              (when refresh
                (mapcar (lambda (mounted) (spec-name (mounted-spec mounted)))
                        (root-mounts remaining))))))

(defun mount-patch-specs (runtime specs)
  "Mount SPECS as one patch, after every name is known to be free."
  (dolist (spec specs)
    (when (find (spec-name spec) (root-mounts (runtime-mounts runtime)) :test #'equal
                :key (lambda (mounted) (spec-name (mounted-spec mounted))))
      (error "Duplicate extension: ~A" (spec-name spec)))
    (setf (spec-patch spec) t))
  (let ((mounts (loop for spec in specs
                      collect (make-mounted :spec spec
                                            :state (copy-data (spec-initial spec))))))
    (when mounts
      (transact runtime (append (root-mounts (runtime-mounts runtime)) mounts)
                (list :type :mount) nil (mapcar #'spec-name specs)))
    (mapcar (lambda (spec) (spec-name spec)) specs)))

(defun roll-back-patch (runtime extensions snapshots refresh)
  "Return to the state before a patch that did not finish applying."
  (dolist (snapshot snapshots) (ignore-errors (restore-patch-definition snapshot)))
  (when extensions (ignore-errors (remove-patch-mounts runtime extensions refresh)))
  (ignore-errors (sweep-source-packages runtime)))

(defun apply-patch (runtime &key file forms name)
  "Apply FORMS or the source FILE as one named patch to the running session.
The patch's extensions mount like any source, and the definitions it makes are
recorded so UNDO-PATCH can restore them. A patch that fails anywhere leaves the
compositor as it was and signals its error. Returns a report plist."
  (cond ((and file forms) (error "A patch takes a file or forms, not both."))
        ((and (null file) (null forms)) (error "A patch needs a file or forms.")))
  (when (and name (not (patch-name-p name)))
    (error "Invalid patch name ~S: use letters, digits, hyphens and underscores." name))
  (let* ((id (1+ (runtime-patch-sequence runtime)))
         (name (or name (format nil "patch-~D" id))))
    (when (find name (runtime-patches runtime) :test #'equal
                :key (lambda (patch) (getf patch :name)))
      (error "Patch ~A is already applied; undo it first." name))
    (let* ((generated (null file))
           (path (if file (source-path file) (write-patch-source name forms)))
           (package (fresh-source-package (format nil "TOMOE-USER/PATCH-~A"
                                                  (string-upcase name))))
           (snapshots nil) (mounted nil) (shared nil))
      (push package (runtime-source-packages runtime))
      (setf (runtime-patch-sequence runtime) id)
      (handler-case
          (let ((text (patch-text path)))
            (setf snapshots (loop for symbol in (patch-defined-symbols text package)
                                  collect (patch-definition-snapshot symbol))
                  shared (patch-shared-definitions-p snapshots package))
            (setf mounted (mount-patch-specs runtime (load-specs runtime path package)))
            (when shared (refresh-mounts runtime))
            (let ((patch (list :id id :name name :file path :generated generated
                               :refresh shared :snapshots snapshots
                               :extensions mounted
                               :definitions (loop for snapshot in snapshots
                                                  collect (getf snapshot :name)))))
              (setf (runtime-patches runtime)
                    (append (runtime-patches runtime) (list patch)))
              (incf (runtime-generation runtime))
              (list :id id :name name :file path :refresh shared
                    :extensions mounted
                    :definitions (getf patch :definitions))))
        (serious-condition (condition)
          (roll-back-patch runtime mounted snapshots shared)
          (when generated (ignore-errors (delete-file path)))
          (error condition))))))

(defun undo-patch (runtime &key name id)
  "Withdraw the patch named NAME, numbered ID, or the most recent one.
Its extensions unmount through the ordinary removal path, which retires the
timers, watches, processes and effects it owned, and the definitions it made or
replaced are restored before the commit that recomputes the session. Returns a
report plist."
  (when (and name id) (error "Undo a patch by name or id, not both."))
  (let ((patch (cond (id (or (find id (runtime-patches runtime)
                                   :key (lambda (patch) (getf patch :id)))
                             (error "No patch ~D." id)))
                     (name (or (find name (runtime-patches runtime) :test #'equal
                                     :key (lambda (patch) (getf patch :name)))
                               (error "No patch named ~A." name)))
                     (t (or (first (runtime-patches runtime))
                            (error "No patch to undo."))))))
    (let ((later (later-definition-patch runtime patch))
          (extensions (getf patch :extensions)) (refresh (getf patch :refresh)))
      (when later
        (error "Patch ~A is older than ~A, which replaced the same definitions; undo ~A first."
               (getf patch :name) (getf later :name) (getf later :name)))
      (when extensions (remove-patch-mounts runtime extensions nil))
      (dolist (snapshot (getf patch :snapshots)) (restore-patch-definition snapshot))
      (setf (runtime-patches runtime) (remove patch (runtime-patches runtime) :test #'eq))
      (sweep-source-packages runtime)
      (when (getf patch :generated) (ignore-errors (delete-file (getf patch :file))))
      (incf (runtime-generation runtime))
      (let ((warning (when refresh
                       (handler-case (progn (refresh-mounts runtime) nil)
                         (serious-condition (condition)
                           (record-error runtime condition)
                           (princ-to-string condition))))))
        (list :id (getf patch :id) :name (getf patch :name) :warning warning
              :extensions (copy-list extensions)
              :definitions (copy-list (getf patch :definitions)))))))

(defun describe-patches (runtime)
  (loop for patch in (runtime-patches runtime)
        collect (list :id (getf patch :id) :name (getf patch :name)
                      :file (getf patch :file) :refresh (getf patch :refresh)
                      :extensions (copy-list (getf patch :extensions))
                      :definitions (copy-list (getf patch :definitions)))))

(defun patch-command (runtime arguments)
  (apply-patch runtime :file (getf arguments :file) :forms (getf arguments :forms)
               :name (getf arguments :name)))

(defun undo-patch-command (runtime arguments)
  (undo-patch runtime :name (getf arguments :name) :id (getf arguments :id)))

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
