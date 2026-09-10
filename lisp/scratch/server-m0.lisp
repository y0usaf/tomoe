(require :sb-posix)
;;; M0 spike: SBCL owns a Wayland server socket and event loop with no wlroots.
;;; Only libwayland-server is used, through sb-alien. Disposable.
(sb-alien:load-shared-object "libwayland-server.so.0")

(sb-alien:define-alien-routine ("wl_display_create" %display-create) (* t))
(sb-alien:define-alien-routine ("wl_display_add_socket" %add-socket) sb-alien:int
  (display (* t)) (name sb-alien:c-string))
(sb-alien:define-alien-routine ("wl_display_get_event_loop" %event-loop) (* t)
  (display (* t)))
(sb-alien:define-alien-routine ("wl_event_loop_get_fd" %loop-fd) sb-alien:int
  (loop (* t)))
(sb-alien:define-alien-routine ("wl_event_loop_dispatch" %dispatch) sb-alien:int
  (loop (* t)) (timeout sb-alien:int))
(sb-alien:define-alien-routine ("wl_display_flush_clients" %flush) sb-alien:void
  (display (* t)))
(sb-alien:define-alien-routine ("wl_display_destroy" %destroy) sb-alien:void
  (display (* t)))

(defun main ()
  (let* ((display (%display-create))
         (name (or (sb-ext:posix-getenv "SMOKE_SOCKET") "wayland-m0"))
         (status (progn
                   (ignore-errors
                    (sb-posix:unlink (format nil "~A/~A" (sb-ext:posix-getenv "XDG_RUNTIME_DIR") name)))
                   (%add-socket display name))))
    (unless (zerop status) (error "wl_display_add_socket failed: ~D" status))
    (let* ((evloop (%event-loop display))
          (fd (%loop-fd evloop))
          (wakeups 0)
          (deadline (+ (get-internal-real-time) (* 20 internal-time-units-per-second))))
      (format t "ready socket=~A fd=~D~%" name fd) (finish-output)
      (loop while (and (< wakeups 6) (< (get-internal-real-time) deadline)) do
        (when (sb-sys:wait-until-fd-usable fd :input 0.25)
          (incf wakeups)
          (let ((result (%dispatch evloop 0)))
            (%flush display)
            (format t "wakeup ~D dispatch=~D~%" wakeups result) (finish-output))))
      (%destroy display)
      (format t "destroyed wakeups=~D~%" wakeups) (finish-output)
      wakeups)))

(sb-ext:exit :code (if (plusp (main)) 0 1))
