(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_network_abi" %network-abi) sb-alien:int)
(sb-alien:define-alien-routine ("tomoe_network_open" %network-open) (* t)
  (sysfs-root sb-alien:c-string) (error (* sb-alien:int)))
(sb-alien:define-alien-routine ("tomoe_network_poll" %network-poll) sb-alien:int
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_network_snapshot" %network-snapshot) sb-alien:c-string
  (producer (* t)))
(sb-alien:define-alien-routine ("tomoe_network_timeout" %network-timeout) sb-alien:int
  (producer (* t)) (maximum sb-alien:int))
(sb-alien:define-alien-routine ("tomoe_network_close" %network-close) sb-alien:void
  (producer (* t)))

(defvar *network-support-loaded* nil)

(defun decode-network-snapshot (text)
  (let* ((snapshot (read-data (or text (error "Cannot allocate network snapshot."))))
         (ssid (getf snapshot :ssid)))
    (when ssid
      (unless (and (listp ssid) (<= (length ssid) 4096))
        (error "Malformed network SSID encoding."))
      (setf (getf snapshot :ssid)
            (map 'string
                 (lambda (code)
                   (unless (and (integerp code) (<= 0 code #x10ffff)
                                (not (<= #xd800 code #xdfff)))
                     (error "Invalid network SSID Unicode scalar."))
                   (code-char code))
                 ssid)))
    snapshot))

(defun stop-network (runtime)
  (let ((native (runtime-network-producer runtime)))
    (when native
      (setf (runtime-network-producer runtime) nil)
      (%network-close native))))

(defun start-network (runtime)
  "Start the session producer. Missing NetworkManager or sysfs is normal."
  (handler-case
      (progn
        (unless *network-support-loaded*
          (sb-alien:load-shared-object
           (or (sb-ext:posix-getenv "TOMOE_NETWORK_LIB") "build/libtomoe-network.so"))
          (unless (= 1 (%network-abi)) (error "Incompatible network helper ABI."))
          (setf *network-support-loaded* t))
        (sb-alien:with-alien ((error-code sb-alien:int))
          (let ((native (%network-open
                         (or (sb-ext:posix-getenv "TOMOE_NETWORK_SYSFS_ROOT")
                             "/sys/class/net")
                         (sb-alien:addr error-code))))
            (unless (sb-alien:null-alien native)
              (setf (runtime-network-producer runtime) native
                    (getf (runtime-services runtime) :network)
                    (decode-network-snapshot (%network-snapshot native)))))))
    (serious-condition (condition)
      (stop-network runtime)
      (format *error-output* "tomoe: network unavailable: ~A~%" condition))))

(defun network-wait-milliseconds (runtime maximum)
  (let ((native (runtime-network-producer runtime)))
    (if native (%network-timeout native maximum) maximum)))

(defun poll-network-snapshot (runtime)
  "Return changed facts and T, or NIL/NIL when the producer is unchanged."
  (handler-case
      (let* ((native (runtime-network-producer runtime))
             (status (%network-poll native)))
        (cond ((minusp status)
               (stop-network runtime)
               (values (default-network-state) t))
              ((plusp status)
               (values (decode-network-snapshot (%network-snapshot native)) t))))
    (serious-condition (condition)
      (stop-network runtime)
      (format *error-output* "tomoe: network producer stopped: ~A~%" condition)
      (values (default-network-state) t))))

(defun service-network (runtime)
  (reconcile-backend-observations runtime)
  (when (and (runtime-running runtime) (not *stop-requested*)
             (runtime-network-producer runtime))
    (multiple-value-bind (snapshot changed) (poll-network-snapshot runtime)
      (when changed
        (let ((services (copy-data (runtime-services runtime))))
          (setf (getf services :network) snapshot)
          (dispatch-event runtime (list :type :services :services services)))))))
