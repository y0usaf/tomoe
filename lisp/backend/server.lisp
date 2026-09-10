;;; A Wayland compositor server in Common Lisp on libwayland-server.
;;; Objects are data in one registry. Requests are table entries, so reloading
;;; this file changes behaviour without disturbing live clients, the socket, or
;;; the globals.
(in-package #:tomoe)

(defconstant +prot-read+ 1)
(defconstant +map-shared+ 1)

(defvar *objects* (make-hash-table) "internal id -> object plist")
(defvar *next-object-id* 0)
(defvar *globals* nil "interface names; a global's index is its data pointer")
(defvar *events* nil "queued event data for the policy runtime, newest first")
(defvar *last-buffer* nil "buffer id of the most recently committed surface")
(defvar *serial* 0)
(defvar *bind-hooks* (make-hash-table :test #'equal))
(defvar *handlers* (make-hash-table :test #'equal))
(defvar *selection* 0 "data source the last wl_data_device.set_selection named")

(defparameter *advertised*
  '(("wl_compositor" . 4) ("wl_subcompositor" . 1) ("wl_shm" . 1) ("wl_output" . 2)
    ("wl_seat" . 5) ("xdg_wm_base" . 1) ("wl_data_device_manager" . 3))
  "Conservative versions: no client will use a request we do not implement.")

(defparameter *global-order* '("wl_compositor" "wl_subcompositor" "wl_shm" "wl_output"
                               "wl_seat" "xdg_wm_base" "wl_data_device_manager"))

(defun advertised (name) (or (cdr (assoc name *advertised* :test #'equal)) 1))
(defun object (id) (and (integerp id) (gethash id *objects*)))

(defun set-object (id &rest keys-and-values)
  "Objects are plists in one registry; every mutation goes through here.
Setting a new key can move the list head, so the record is stored back."
  (let ((record (object id)))
    (when record
      (loop for (key value) on keys-and-values by #'cddr
            do (setf (getf record key) value))
      (setf (gethash id *objects*) record))
    record))
(defun push-event (&rest event) (push event *events*))
(defun serial () (incf *serial*))

(defun make-object (interface &key resource client)
  (let ((id (incf *next-object-id*)))
    (setf (gethash id *objects*)
          (list :id id :interface interface :resource resource :client client))
    id))

(defun make-child (entry interface new-id)
  "An object the client named, with the parent resource's negotiated version."
  (let* ((resource (resource-create (getf entry :client) (alien-at (interface-address interface))
                                    (if (getf entry :resource)
                                        (resource-version (getf entry :resource))
                                        (advertised interface))
                                    new-id))
         (id (make-object interface :resource resource :client (getf entry :client))))
    (resource-set-dispatcher resource (callable-pointer 'dispatch-request)
                             (sb-sys:int-sap 0) (sb-sys:int-sap id)
                             (callable-pointer 'destroy-resource))
    id))

(defun id-of-resource (resource-sap)
  "The registry id stored as the resource's user data, or NIL."
  (let ((data (resource-user-data (alien-at resource-sap))))
    (unless (or (null data) (zerop (sap-of data)))
      (sap-of data))))

(defun resource-client-id (entry) (getf entry :client))
(defun version-of (entry)
  (if (getf entry :resource) (resource-version (getf entry :resource)) 1))

;;; --------------------------------------------------------------- callables

(defmacro stage (name &body body)
  `(handler-case (progn ,@body)
     (serious-condition (condition) (error "stage ~A: ~A" ,name condition))))

(defun dispatch-request* (resource opcode message args)
  (let* ((id (stage "id" (and resource (id-of-resource (sap-of resource)))))
         (entry (stage "entry" (object id))))
    (cond
      ((null entry)
       (format *error-output* "tomoe-lisp: request for dead object ~D~%" id))
      (t
       (let* ((interface (getf entry :interface))
              (signature (stage "signature" (message-signature-of message)))
              (values (stage "decode" (decode-arguments signature (sap-of args))))
              (handler (gethash (cons interface opcode) *handlers*)))
         (cond
           (handler
            (handler-case (funcall handler entry values)
              (serious-condition (condition)
                (error "~A request ~D with ~S: ~A" interface opcode values condition))))
           (t
            (format *error-output* "tomoe-lisp: unhandled ~A request ~D~%"
                    interface opcode)))))))
  0)

(sb-alien:define-alien-callable dispatch-request sb-alien:int
    ((implementation (* t)) (resource (* t)) (opcode sb-alien:unsigned-int)
     (message (* t)) (args (* t)))
  (declare (ignore implementation))
  (handler-case
      (dispatch-request* resource opcode message args)
    (serious-condition (condition)
      (format *error-output* "tomoe-lisp: dispatch ~A~%" condition)
      0)))

(sb-alien:define-alien-callable bind-global sb-alien:void
    ((client (* t)) (data (* t)) (version sb-alien:unsigned-int) (id sb-alien:unsigned-int))
  (handler-case
      (let ((index (sap-of data)))
        (let ((name (nth index *globals*)))
          (let ((resource (resource-create client (alien-at (interface-address name))
                                           (min version (advertised name)) id)))
            (let ((object-id (make-object name :resource resource :client client)))
              (resource-set-dispatcher resource (callable-pointer 'dispatch-request)
                                       (sb-sys:int-sap 0) (sb-sys:int-sap object-id)
                                       (callable-pointer 'destroy-resource))
              (let ((hook (gethash name *bind-hooks*)))
                (when hook (funcall hook object-id)))))))
    (serious-condition (condition)
      (format *error-output* "tomoe-lisp: bind failed: ~A~%" condition))))

(sb-alien:define-alien-callable destroy-resource sb-alien:void ((resource (* t)))
  (handler-case
      (let* ((resource-sap (sap-of resource))
             (id (id-of-resource resource-sap))
             (entry (object id)))
        (when entry
          (let ((role (getf entry :role)))
            (cond
              ((equal (getf entry :interface) "xdg_toplevel")
               (push-event :type :unmap :id id))
              ((and role (object role) (getf (object role) :mapped))
               (set-object role :mapped nil)
               (push-event :type :unmap :id role))))
          (remhash id *objects*)))
    (serious-condition (condition)
      (format *error-output* "tomoe-lisp: destroy failed: ~A~%" condition)
      (sb-debug:print-backtrace :count 14))))

;;; ---------------------------------------------------------------- globals

(defun create-globals (display)
  (setf *globals* (copy-list *global-order*))
  (loop for name in *globals* for index from 0 do
    (global-create display (alien-at (interface-address name)) (advertised name)
                   (sb-sys:int-sap index)
                   (callable-pointer 'bind-global))))

;;; --------------------------------------------------------------- requests

(defun empty-array ()
  "A zeroed struct wl_array { size_t size; size_t alloc; void *data; } for states."
  (let ((array (allocate 24)))
    (write-pointer (sb-alien:alien-sap array) 0 0)
    array))

(defun send-configure (toplevel-id)
  "xdg_toplevel.configure events first, then xdg_surface.configure."
  (let* ((toplevel (object toplevel-id))
         (xdg (object (getf toplevel :xdg-surface)))
         (states (empty-array)))
    (send-event toplevel 0 (or (getf toplevel :width) 1280) (or (getf toplevel :height) 720)
                (sap-of states))
    (send-event xdg 0 (serial))
    (setf (getf xdg :configured) t)))

(defun handle-ignore (entry values) (declare (ignore entry values)) nil)

(defun handle-compositor-surface (entry values)
  (destructuring-bind (new-id) values
    (let ((surface (make-child entry "wl_surface" new-id)))
      (set-object surface :frame-callbacks nil :pending :none))))

(defun handle-compositor-region (entry values)
  (destructuring-bind (new-id) values (make-child entry "wl_region" new-id)))

(defun handle-surface-attach (entry values)
  "attach(NULL) is a detach; :none means no attach since the last commit."
  (destructuring-bind (buffer x y) values
    (declare (ignore x y))
    (set-object (getf entry :id)
                :pending (if (zerop buffer) :detached (id-of-resource buffer)))))

(defun handle-surface-frame (entry values)
  (destructuring-bind (new-id) values
    (let* ((id (getf entry :id))
           (callback (make-child entry "wl_callback" new-id)))
      (set-object id :frame-callbacks
                  (cons callback (getf (object id) :frame-callbacks))))))

(defun handle-commit (entry values)
  (declare (ignore values))
  (let* ((id (getf entry :id))
         (surface (object id))
         (role (getf surface :role))
         (toplevel (and role (object role)))
         (pending (getf surface :pending))
         (buffer (if (eql pending :detached) nil (getf surface :buffer))))
    (dolist (callback (getf surface :frame-callbacks))
      (send-event (object callback) 0 (serial)))
    (set-object id :frame-callbacks nil)
    (when (and toplevel buffer)
      (let ((record (object buffer)))
        (setf *last-buffer* buffer)
        (set-object role :width (getf record :width) :height (getf record :height))
        (unless (getf toplevel :mapped)
          (set-object role :mapped t)
          (push-event :type :map :id role
                      :title (or (getf toplevel :title) "")
                      :app-id (or (getf toplevel :app-id) "")
                      :width (getf record :width) :height (getf record :height)))))
    (when (and toplevel (null buffer)
               (not (getf (object (getf toplevel :xdg-surface)) :configured)))
      (send-configure role))
    (unless (eql pending :none)
      (set-object id :buffer (unless (eql pending :detached) pending) :pending :none))))

(defun handle-shm-create-pool (entry values)
  (destructuring-bind (new-id fd size) values
    (let* ((pool (make-child entry "wl_shm_pool" new-id))
           (address (sb-posix:mmap nil size +prot-read+ +map-shared+ fd 0)))
      (sb-posix:close fd)
      (set-object pool :size size :address (sap-of address))))

(defun handle-shm-pool-create-buffer (entry values)
  (destructuring-bind (new-id offset width height stride format) values
    (let ((buffer (make-child entry "wl_buffer" new-id)))
      (set-object buffer :pool (getf entry :id) :offset offset :width width
                  :height height :stride stride :format format)))))

(defun handle-get-surface (entry values)
  (destructuring-bind (new-id surface-sap) values
    (let* ((surface (id-of-resource surface-sap))
           (xdg (make-child entry "xdg_surface" new-id)))
      (set-object xdg :surface surface)
      (set-object surface :role xdg))))

(defun handle-get-toplevel (entry values)
  (destructuring-bind (new-id) values
    (let* ((xdg-id (getf entry :id))
           (surface-id (getf (object xdg-id) :surface))
           (toplevel (make-child entry "xdg_toplevel" new-id)))
      (set-object toplevel :surface surface-id :xdg-surface xdg-id)
      (set-object xdg-id :toplevel toplevel)
      (set-object surface-id :role toplevel))))

(defun handle-set-title (entry values)
  (destructuring-bind (title) values
    (let ((toplevel (object (getf entry :id))))
      (setf (getf toplevel :title) title)
      (when (getf toplevel :mapped)
        (push-event :type :metadata :id (getf entry :id) :title title
                    :app-id (or (getf toplevel :app-id) "")
                    :width (or (getf toplevel :width) 0)
                    :height (or (getf toplevel :height) 0))))))

(defun handle-set-app-id (entry values)
  (destructuring-bind (app-id) values
    (set-object (getf entry :id) :app-id app-id)))

(defun handle-set-selection (entry values)
  "Clipboard selection is accepted and recorded; no offer is sent yet."
  (destructuring-bind (source serial) values
    (declare (ignore serial))
    (setf *selection* source)))

(defun handle-ack-configure (entry values)
  (destructuring-bind (serial) values
    (set-object (getf entry :id) :acked serial)))

(defun handle-destroy-object (entry values)
  (declare (ignore values))
  (let ((id (getf entry :id)))
    (when (object id)
      (remhash id *objects*)
      (push-event :type :unmap :id id))))

(defmacro define-create-handler (name interface)
  "Requests that only name a new object of INTERFACE; later arguments are ignored."
  `(defun ,name (entry values)
     (destructuring-bind (new-id &rest ignored) values
       (declare (ignore ignored))
       (make-child entry ,interface new-id))))

(define-create-handler handle-get-pointer "wl_pointer")
(define-create-handler handle-get-keyboard "wl_keyboard")
(define-create-handler handle-get-touch "wl_touch")
(define-create-handler handle-get-positioner "xdg_positioner")
(define-create-handler handle-get-subsurface "wl_subsurface")
(define-create-handler handle-get-popup "xdg_popup")
(define-create-handler handle-get-data-source "wl_data_source")
(define-create-handler handle-get-data-device "wl_data_device")

;;; ----------------------------------------------------------- bind hooks

(defun bind-shm (id)
  (send-event (object id) 0 0)
  (send-event (object id) 0 1))

(defun bind-output (id)
  (let ((output (object id)))
    (send-event output 0 0 0 600 340 0 "tomoe" "nested" 0)
    (send-event output 1 1 1280 720 60000)
    (send-event output 3 1)
    (send-event output 2)))

(defun bind-seat (id)
  (send-event (object id) 0 0)
  (send-event (object id) 1 "seat0"))

(defparameter +handler-table+
  '(("wl_compositor" (0 . handle-compositor-surface) (1 . handle-compositor-region))
    ("wl_surface" (0 . handle-destroy-object) (1 . handle-surface-attach) (2 . handle-ignore)
     (3 . handle-surface-frame) (4 . handle-ignore) (5 . handle-ignore) (6 . handle-commit)
     (7 . handle-ignore) (8 . handle-ignore) (9 . handle-ignore) (10 . handle-ignore))
    ("wl_region" (0 . handle-ignore) (1 . handle-ignore) (2 . handle-destroy-object))
    ("wl_shm" (0 . handle-shm-create-pool) (1 . handle-destroy-object))
    ("wl_shm_pool" (0 . handle-shm-pool-create-buffer) (1 . handle-destroy-object) (2 . handle-ignore))
    ("wl_buffer" (0 . handle-destroy-object))
    ("wl_callback" (0 . handle-destroy-object))
    ("wl_output" (0 . handle-destroy-object))
    ("wl_seat" (0 . handle-get-pointer) (1 . handle-get-keyboard) (2 . handle-get-touch) (3 . handle-destroy-object))
    ("wl_subcompositor" (0 . handle-destroy-object) (1 . handle-get-subsurface))
    ("xdg_wm_base" (0 . handle-destroy-object) (1 . handle-get-positioner) (2 . handle-get-surface) (3 . handle-ignore))
    ("xdg_positioner" (0 . handle-destroy-object) (1 . handle-ignore) (2 . handle-ignore) (3 . handle-ignore)
     (4 . handle-ignore) (5 . handle-ignore) (6 . handle-ignore) (7 . handle-ignore) (8 . handle-ignore)
     (9 . handle-ignore))
    ("xdg_surface" (0 . handle-destroy-object) (1 . handle-get-toplevel) (2 . handle-get-popup)
     (3 . handle-ignore) (4 . handle-ack-configure))
    ("xdg_toplevel" (0 . handle-destroy-object) (1 . handle-ignore) (2 . handle-set-title)
     (3 . handle-set-app-id) (4 . handle-ignore) (5 . handle-ignore) (6 . handle-ignore)
     (7 . handle-ignore) (8 . handle-ignore) (9 . handle-ignore) (10 . handle-ignore)
     (11 . handle-ignore) (12 . handle-ignore) (13 . handle-ignore))
    ("xdg_popup" (0 . handle-destroy-object) (1 . handle-ignore) (2 . handle-ignore))
    ("wl_subsurface" (0 . handle-destroy-object) (1 . handle-ignore) (2 . handle-ignore)
     (3 . handle-ignore) (4 . handle-ignore) (5 . handle-ignore))
    ("wl_data_device_manager" (0 . handle-get-data-source) (1 . handle-get-data-device)
     (2 . handle-destroy-object))
    ("wl_data_source" (0 . handle-ignore) (1 . handle-destroy-object) (2 . handle-ignore))
    ("wl_data_device" (0 . handle-ignore) (1 . handle-set-selection) (2 . handle-destroy-object))
    ("wl_data_offer" (0 . handle-ignore) (1 . handle-ignore) (2 . handle-destroy-object)
     (3 . handle-ignore) (4 . handle-ignore))))

(defun register-protocol ()
  (setf *handlers* (make-hash-table :test #'equal)
        *bind-hooks* (make-hash-table :test #'equal))
  (dolist (group +handler-table+)
    (destructuring-bind (interface . handlers) group
      (dolist (handler handlers)
        (setf (gethash (cons interface (car handler)) *handlers*) (cdr handler)))))
  (setf (gethash "wl_shm" *bind-hooks*) 'bind-shm
        (gethash "wl_output" *bind-hooks*) 'bind-output
        (gethash "wl_seat" *bind-hooks*) 'bind-seat)
  t)

;;; --------------------------------------------------------- backend contract

(defun open-backend (socket-name)
  (register-tables +core-protocol-tables+)
  (register-tables +xdg-shell-protocol-tables+)
  (build-interfaces)
  (register-protocol)
  (let* ((display (display-create))
         (status (display-add-socket display socket-name)))
    (unless (zerop status) (error "Cannot create socket ~A (status ~D)." socket-name status))
    (create-globals display)
    (push-event :type :outputs
                :outputs (list (list :name "tomoe-lisp-0" :x 0 :y 0 :width 1280 :height 720)))
    (list :display display
          :event-loop (display-event-loop display)
          :fd (event-loop-fd (display-event-loop display))
          :socket socket-name)))

(defun %step (backend timeout)
  (when (sb-sys:wait-until-fd-usable (getf backend :fd) :input (/ timeout 1000))
    (event-loop-dispatch (getf backend :event-loop) 0))
  (display-flush-clients (getf backend :display))
  0)

(defun %event (backend)
  (declare (ignore backend))
  (let ((event (pop *events*)))
    (and event (with-standard-io-syntax
                 (let ((*print-circle* nil)) (prin1-to-string event))))))

(defun %place (backend id x y width height visible)
  (declare (ignore backend x y visible))
  (let ((toplevel (object id)))
    (when (and toplevel (equal (getf toplevel :interface) "xdg_toplevel"))
      (unless (and (= (or (getf toplevel :width) 0) width)
                   (= (or (getf toplevel :height) 0) height))
        (setf (getf toplevel :width) width (getf toplevel :height) height)
        (when (getf toplevel :mapped) (send-configure id))))))

(defun %focus (backend id) (declare (ignore backend)) (when (object id) id))
(defun %close (backend id) (declare (ignore backend))
  (let ((toplevel (object id)))
    (when (and toplevel (equal (getf toplevel :interface) "xdg_toplevel"))
      (send-event toplevel 1))))
(defun %bind (backend modifiers code owner command)
  (declare (ignore backend owner command))
  (if (and (integerp modifiers) (integerp code) (plusp code)) 1 0))
(defun %clear-bindings (backend) (declare (ignore backend)) nil)

;; The Lisp backend has no input devices, so it never holds a pointer grab.
(defun %grab (backend id mode)
  (declare (ignore backend id mode))
  nil)

;; The Lisp backend has no xdg_toplevel state: a client keeps whatever layout it gets.
(defun %window-state (backend id fullscreen maximize)
  (declare (ignore backend id fullscreen maximize))
  nil)

;; The Lisp backend has no layer surfaces and advertises no layer-shell global.
(defun %layer (backend id layer exclusive-zone keyboard visible)
  (declare (ignore backend id layer exclusive-zone keyboard visible))
  nil)

(defun %keysym (name) (%keysym-from-name name 0))

(defun tomoe::configure-native-outputs (backend outputs)
  (declare (ignore backend outputs))
  (error "Output configuration requires the wlroots backend; the Lisp backend has no hardware output support yet."))

(defun %destroy (backend)
  (maphash (lambda (id entry)
             (declare (ignore id))
             (let ((pool (and (equal (getf entry :interface) "wl_shm_pool")
                              (getf entry :address))))
               (when (and pool (plusp pool))
                 (sb-posix:munmap (sb-sys:int-sap pool) (getf entry :size)))))
           *objects*)
  (clrhash *objects*)
  (setf *events* nil)
  (display-destroy (getf backend :display)))

;;; ------------------------------------------------------------- inspection

(defun buffer-record (id) (getf (object id) :offset))
(defun buffer-pixels (id)
  "Mapped pixels of a committed buffer: values are the SAP, stride, width, height."
  (let* ((buffer (object id))
         (pool (object (getf buffer :pool)))
         (address (+ (getf pool :address) (getf buffer :offset))))
    (values (sb-sys:int-sap address) (getf buffer :stride)
            (getf buffer :width) (getf buffer :height))))

(defun buffer-statistics (id)
  "Distinct colours and non-zero byte share, to show the client actually drew."
  (multiple-value-bind (sap stride width height) (buffer-pixels id)
    (let ((colours (make-hash-table)) (non-zero 0) (total 0) (row (min 64 height)))
      (loop for y from 0 below row do
        (loop for x from 0 below (min 256 width) do
          (let* ((offset (+ (* y stride) (* x 4)))
                 (pixel (sb-sys:sap-ref-32 sap offset)))
            (setf (gethash pixel colours) t)
            (incf total)
            (unless (zerop pixel) (incf non-zero)))))
      (values (hash-table-count colours) non-zero total))))

(defun write-ppm (id path)
  (multiple-value-bind (sap stride width height) (buffer-pixels id)
    (with-open-file (stream path :direction :output :element-type '(unsigned-byte 8)
                                 :if-exists :supersede)
      (flet ((write-text (text)
               (write-sequence (map 'vector #'char-code text) stream)))
        (write-text (format nil "P6~%~D ~D~%255~%" width height))
        (loop for y from 0 below height do
          (loop for x from 0 below width do
            (let* ((offset (+ (* y stride) (* x 4)))
                   (pixel (sb-sys:sap-ref-32 sap offset)))
              (write-byte (ldb (byte 8 16) pixel) stream)
              (write-byte (ldb (byte 8 8) pixel) stream)
              (write-byte (ldb (byte 8 0) pixel) stream)))))))
  path)
