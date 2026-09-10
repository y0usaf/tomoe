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
    ;; Bound nesting with the standard list reader: it is the only one that
    ;; understands the cons dot the printer emits for extension state.
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
         ;; SBCL's readable printer encodes BASE-STRING as #A, outside our data
         ;; grammar. Escaped printing writes every string as a quoted string.
         (text (write-to-string form :readably nil :escape t :pretty nil :base 10
                                     :radix nil :level nil :length nil :circle nil :case :upcase)))
    (when (> (length text) 1048576) (error "Control frame exceeds 1 MiB characters."))
    (format stream "~D~%~A" (length text) text)
    (finish-output stream)))

(defun read-frame (stream)
  ;; The length counts decoded UTF-8 characters, not bytes. Framing permits
  ;; embedded newlines in titles and prevents READ from consuming another request.
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
           ;; A refused connection means the path was left behind by an exit that
           ;; skipped cleanup, so reclaim it. A live listener is never taken over.
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
  (destructuring-bind (version operation &rest args) request
    (unless (eql version +wire-version+)
      (error "Unsupported wire version ~S; expected ~D." version +wire-version+))
    (ecase operation
      (:inspect (destructuring-bind () args (describe-runtime runtime)))
      (:reload (destructuring-bind () args (configure runtime (runtime-sources runtime))) nil)
      (:mount
       (destructuring-bind (path) args
         (check-type path string)
         (let ((path (namestring (truename path))))
           (configure runtime (append (remove path (runtime-sources runtime) :test #'equal) (list path)) path)))
       nil)
      (:unmount
       (destructuring-bind (name) args (check-type name string) (unmount runtime name)) nil)
      (:command
       (destructuring-bind (owner name) args
         (check-type owner string) (check-type name string)
         (unless (find owner (runtime-mounts runtime) :test #'equal
                       :key (lambda (m) (spec-name (mounted-spec m))))
           (error "No mounted extension named ~A." owner))
         (unless (find-if (lambda (b) (and (equal owner (getf b :owner))
                                          (equal name (getf b :command))))
                          (getf (runtime-effective runtime) :bindings))
           (error "No active command ~A/~A." owner name))
         (transact runtime (runtime-mounts runtime)
                   (list :type :key :owner owner :command name) '(:key))) nil)
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
