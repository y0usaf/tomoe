;;; libwayland-server boundary. Nothing above this file calls C.
;;;
;;; struct wl_message { const char *name; const char *signature; const struct wl_interface **types; }
;;; struct wl_interface { const char *name; int version; int method_count;
;;;                       const struct wl_message *methods; int event_count;
;;;                       const struct wl_message *events; }
;;; Layouts are written as byte offsets so no alien struct type is declared.
(require :sb-posix) ; only for mmap on client buffers
(in-package #:tomoe)

(sb-alien:load-shared-object "libwayland-server.so.0")
(sb-alien:load-shared-object "libxkbcommon.so.0")

(defconstant +message-name+ 0)
(defconstant +message-signature+ 8)
(defconstant +message-types+ 16)
(defconstant +message-size+ 24)
(defconstant +interface-name+ 0)
(defconstant +interface-version+ 8)
(defconstant +interface-method-count+ 12)
(defconstant +interface-methods+ 16)
(defconstant +interface-event-count+ 24)
(defconstant +interface-events+ 32)
(defconstant +interface-size+ 40)

(sb-alien:define-alien-routine ("wl_display_create" display-create) (* t))
(sb-alien:define-alien-routine ("wl_display_add_socket" display-add-socket) sb-alien:int
  (display (* t)) (name sb-alien:c-string))
(sb-alien:define-alien-routine ("wl_display_get_event_loop" display-event-loop) (* t)
  (display (* t)))
(sb-alien:define-alien-routine ("wl_event_loop_get_fd" event-loop-fd) sb-alien:int
  (event-loop (* t)))
(sb-alien:define-alien-routine ("wl_event_loop_dispatch" event-loop-dispatch) sb-alien:int
  (event-loop (* t)) (timeout sb-alien:int))
(sb-alien:define-alien-routine ("wl_display_flush_clients" display-flush-clients) sb-alien:void
  (display (* t)))
(sb-alien:define-alien-routine ("wl_display_destroy" display-destroy) sb-alien:void
  (display (* t)))
(sb-alien:define-alien-routine ("wl_global_create" global-create) (* t)
  (display (* t)) (interface (* t)) (version sb-alien:int) (data (* t)) (bind (* t)))
(sb-alien:define-alien-routine ("wl_resource_create" resource-create) (* t)
  (client (* t)) (interface (* t)) (version sb-alien:int) (id sb-alien:unsigned-int))
(sb-alien:define-alien-routine ("wl_resource_set_dispatcher" resource-set-dispatcher) sb-alien:void
  (resource (* t)) (dispatcher (* t)) (implementation (* t)) (data (* t)) (destroy (* t)))
(sb-alien:define-alien-routine ("wl_resource_post_event_array" resource-post-event-array) sb-alien:void
  (resource (* t)) (opcode sb-alien:unsigned-int) (args (* t)))
(sb-alien:define-alien-routine ("wl_resource_get_user_data" resource-user-data) (* t)
  (resource (* t)))
(sb-alien:define-alien-routine ("wl_resource_get_version" resource-version) sb-alien:int
  (resource (* t)))
(sb-alien:define-alien-routine ("wl_client_flush" client-flush) sb-alien:void (client (* t)))
(sb-alien:define-alien-routine ("xkb_keysym_from_name" %keysym-from-name) sb-alien:unsigned-int
  (name sb-alien:c-string) (flags sb-alien:int))

(defvar *pinned* nil "Foreign allocations that must outlive the call that made them.")
(defvar *interfaces* (make-hash-table :test #'equal) "interface name -> SAP")
(defvar *tables* (make-hash-table :test #'equal) "interface name -> (version requests events)")

(defun sap-of (thing)
  "Address of a SAP, an alien pointer, or an integer address."
  (cond ((integerp thing) thing)
        ((typep thing 'sb-sys:system-area-pointer) (sb-sys:sap-int thing))
        (t (sb-sys:sap-int (sb-alien:alien-sap thing)))))

(defun alien-at (thing)
  "A (* t) alien for a SAP, an alien, or an integer address."
  (if (typep thing 'sb-sys:system-area-pointer)
      (sb-alien:sap-alien thing (* t))
      (sb-alien:sap-alien (sb-sys:int-sap (sap-of thing)) (* t))))
(defun callable-pointer (name)
  "Address of a define-alien-callable function, as a C function pointer."
  (alien-at (sap-of (sb-alien:alien-callable-function name))))
(defun zero-sap () (sb-sys:int-sap 0))
(defun pointer-at (thing offset)
  (sb-sys:sap-ref-sap (sb-sys:int-sap (sap-of thing)) offset))
(defun uint-at (thing offset)
  (sb-sys:sap-ref-32 (sb-sys:int-sap (sap-of thing)) offset))
(defun write-pointer (sap offset value)
  (setf (sb-sys:sap-ref-sap sap offset)
        (cond ((integerp value) (sb-sys:int-sap value))
              ((typep value 'sb-sys:system-area-pointer) value)
              (t (sb-alien:alien-sap value)))))
(defun write-uint (sap offset value) (setf (sb-sys:sap-ref-32 sap offset) value))

(defun allocate (bytes)
  (let ((alien (sb-alien:make-alien sb-alien:unsigned-char bytes)))
    (push alien *pinned*)
    alien))

(defun cstring (string)
  (let* ((alien (allocate (1+ (length string))))
         (sap (sb-alien:alien-sap alien)))
    (loop for index below (length string)
          do (setf (sb-sys:sap-ref-8 sap index) (char-code (char string index))))
    (setf (sb-sys:sap-ref-8 sap (length string)) 0)
    alien))

(defun utf8-string (thing)
  (let* ((sap (sb-sys:int-sap (sap-of thing)))
         (octets (make-array 64 :element-type '(unsigned-byte 8) :adjustable t :fill-pointer 0)))
    (loop for offset from 0
          for byte = (sb-sys:sap-ref-8 sap offset)
          until (zerop byte)
          do (vector-push-extend byte octets))
    (sb-ext:octets-to-string octets :external-format :utf-8)))

(defun message-signature-of (message)
  (utf8-string (pointer-at message +message-signature+)))
(defun message-name-of (message)
  (utf8-string (pointer-at message +message-name+)))

(defun register-tables (data)
  "DATA is a list of (name version requests events) from a generated table file."
  (dolist (entry data)
    (destructuring-bind (name version requests events) entry
      (setf (gethash name *tables*) (list version requests events)))))

(defun interface-address (name)
  (or (gethash name *interfaces*)
      (error "Unknown interface ~A; register its table first." name)))

(defun build-interface (name table)
  "Allocate one struct wl_interface and its message arrays from the table."
  (destructuring-bind (version requests events) table
    (let* ((size (max (length requests) (length events) 1))
           (messages (alien-at (sap-of (allocate (* size +message-size+)))))
           (methods (sap-of (allocate (* (max 1 (length requests)) +message-size+))))
           (event-array (sap-of (allocate (* (max 1 (length events)) +message-size+))))
           (sap (sap-of (allocate +interface-size+)))
           (interface (alien-at sap)))
      (declare (ignore messages interface))
      (labels ((fill-messages (address descriptions count)
                 (loop for (opcode message-name signature types) in descriptions
                       for slot = (+ address (* opcode +message-size+)) do
                   (write-pointer (sb-sys:int-sap slot) +message-name+
                                  (sap-of (cstring message-name)))
                   (write-pointer (sb-sys:int-sap slot) +message-signature+
                                  (sap-of (cstring signature)))
                   (write-pointer (sb-sys:int-sap slot) +message-types+
                                  (types-array types)))))
        (fill-messages methods requests (length requests))
        (fill-messages event-array events (length events)))
      (write-pointer (sb-sys:int-sap sap) +interface-name+ (sap-of (cstring name)))
      (write-uint (sb-sys:int-sap sap) +interface-version+ version)
      (write-uint (sb-sys:int-sap sap) +interface-method-count+ (length requests))
      (write-pointer (sb-sys:int-sap sap) +interface-methods+ methods)
      (write-uint (sb-sys:int-sap sap) +interface-event-count+ (length events))
      (write-pointer (sb-sys:int-sap sap) +interface-events+ event-array)
      (setf (gethash name *interfaces*) sap))))

(defun types-array (names)
  "One pointer per argument; non-object arguments hold NULL."
  (let* ((count (max 1 (length names)))
         (sap (sap-of (allocate (* 8 count)))))
    (loop for name in names for index from 0
          for slot = (+ sap (* 8 index)) do
      (write-pointer (sb-sys:int-sap slot) 0
                     (if (plusp (length name)) (interface-address name) 0)))
    sap))

(defun build-interfaces ()
  "Two passes: every interface exists before any types array points at one."
  (maphash (lambda (name table)
             (declare (ignore table))
             (setf (gethash name *interfaces*) 0))
           *tables*)
  (maphash (lambda (name table) (build-interface name table)) *tables*))

(defun signature-letters (signature)
  "Drop wayland's version digits and nullability markers: \"?oii\" -> (o i i)."
  (loop for character across signature
        when (find character "iufsonah") collect character))

(defun message-signature (kind name opcode)
  (let* ((table (gethash name *tables*))
         (messages (ecase kind (:request (second table)) (:event (third table))))
         (entry (find opcode messages :key #'first)))
    (unless entry (error "~A has no ~A opcode ~D." name kind opcode))
    (third entry)))

(defun marshal-arguments (signature values)
  "Decoded Lisp values into a union wl_argument array. Pins string storage."
  (let* ((letters (signature-letters signature))
         (array (alien-at (sap-of (allocate (* 8 (max 1 (length letters)))))))
         (sap (sb-alien:alien-sap array)))
    (loop for character in letters
          for value in values
          for index from 0
          for slot = (* 8 index) do
      (ecase character
        ((#\i #\u #\f #\h #\n) (write-uint sap slot (logand value #xffffffff)))
        (#\s (write-pointer sap slot (sap-of (cstring value))))
        ((#\o #\a) (write-pointer sap slot value))))
    array))

(defun send-event (object opcode &rest values)
  "OBJECT is a registry entry. VALUES are decoded; encoding uses the table."
  (handler-case (%send-event object opcode values)
    (serious-condition (condition)
      (error "~A event ~D with ~S: ~A" (getf object :interface) opcode values condition))))

(defun %send-event (object opcode values)
  (let* ((signature (message-signature :event (getf object :interface) opcode))
         (letters (signature-letters signature)))
    (unless (= (length letters) (length values))
      (error "~A event ~D wants ~D arguments, got ~D."
             (getf object :interface) opcode (length letters) (length values)))
    (resource-post-event-array (getf object :resource) opcode
                               (sb-alien:alien-sap (marshal-arguments signature values)))))

(defun decode-arguments (signature args)
  "union wl_argument array into Lisp values, driven by the signature."
  (let ((args-sap (sb-sys:int-sap (sap-of args))))
    (declare (type sb-sys:system-area-pointer args-sap))
    (loop for character in (signature-letters signature)
        for index from 0
        for slot = (* 8 index)
        collect (ecase character
                  ((#\i #\f) (sb-sys:signed-sap-ref-32 args-sap slot))
                  ((#\u #\h #\n) (sb-sys:sap-ref-32 args-sap slot))
                    (#\s (utf8-string (pointer-at args-sap slot)))
                    ((#\o #\a) (sap-of (pointer-at args-sap slot)))))))
