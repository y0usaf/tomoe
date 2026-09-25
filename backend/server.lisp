(in-package #:tomoe)

(setf *backend-kind* :lisp)

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


(defmacro stage (name &body body)
  `(handler-case (progn ,@body)
     (serious-condition (condition) (error "stage ~A: ~A" ,name condition))))

(defun dispatch-request* (resource opcode message args)
  (let* ((id (stage "id" (and resource (id-of-resource (sap-of resource)))))
         (entry (stage "entry" (object id))))
    (cond
      ((null entry)
       (format *error-output* "tomoe: request for dead object ~D~%" id))
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
            (format *error-output* "tomoe: unhandled ~A request ~D~%"
                    interface opcode)))))))
  0)

(sb-alien:define-alien-callable dispatch-request sb-alien:int
    ((implementation (* t)) (resource (* t)) (opcode sb-alien:unsigned-int)
     (message (* t)) (args (* t)))
  (declare (ignore implementation))
  (handler-case
      (dispatch-request* resource opcode message args)
    (serious-condition (condition)
      (format *error-output* "tomoe: dispatch ~A~%" condition)
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
      (format *error-output* "tomoe: bind failed: ~A~%" condition))))

(defun withdraw-toplevel (id)
  "Retire an admitted window once, independently of its current buffer."
  (let ((toplevel (object id)))
    (when (and (equal (getf toplevel :interface) "xdg_toplevel")
               (getf toplevel :mapped))
      (set-object id :mapped nil :buffered nil :client-width 0 :client-height 0)
      (push-event :type :unmap :id id))))

(defun forget-object (id)
  "Only destruction of a toplevel or its root withdraws a window registry ID."
  (let ((entry (object id)))
    (when entry
      (cond ((equal (getf entry :interface) "xdg_toplevel") (withdraw-toplevel id))
            ((equal (getf entry :interface) "wl_surface") (withdraw-toplevel (getf entry :role)))
            ((equal (getf entry :interface) "xdg_surface") (withdraw-toplevel (getf entry :toplevel))))
      (remhash id *objects*))))

(sb-alien:define-alien-callable destroy-resource sb-alien:void ((resource (* t)))
  (handler-case
      (forget-object (id-of-resource (sap-of resource)))
    (serious-condition (condition)
      (format *error-output* "tomoe: destroy failed: ~A~%" condition)
      (sb-debug:print-backtrace :count 14))))


(defun create-globals (display)
  (setf *globals* (copy-list *global-order*))
  (loop for name in *globals* for index from 0 do
    (global-create display (alien-at (interface-address name)) (advertised name)
                   (sb-sys:int-sap index)
                   (callable-pointer 'bind-global))))


(defun empty-array ()
  "A zeroed struct wl_array { size_t size; size_t alloc; void *data; } for states."
  (let ((array (allocate 24)))
    (write-pointer (sb-alien:alien-sap array) 0 0)
    array))

(defun send-configure (toplevel-id)
  "xdg_toplevel.configure events first, then xdg_surface.configure."
  (let* ((toplevel (object toplevel-id))
         (xdg (object (getf toplevel :xdg-surface)))
         (states (empty-array))
         (width (or (getf toplevel :width) 1280))
         (height (or (getf toplevel :height) 720)))
    (send-event toplevel 0 width height (sap-of states))
    (send-event xdg 0 (serial))
    (set-object toplevel-id :last-configure-width width
                :last-configure-height height :configure-sent t)
    (set-object (getf toplevel :xdg-surface) :configured t)))

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

(defun handle-surface-buffer-transform (entry values)
  (destructuring-bind (transform) values
    (unless (<= 0 transform 7) (error "Invalid buffer transform ~D." transform))
    (set-object (getf entry :id) :pending-buffer-transform transform)))

(defun handle-surface-buffer-scale (entry values)
  (destructuring-bind (scale) values
    (unless (plusp scale) (error "Invalid buffer scale ~D." scale))
    (set-object (getf entry :id) :pending-buffer-scale scale)))

(defun handle-set-window-geometry (entry values)
  (destructuring-bind (x y width height) values
    (unless (and (plusp width) (plusp height))
      (error "Invalid xdg window geometry ~S." values))
    (set-object (getf entry :id) :pending-window-geometry (list x y width height))))

(defun committed-window-size (surface xdg)
  "Committed logical size, independent of the toplevel's configure request."
  (let* ((scale (getf surface :buffer-scale 1))
         (width (floor (getf surface :buffer-width 0) scale))
         (height (floor (getf surface :buffer-height 0) scale))
         (geometry (getf xdg :window-geometry)))
    (when (member (getf surface :buffer-transform 0) '(1 3 5 7))
      (rotatef width height))
    (if geometry
        (destructuring-bind (x y requested-width requested-height) geometry
          (let ((clamped-width (- (min width (+ x requested-width)) (max 0 x)))
                (clamped-height (- (min height (+ y requested-height)) (max 0 y))))
            (if (and (plusp clamped-width) (plusp clamped-height))
                (values clamped-width clamped-height)
                (values width height))))
        (values width height))))

(defun handle-commit (entry values)
  (declare (ignore values))
  (let* ((id (getf entry :id))
         (surface (object id))
         (role (getf surface :role))
         (toplevel (and role (object role)))
         (xdg-id (getf toplevel :xdg-surface))
         (was-buffered (not (null (getf surface :buffer))))
         (admitted (getf toplevel :mapped))
         (pending (getf surface :pending))
         (buffer (and (not (eql pending :none))
                      (not (eql pending :detached))
                      pending)))
    (dolist (callback (getf surface :frame-callbacks))
      (send-event (object callback) 0 (serial)))
    (set-object id :frame-callbacks nil)
    (dolist (keys '((:pending-buffer-scale :buffer-scale)
                    (:pending-buffer-transform :buffer-transform)))
      (let ((value (getf surface (first keys))))
        (when value (set-object id (second keys) value (first keys) nil))))
    (when xdg-id
      (let ((geometry (getf (object xdg-id) :pending-window-geometry)))
        (when geometry
          (set-object xdg-id :window-geometry geometry :pending-window-geometry nil))))
    (unless (eql pending :none)
      (set-object id :buffer (unless (eql pending :detached) pending) :pending :none))
    (when (and toplevel buffer)
      (let ((record (object buffer)))
        (setf *last-buffer* buffer)
        (unless admitted
          (set-object role :width (getf record :width) :height (getf record :height)))
        (set-object id :buffer-width (getf record :width) :buffer-height (getf record :height))))
    (when (and xdg-id admitted was-buffered (eql pending :detached))
      (set-object role :buffered nil :client-width 0 :client-height 0 :configure-sent nil)
      (set-object xdg-id :configured nil :acked nil :window-geometry nil :pending-window-geometry nil)
      (push-event :type :buffer :id role :attached nil :width 0 :height 0
                  :fullscreen nil :maximize nil)
      (return-from handle-commit))
    (when (and xdg-id (getf (object id) :buffer))
      (multiple-value-bind (width height) (committed-window-size (object id) (object xdg-id))
        (let ((changed (or (not (eql width (getf toplevel :client-width)))
                           (not (eql height (getf toplevel :client-height))))))
          (set-object role :buffered t :client-width width :client-height height)
          (cond
            ((and buffer (not admitted))
             (set-object role :mapped t
                              :map-width (getf (object buffer) :width)
                              :map-height (getf (object buffer) :height))
             (push-event :type :map :id role
                         :title (or (getf toplevel :title) "")
                         :app-id (or (getf toplevel :app-id) "")
                         :width (getf (object buffer) :width) :height (getf (object buffer) :height)
                         :client-width width :client-height height :buffered t))
            ((and admitted (not was-buffered))
             (push-event :type :buffer :id role :attached t :width width :height height
                         :fullscreen nil :maximize nil))
            ((and admitted changed)
             (push-event :type :geometry :id role :width width :height height))))))
    (when (and toplevel (null buffer)
               (not (getf (object (getf toplevel :xdg-surface)) :configured)))
      (send-configure role))))

(defun handle-shm-create-pool (entry values)
  (destructuring-bind (new-id fd size) values
    (let* ((pool (make-child entry "wl_shm_pool" new-id))
           (address (sb-posix:mmap nil size +prot-read+ +map-shared+ fd 0)))
      (sb-posix:close fd)
      (set-object pool :size size :address (sap-of address)))))

(defun handle-shm-pool-create-buffer (entry values)
  (destructuring-bind (new-id offset width height stride format) values
    (let ((buffer (make-child entry "wl_buffer" new-id)))
      (set-object buffer :pool (getf entry :id) :offset offset :width width
                  :height height :stride stride :format format))))

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

(defun toplevel-metadata (id)
  (let ((toplevel (object id)))
    (when (getf toplevel :mapped)
      (push-event :type :metadata :id id :title (or (getf toplevel :title) "")
                  :app-id (or (getf toplevel :app-id) "")
                  :width (or (getf toplevel :map-width) (getf toplevel :width) 0)
                  :height (or (getf toplevel :map-height) (getf toplevel :height) 0)
                  :client-width (getf toplevel :client-width)
                  :client-height (getf toplevel :client-height)
                  :buffered (and (getf toplevel :buffered) t)))))

(defun handle-set-title (entry values)
  (destructuring-bind (title) values
    (set-object (getf entry :id) :title title)
    (toplevel-metadata (getf entry :id))))

(defun handle-set-app-id (entry values)
  (destructuring-bind (app-id) values
    (set-object (getf entry :id) :app-id app-id)
    (toplevel-metadata (getf entry :id))))

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
  (forget-object (getf entry :id)))

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
     (7 . handle-surface-buffer-transform) (8 . handle-surface-buffer-scale)
     (9 . handle-ignore) (10 . handle-ignore))
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
     (3 . handle-set-window-geometry) (4 . handle-ack-configure))
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
                :outputs (list (list :name "tomoe-0" :x 0 :y 0 :width 1280 :height 720)))
    (list :display display
          :event-loop (display-event-loop display)
          :fd (event-loop-fd (display-event-loop display))
          :socket socket-name)))

(defun %step (backend timeout)
  (when (sb-sys:wait-until-fd-usable (getf backend :fd) :input (/ timeout 1000))
    (event-loop-dispatch (getf backend :event-loop) 0))
  (display-flush-clients (getf backend :display))
  0)

(defun %event-count (backend)
  (declare (ignore backend))
  (length *events*))

(defun %event-barrier (backend)
  (declare (ignore backend))
  (let ((position (position-if
                   (lambda (event)
                     (member (getf event :type) '(:map :metadata :buffer :geometry :layer :unmap)))
                   *events*)))
    (if position (- (length *events*) position) 0)))

(defun %event (backend)
  (declare (ignore backend))
  (let ((event (car (last *events*))))
    (setf *events* (butlast *events*))
    (and event (with-standard-io-syntax
                 (let ((*print-circle* nil)) (prin1-to-string event))))))

(defun %place (backend id x y width height visible)
  (declare (ignore backend x y visible))
  (let ((toplevel (object id)))
    (when (and toplevel (equal (getf toplevel :interface) "xdg_toplevel"))
      (let ((same-sent-size
              (and (getf toplevel :configure-sent)
                   (eql (getf toplevel :last-configure-width) width)
                   (eql (getf toplevel :last-configure-height) height))))
        (set-object id :width width :height height)
        (setf toplevel (object id))
        (when (and (getf toplevel :buffered) (not same-sent-size))
          (send-configure id))))))

(defun %set-view (backend x y zoom)
  (declare (ignore backend x y zoom))
  nil)
(defun %hit-test (backend screen-x screen-y)
  (declare (ignore backend screen-x screen-y))
  nil)

(defun %display-name (backend) (declare (ignore backend)) nil)
(defun %focus (backend id) (declare (ignore backend)) (when (object id) id))
(defun %close (backend id) (declare (ignore backend))
  (let ((toplevel (object id)))
    (when (and toplevel (equal (getf toplevel :interface) "xdg_toplevel"))
      (send-event toplevel 1))))
(defun %bind (backend modifiers code owner command release source-id)
  (declare (ignore backend owner command release source-id))
  (if (and (integerp modifiers) (integerp code) (plusp code)) 1 0))
(defun %clear-bindings (backend) (declare (ignore backend)) nil)
(defun %binding-current (backend binding-id) (declare (ignore backend binding-id)) 0)

(defun %grab (backend id mode)
  (declare (ignore backend id))
  (if (zerop mode) 1 0))
(defun %grab-id (backend) (declare (ignore backend)) 0)
(defun %grab-mode (backend) (declare (ignore backend)) 0)

(defun %window-state (backend id fullscreen maximize)
  (declare (ignore backend id fullscreen maximize))
  nil)

(defun %layer (backend id layer exclusive-zone keyboard visible)
  (declare (ignore backend id layer exclusive-zone keyboard visible))
  nil)

(defun %keysym (name) (%keysym-from-name name 0))

(defun tomoe::configure-native-outputs (backend outputs)
  (declare (ignore backend outputs))
  (error "Output configuration requires the wlroots backend; the Lisp backend has no hardware output support yet."))

(defun tomoe::configure-native-presentation
    (backend outputs context overrides restack outputs-changed bindings-changed grab &optional keyboard-changed)
  "Replay one accepted scene transaction through the pure backend stubs.
Output changes remain unsupported, just as they were before presentation
transactions also carried ordinary scene and input state."
  (declare (ignore overrides restack))
  (when outputs-changed
    (tomoe::configure-native-outputs backend outputs))
  (when (and keyboard-changed
             (not (equal (getf context :keyboard) (tomoe::default-keyboard-config))))
    (error "Keyboard configuration requires the wlroots backend; the Lisp backend has no keyboard devices yet."))
  (dolist (window (getf context :layout))
    (%window-state backend (getf window :id)
                   (if (getf window :fullscreen) 1 0)
                   (if (getf window :maximize) 1 0)))
  (when bindings-changed
    (%clear-bindings backend)
    (dolist (binding (getf context :bindings))
      (unless (= 1 (%bind backend (getf binding :modifiers) (getf binding :code)
                          (getf binding :owner) (getf binding :command)
                          (getf binding :release) (getf binding :source-id)))
        (error "Pure backend binding allocation failed."))))
  (if grab
      (%grab backend (first grab) (ecase (second grab) (:move 1) (:resize 2)))
      (%grab backend 0 0))
  (let ((view (getf context :view)))
    (%set-view backend (getf view :x) (getf view :y) (getf view :zoom)))
  (dolist (window (getf context :layout))
    (%place backend (getf window :id) (getf window :x) (getf window :y)
            (getf window :width) (getf window :height)
            (if (getf window :visible) 1 0)))
  (%focus backend (or (getf context :focus) 0)))

(defun tomoe::preview-native-outputs (backend outputs)
  (when outputs (tomoe::configure-native-outputs backend outputs))
  (list :outputs (list (list :name "tomoe-0" :x 0 :y 0 :width 1280 :height 720))
        :connectors (list (list :name "tomoe-0" :enabled t
                                :adaptive-sync-supported nil :adaptive-sync nil))))

(defun tomoe::%outputs-revision (backend) (declare (ignore backend)) 0)
(defun tomoe::pending-native-outputs-p (backend) (declare (ignore backend)) nil)

(defun tomoe::full-workareas (outputs)
  (loop for output in outputs collect
    (list :name (getf output :name) :x (getf output :x) :y (getf output :y)
          :width (getf output :width) :height (getf output :height))))

(defun tomoe::preview-native-layers (backend outputs layers overrides)
  (declare (ignore backend overrides))
  (values layers (tomoe::full-workareas outputs)))

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
