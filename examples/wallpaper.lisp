(in-package #:tomoe-user)

(defparameter +wallpaper-directory+ "/usr/share/backgrounds"
  "Directory whose PNG and JPEG files Mod+w shuffles through.")

(defun wallpaper--quote (text)
  (with-output-to-string (out)
    (write-char #\' out)
    (loop for c across text do (if (char= c #\') (write-string "'\\''" out) (write-char c out)))
    (write-char #\' out)))

(define-extension "wallpaper" (:reads (:key) :state '(:path nil :run 0)) (snapshot state event)
  (declare (ignore snapshot))
  (let ((path (getf state :path)) (run (getf state :run)))
    (cond ((and (eq (getf event :type) :key) (equal (getf event :owner) "wallpaper")
                (equal (getf event :command) "shuffle"))
           (incf run))
          ((and (eq (getf event :type) :exec) (eql (getf event :code) 0))
           (let ((line (string-trim '(#\Newline) (getf event :stdout))))
             (when (plusp (length line)) (setf path line)))))
    (values (list :path path :run run)
            (append
             (list (bind-key '(:mod) "w" :shuffle :description "Next wallpaper")
                   (exec-async (intern (format nil "SHUFFLE-~D" run) :keyword)
                               (format nil "find ~A -type f \\( -iname '*.png' -o -iname '*.jpg' -o -iname '*.jpeg' \\) | shuf -n 1"
                                       (wallpaper--quote +wallpaper-directory+))))
             (when path
               (list (shell-surface :wallpaper (ui :stack)
                                    :anchors '(:top :right :bottom :left) :layer :background
                                    :background (list :image path :fit :cover)))))
            nil)))
