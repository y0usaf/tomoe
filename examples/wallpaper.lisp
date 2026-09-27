(in-package #:tomoe-user)

(defparameter +wallpaper-directory+ "/usr/share/backgrounds"
  "Directory whose PNG and JPEG files, at any depth, Mod+w shuffles through.")

(defun wallpaper--pick (current)
  "A random wallpaper other than CURRENT, or CURRENT when there is no other."
  (let ((files (remove current
                       (loop for path in (directory (format nil "~A/**/*.*" +wallpaper-directory+))
                             when (member (pathname-type path) '("png" "jpg" "jpeg") :test #'string-equal)
                               collect (sb-ext:native-namestring path))
                       :test #'equal)))
    (if files (nth (random (length files) (make-random-state t)) files) current)))

(define-extension "wallpaper" (:reads (:key) :state nil) (snapshot path event)
  (declare (ignore snapshot))
  (let ((path (if (or (null path)
                      (and (eq (getf event :type) :key) (equal (getf event :owner) "wallpaper")
                           (equal (getf event :command) "shuffle")))
                  (wallpaper--pick path)
                  path)))
    (values path
            (cons (bind-key '(:mod) "w" :shuffle :description "Next wallpaper")
                  (when path
                    (list (shell-surface :wallpaper (ui :stack)
                                         :anchors '(:top :right :bottom :left) :layer :background
                                         :background (list :image path :fit :cover)))))
            nil)))
