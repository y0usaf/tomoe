;;; M1 runner: the new backend alone, no policy, no wlroots.
(defpackage #:tomoe (:use #:cl))
(load "backend/core-protocols.lisp")
(load "backend/xdg-shell.lisp")
(load "backend/wl.lisp")
(load "backend/server.lisp")

(in-package #:tomoe)
(defun show-event (text)
  (format t "~A~%" text) (finish-output)
  (when (search ":TYPE :MAP" text)
    (let ((buffer tomoe::*last-buffer*))
      (unless buffer (return-from show-event nil))
      (multiple-value-bind (colours non-zero total) (tomoe::buffer-statistics buffer)
        (format t "  buffer ~D: distinct colours ~D, non-zero ~D/~D bytes~%"
                buffer colours non-zero total))
      (format t "  ppm ~A~%" (tomoe::write-ppm buffer "/tmp/tomoe-m1.ppm"))
      (finish-output))))

(let ((backend (tomoe::open-backend (or (sb-posix:getenv "M1_SOCKET") "wayland-m1"))))
  (format t "ready~%") (finish-output)
  (let ((deadline (+ (get-internal-real-time) (* 25 internal-time-units-per-second))))
    (loop while (< (get-internal-real-time) deadline) do
      (tomoe::%step backend 8)
      (loop for text = (tomoe::%event backend) while text do (show-event text))))
  (tomoe::%destroy backend)
  (format t "done~%"))
