(in-package #:tomoe)

(sb-alien:define-alien-routine ("tomoe_backlight_set" %backlight-set) sb-alien:int
  (name sb-alien:c-string) (value sb-alien:unsigned))

(defun %backlight-value (path)
  (with-open-file (in path) (parse-integer (read-line in))))

(defun adjust-backlight (percent)
  "Step the first backlight, in name order, by PERCENT of its range through logind."
  (let* ((root (or (sb-ext:posix-getenv "TOMOE_BACKLIGHT_ROOT") "/sys/class/backlight"))
         (device (first (sort (mapcar (lambda (path) (car (last (pathname-directory path))))
                                      (directory (format nil "~A/*/" root) :resolve-symlinks nil))
                              #'string<))))
    (unless device (error "No backlight under ~A." root))
    (let* ((maximum (%backlight-value (format nil "~A/~A/max_brightness" root device)))
           (value (max 0 (min maximum (+ (%backlight-value (format nil "~A/~A/brightness" root device))
                                         (round (* maximum percent) 100)))))
           (status (%backlight-set device value)))
      (unless (zerop status)
        (error "logind did not set ~A to ~D: errno ~D." device value (- status))))))
