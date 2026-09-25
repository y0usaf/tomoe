(in-package #:tomoe)

(defconstant +json-frame-limit+ (* 1024 1024))
(defconstant +json-client-limit+ 128)
(defconstant +json-io-quota+ (* 16 1024))
(defconstant +json-request-quota+ 16)

(defstruct json-client
  socket
  (input (make-array 4096 :element-type '(unsigned-byte 8) :adjustable t :fill-pointer 0))
  (input-start 0) (input-scan 0)
  output output-tail (output-offset 0) (output-bytes 0)
  subscribed events read-eof dead)

(defstruct json-server socket path clients (cursor 0)
  (read-buffer (make-array 4096 :element-type '(unsigned-byte 8))))

(defun json-transport-log (message &optional condition)
  (format *error-output* "tomoe: JSON IPC ~A~@[ (~A)~]~%" message
          (when condition
            (let ((text (princ-to-string condition)))
              (subseq text 0 (min 240 (length text)))))))

(defun json-drop-client (client)
  (unless (json-client-dead client)
    (setf (json-client-dead client) t)
    (ignore-errors (sb-bsd-sockets:socket-close (json-client-socket client) :abort t)))
  (setf (json-client-output client) nil (json-client-output-tail client) nil
        (json-client-output-bytes client) 0
        (fill-pointer (json-client-input client)) 0 (json-client-input-start client) 0
        (json-client-input-scan client) 0)
  nil)

(defun open-json-control (path)
  (let ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream))
        (bound nil) (ready nil))
    (unwind-protect
         (progn
           (when (sb-ext:with-timeout 0.1 (socket-answering-p path))
             (error "Another session is listening on ~A." path))
           (ignore-errors (delete-file path))
           (sb-bsd-sockets:socket-bind socket path)
           (setf bound t)
           (sb-posix:chmod path #o600)
           (setf (sb-bsd-sockets:non-blocking-mode socket) t)
           (sb-bsd-sockets:socket-listen socket 16)
           (setf ready t)
           (make-json-server :socket socket :path path))
      (unless ready
        (ignore-errors (sb-bsd-sockets:socket-close socket :abort t))
        (when bound (ignore-errors (delete-file path)))))))

(defun close-json-control (server)
  (when server
    (dolist (client (json-server-clients server)) (json-drop-client client))
    (setf (json-server-clients server) nil)
    (when (json-server-socket server)
      (ignore-errors (sb-bsd-sockets:socket-close (json-server-socket server) :abort t))
      (setf (json-server-socket server) nil)
      (ignore-errors (delete-file (json-server-path server)))))
  nil)

(defun json-line-octets (value)
  (sb-ext:string-to-octets (concatenate 'string (write-json value) (string #\Newline))
                           :external-format :utf-8))

(defun json-queue-octets (client octets)
  (unless (json-client-dead client)
    (when (> (+ (json-client-output-bytes client) (length octets)) +json-frame-limit+)
      (json-transport-log "dropping a client whose output backlog exceeds 1 MiB")
      (return-from json-queue-octets (json-drop-client client)))
    (let ((cell (list octets)))
      (if (json-client-output-tail client)
          (setf (cdr (json-client-output-tail client)) cell)
          (setf (json-client-output client) cell))
      (setf (json-client-output-tail client) cell))
    (incf (json-client-output-bytes client) (length octets))
    t))

(defun json-send (server client value)
  (declare (ignore server))
  (unless (json-client-dead client)
    (json-queue-octets client (json-line-octets value))))

(defun json-broadcast (server event-name payload)
  (let ((octets nil))
    (dolist (client (json-server-clients server))
      (when (and (not (json-client-dead client)) (json-client-subscribed client)
                 (or (null (json-client-events client))
                     (member event-name (json-client-events client) :test #'equal)))
        (unless octets
          (setf octets (json-line-octets (json-object (cons "event" event-name)
                                                    (cons "payload" payload)))))
        (json-queue-octets client octets))))
  nil)

(defun json-socket-ready-p (socket direction)
  (sb-sys:wait-until-fd-usable (sb-bsd-sockets:socket-file-descriptor socket) direction 0 nil))

(defun json-flush-client (client quota)
  "Return bytes written. A zero result also covers a peer not currently writable."
  (let ((written 0))
    (handler-case
        (loop while (and (not (json-client-dead client)) (json-client-output client)
                         (< written quota) (json-socket-ready-p (json-client-socket client) :output)) do
          (let* ((octets (first (json-client-output client)))
                 (offset (json-client-output-offset client))
                 (count (min (- quota written) (- (length octets) offset)))
                 (buffer (if (zerop offset) octets (subseq octets offset (+ offset count))))
                 (sent (sb-bsd-sockets:socket-send (json-client-socket client) buffer count
                                                  :dontwait t :nosignal t)))
            (unless sent (return))
            (when (zerop sent) (json-drop-client client) (return))
            (incf written sent)
            (decf (json-client-output-bytes client) sent)
            (incf (json-client-output-offset client) sent)
            (when (= (json-client-output-offset client) (length octets))
              (pop (json-client-output client))
              (setf (json-client-output-offset client) 0)
              (unless (json-client-output client) (setf (json-client-output-tail client) nil)))))
      (sb-bsd-sockets:socket-error () (json-drop-client client)))
    written))

(defun json-compact-input (client)
  (let ((start (json-client-input-start client)) (input (json-client-input client)))
    (when (plusp start)
      (replace input input :start2 start)
      (decf (fill-pointer input) start)
      (setf (json-client-input-scan client) (max 0 (- (json-client-input-scan client) start)))
      (setf (json-client-input-start client) 0))))

(defun json-append-input (client octets count)
  (json-compact-input client)
  (let* ((input (json-client-input client)) (start (fill-pointer input)) (end (+ start count)))
    (when (> end (array-total-size input))
      (setf input (adjust-array input (max end (min (+ +json-frame-limit+ 4096)
                                                   (* 2 (array-total-size input)))))
            (json-client-input client) input))
    (setf (fill-pointer input) end)
    (replace input octets :start1 start :end2 count)))

(defun json-next-line (client)
  "Return octets, present-p, oversized-p; a blank line is still present."
  (let* ((input (json-client-input client)) (start (json-client-input-start client))
         (end (position 10 input :start (max start (json-client-input-scan client)))))
    (setf (json-client-input-scan client) (if end (1+ end) (length input)))
    (cond ((and end (> (- end start) +json-frame-limit+)) (values nil nil t))
          (end (setf (json-client-input-start client) (1+ end))
               (values (subseq input start end) t nil))
          ((> (- (length input) start) +json-frame-limit+) (values nil nil t))
          (t (values nil nil nil)))))

(defun json-blank-line-p (octets)
  (every (lambda (octet) (member octet '(9 10 11 12 13 32))) octets))

(defun json-dispatch-line (runtime server client octets)
  (unless (json-blank-line-p octets)
    (multiple-value-bind (request valid)
        (handler-case
            (values (parse-json (sb-ext:octets-to-string octets :external-format :utf-8)
                                :reject-duplicate-keys t) t)
          (error (condition) (json-transport-log "ignoring an invalid request line" condition) (values nil nil)))
      (when valid
        (handler-case (handle-json-request runtime server client request)
          (serious-condition (condition)
            (json-transport-log "request dispatch failed" condition)))))))

(defun json-read-client (runtime server client deadline)
  (let ((read-bytes 0) (requests 0)
        (buffer (json-server-read-buffer server)))
    (handler-case
        (loop while (and (not (json-client-dead client)) (< requests +json-request-quota+)
                         (< (get-internal-real-time) deadline)) do
          (multiple-value-bind (line present oversized) (json-next-line client)
            (when oversized
              (json-transport-log "dropping a client whose request exceeds 1 MiB")
              (json-drop-client client) (return))
            (cond (present
                   (incf requests)
                   (json-dispatch-line runtime server client line))
                  ((or (json-client-read-eof client) (>= read-bytes +json-io-quota+)
                       (not (json-socket-ready-p (json-client-socket client) :input)))
                   (return))
                  (t
                   (multiple-value-bind (received count)
                       (sb-bsd-sockets:socket-receive (json-client-socket client) buffer
                                                     (min 4096 (- +json-io-quota+ read-bytes)) :dontwait t)
                     (unless received (return))
                     (if (zerop count)
                         (setf (json-client-read-eof client) t)
                         (progn (incf read-bytes count) (json-append-input client buffer count))))))))
      (sb-bsd-sockets:socket-error () (json-drop-client client)))))

(defun json-client-drained-p (client)
  (and (json-client-read-eof client) (zerop (json-client-output-bytes client))
       (null (position 10 (json-client-input client) :start (json-client-input-start client)))))

(defun serve-json-control (runtime server)
  (when (and server (json-server-socket server))
    (let ((deadline (+ (get-internal-real-time) (max 1 (ceiling (* 8 internal-time-units-per-second) 1000)))))
      (setf (json-server-clients server) (delete-if #'json-client-dead (json-server-clients server)))
      (handler-case
          (loop repeat 8 while (and (< (get-internal-real-time) deadline)
                                    (json-socket-ready-p (json-server-socket server) :input)) do
            (let ((socket (sb-bsd-sockets:socket-accept (json-server-socket server))))
              (unless socket (return))
              (handler-case
                  (if (>= (length (json-server-clients server)) +json-client-limit+)
                      (sb-bsd-sockets:socket-close socket :abort t)
                      (progn
                        (setf (sb-bsd-sockets:non-blocking-mode socket) t)
                        (setf (json-server-clients server)
                              (nconc (json-server-clients server) (list (make-json-client :socket socket))))))
                (error (condition)
                  (ignore-errors (sb-bsd-sockets:socket-close socket :abort t))
                  (json-transport-log "accept setup failed" condition)))))
        (sb-bsd-sockets:socket-error (condition) (json-transport-log "accept failed" condition)))
      (let* ((clients (copy-list (json-server-clients server))) (count (length clients))
             (start (if (plusp count) (mod (json-server-cursor server) count) 0))
             (visited 0))
        (loop for index below count while (< (get-internal-real-time) deadline) do
          (incf visited)
          (let ((client (nth (mod (+ start index) count) clients)))
            (unless (json-client-dead client)
              (let ((written (json-flush-client client +json-io-quota+)))
                (json-read-client runtime server client deadline)
                (json-flush-client client (- +json-io-quota+ written)))
              (when (json-client-drained-p client) (json-drop-client client)))))
        (setf (json-server-cursor server) (+ start visited)))
      (setf (json-server-clients server) (delete-if #'json-client-dead (json-server-clients server)))))
  nil)

(defun json-control-pending-p (server)
  (some (lambda (client) (and (not (json-client-dead client))
                              (plusp (json-client-output-bytes client))))
        (json-server-clients server)))

(defun drain-json-control (server)
  "Flush a bounded batch of existing output only; return whether bytes remain."
  (dolist (client (json-server-clients server))
    (unless (json-client-dead client)
      (json-flush-client client +json-io-quota+)
      (when (json-client-drained-p client) (json-drop-client client))))
  (setf (json-server-clients server) (delete-if #'json-client-dead (json-server-clients server)))
  (json-control-pending-p server))

(defun json-cli-read-frame (client)
  (let ((buffer (make-array 4096 :element-type '(unsigned-byte 8))))
    (loop
      (multiple-value-bind (line present oversized) (json-next-line client)
        (when oversized (error "IPC response exceeds 1 MiB."))
        (when (and present (not (json-blank-line-p line)))
          (return-from json-cli-read-frame
            (parse-json (sb-ext:octets-to-string line :external-format :utf-8))))
        (unless present
          (multiple-value-bind (received count)
              (sb-bsd-sockets:socket-receive (json-client-socket client) buffer 4096)
            (when received
              (when (zerop count) (error "IPC server closed the connection."))
              (json-append-input client buffer count))))))))

(defun json-cli-frame-kind (frame)
  (unless (and (consp frame) (eq (first frame) :json-object)) (error "Invalid IPC response frame."))
  (let* ((missing :json-missing-field) (id (json-get frame "id"))
         (error (json-get frame "error")) (result (json-get frame "result" missing)))
    (cond ((and (typep id '(integer 0 18446744073709551615)) (stringp error)) :error)
          ((and (typep id '(integer 0 18446744073709551615)) (not (eq result missing))) :result)
          ((stringp (json-get frame "event")) :event)
          (t (error "Invalid IPC response frame.")))))

(defun json-msg-client (path method params)
  (let* ((socket (make-instance 'sb-bsd-sockets:local-socket :type :stream))
         (client (make-json-client :socket socket)))
    (unwind-protect
         (progn
           (sb-bsd-sockets:socket-connect socket path)
           (let ((octets (json-line-octets
                          (apply #'json-object (append (list (cons "id" 1) (cons "method" method))
                                                       (when params (list (cons "params" params)))))))
                 (offset 0))
             (when (> (length octets) (1+ +json-frame-limit+)) (error "IPC request exceeds 1 MiB."))
             (loop while (< offset (length octets)) do
               (let ((sent (sb-bsd-sockets:socket-send socket
                                                      (if (zerop offset) octets (subseq octets offset))
                                                      (- (length octets) offset) :nosignal t)))
                 (when sent
                   (when (zerop sent) (error "IPC server closed the connection."))
                   (incf offset sent)))))
           (loop for frame = (json-cli-read-frame client) for kind = (json-cli-frame-kind frame)
                 when (and (member kind '(:error :result)) (eql 1 (json-get frame "id"))) do
                   (when (eq kind :error) (error "~A" (json-get frame "error")))
                   (write-line (write-json (json-get frame "result") :pretty t))
                   (finish-output)
                   (return))
           (when (equal method "subscribe")
             (loop for frame = (json-cli-read-frame client) do
               (when (eq :event (json-cli-frame-kind frame))
                 (write-line (write-json (json-object (cons "event" (json-get frame "event"))
                                                     (cons "payload" (json-get frame "payload")))))
                 (finish-output))))
           0)
      (json-drop-client client))))
