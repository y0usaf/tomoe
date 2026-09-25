(in-package #:tomoe)

(defvar *read-depth* 0)
(defun read-data (text)
  "Read one data form. Disable dispatch macros and reader syntax that can run code."
  (let ((*read-eval* nil) (*readtable* (copy-readtable nil)) (*read-base* 10)
        (*read-depth* 0) (*package* (find-package :tomoe)))
    (dolist (char '(#\# #\' #\` #\,))
      (set-macro-character char (lambda (stream char)
                                  (declare (ignore stream))
                                  (error "Reader syntax ~S is not allowed in data." char))))
    (let ((read-list (get-macro-character #\( )))
      (set-macro-character #\(
                           (lambda (stream char)
                             (let ((*read-depth* (1+ *read-depth*)))
                               (when (> *read-depth* 64) (error "Data nesting exceeds 64."))
                               (funcall read-list stream char)))))
    (multiple-value-bind (value end) (read-from-string text)
      (unless (every (lambda (c) (find c '(#\Space #\Tab #\Newline #\Return))) (subseq text end))
        (error "Trailing data after the first form."))
      (copy-data value))))

(defun write-frame (stream form)
  (let* ((*package* (find-package :tomoe))
         (text (write-to-string form :readably nil :escape t :pretty nil :base 10
                                     :radix nil :level nil :length nil :circle nil :case :upcase)))
    (when (> (length text) 1048576) (error "Control frame exceeds 1 MiB characters."))
    (format stream "~D~%~A" (length text) text)
    (finish-output stream)))

(defun read-frame (stream)
  (let* ((digits (loop for i from 0 for c = (read-char stream)
                      until (char= c #\Newline)
                      do (when (or (= i 7) (not (digit-char-p c))) (error "Invalid frame length."))
                      collect c into chars
                      finally (return (coerce chars 'string))))
         (length (parse-integer digits)))
    (unless (<= 1 length 1048576) (error "Frame length outside 1..1048576."))
    (let ((text (make-string length)))
      (unless (= (read-sequence text stream) length) (error "Truncated control frame."))
      (read-data text))))

(defstruct control socket path)

(defun native-hit-test (runtime x y)
  "Read and copy one native hit-test response before its C buffer is reused."
  (flet ((plist-keys (value lengths)
           (when (and (listp value) (member (ignore-errors (length value)) lengths))
             (loop for key in value by #'cddr collect key)))
         (hit-string-p (value limit)
           (and (stringp value) (<= 1 (length value) limit) (not (find #\Null value)))))
   (let ((screen-x (%double-float x)) (screen-y (%double-float y)))
    (let ((text (%hit-test (runtime-backend runtime) screen-x screen-y)))
      (if text
          (let* ((result (read-data text))
                 (coordinates '(:screen-x :screen-y :world-x :world-y :surface-x :surface-y))
                 (keys (plist-keys result '(14 16)))
                 (ui-p (member :ui keys))
                 (ui (and ui-p (getf result :ui)))
                 (ui-keys (plist-keys ui '(8))))
            (unless (and keys (= (length (remove-duplicates keys)) (if ui-p 8 7))
                         (every (lambda (key) (member key (list* :id :ui coordinates))) keys)
                         (typep (getf result :id) '(integer 0 4294967295))
                         (every (lambda (key) (%finite-real-p (getf result key))) coordinates)
                         (or (not ui-p)
                             (and (zerop (getf result :id)) ui-keys
                                  (= (length (remove-duplicates ui-keys)) 4)
                                  (every (lambda (key) (member key '(:owner :surface :output :element))) ui-keys)
                                  (hit-string-p (getf ui :owner) 65536)
                                  (hit-string-p (getf ui :surface) 128)
                                  (hit-string-p (getf ui :output) 65536)
                                  (or (null (getf ui :element))
                                      (hit-string-p (getf ui :element) 128)))))
              (error "Native hit-test returned an invalid plist: ~S" result))
            result)
          (error "Native hit-test returned no result."))))))

(defun socket-answering-p (path)
  "True when a process is accepting connections on the Unix socket PATH."
  (let ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream)))
    (unwind-protect
         (handler-case (progn (sb-bsd-sockets:socket-connect socket path) t)
           (sb-bsd-sockets:socket-error () nil))
      (sb-bsd-sockets:socket-close socket :abort t))))

(defun open-control (path)
  (let ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream))
        (bound nil) (ready nil))
    (unwind-protect
         (progn
           (when (socket-answering-p path)
             (error "Another session is listening on ~A." path))
           (ignore-errors (delete-file path))
           (sb-bsd-sockets:socket-bind socket path)
           (setf bound t)
           (sb-posix:chmod path #o600)
           (sb-bsd-sockets:socket-listen socket 8)
           (setf ready t)
           (make-control :socket socket :path path))
      (unless ready
        (sb-bsd-sockets:socket-close socket)
        (when bound (delete-file path))))))

(defun close-control (control)
  (sb-bsd-sockets:socket-close (control-socket control))
  (delete-file (control-path control)))

(defun handle-request (runtime request)
  (reconcile-backend-observations runtime)
  (unless (and (runtime-running runtime) (not *stop-requested*))
    (error "Compositor is stopping."))
  (destructuring-bind (version operation &rest args) request
    (unless (eql version +wire-version+)
      (error "Unsupported wire version ~S; expected ~D." version +wire-version+))
    (ecase operation
      (:inspect (destructuring-bind () args (describe-runtime runtime)))
      (:hit-test
       (destructuring-bind (x y) args
         (native-hit-test runtime x y)))
      (:reload (destructuring-bind () args (configure runtime (runtime-sources runtime))) nil)
      (:mount
       (destructuring-bind (path) args
         (check-type path string)
         (let ((path (namestring (truename path))))
           (configure runtime
                      (if (member path (runtime-sources runtime) :test #'equal)
                          (runtime-sources runtime)
                          (append (runtime-sources runtime) (list path)))
                      path)))
       nil)
      (:unmount
       (destructuring-bind (name) args (check-type name string) (unmount runtime name)) nil)
      (:command
       (destructuring-bind (owner name) args
         (check-type owner string) (check-type name string)
         (unless (find owner (runtime-mounts runtime) :test #'equal
                       :key (lambda (m) (spec-name (mounted-spec m))))
           (error "No mounted extension named ~A." owner))
         (let* ((bindings (getf (runtime-effective runtime) :bindings))
                (press (find-if (lambda (b) (and (equal owner (getf b :owner))
                                                (equal name (getf b :command)))) bindings))
                (release (and (not press)
                              (find-if (lambda (b) (and (equal owner (getf b :owner))
                                                       (equal name (getf b :release)))) bindings))))
           (unless (or press release) (error "No active command ~A/~A." owner name))
           (transact runtime (runtime-mounts runtime)
                     (list :type :key :owner owner :command name
                           :state (if press :pressed :released)) '(:key)))) nil)
      (:event
       (destructuring-bind (text) args
         (check-type text string)
         (let ((event (read-data text)))
           (unless (and (listp event) (getf event :type))
             (error "Injected event must be a plist carrying :type."))
           (unless (member (getf event :type) '(:key :button :grab))
             (error "Injected event type must be :key, :button or :grab, not ~S."
                    (getf event :type)))
           (dispatch-event runtime event)))
       nil)
      (:quit (destructuring-bind () args (setf (runtime-running runtime) nil)) nil))))

(defun serve-control (runtime control)
  (when (sb-sys:wait-until-fd-usable
         (sb-bsd-sockets:socket-file-descriptor (control-socket control)) :input 0 nil)
    (let ((client (sb-bsd-sockets:socket-accept (control-socket control))))
      (unwind-protect
           (let ((stream (sb-bsd-sockets:socket-make-stream
                          client :input t :output t :element-type 'character :external-format :utf-8)))
             (handler-case
                 (let ((reply (handler-case
                                  (list +wire-version+ :ok
                                        (handle-request runtime (sb-ext:with-timeout 0.1 (read-frame stream))))
                                (serious-condition (condition)
                                  (record-error runtime condition)
                                  (list +wire-version+ :error (princ-to-string condition))))))
                   (sb-ext:with-timeout 0.1 (write-frame stream reply)))
               (serious-condition (condition) (record-error runtime condition))))
        (sb-bsd-sockets:socket-close client :abort t)))))

(defun control-client (path operation args)
  (let ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream)))
    (unwind-protect
         (sb-ext:with-timeout 5
           (sb-bsd-sockets:socket-connect socket path)
           (let ((stream (sb-bsd-sockets:socket-make-stream
                          socket :input t :output t :element-type 'character :external-format :utf-8)))
             (write-frame stream (list* +wire-version+ operation args))
             (destructuring-bind (version status result) (read-frame stream)
               (unless (= version +wire-version+) (error "Control server wire version mismatch."))
               (unless (member status '(:ok :error)) (error "Unknown control status ~S." status))
               (when (or result (eq status :error))
                 (write (list version status result) :pretty nil :readably nil :escape t)
                 (terpri))
               (if (eq status :ok) 0 1))))
      (sb-bsd-sockets:socket-close socket :abort t))))
