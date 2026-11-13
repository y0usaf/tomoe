(in-package #:tomoe-user)

(defparameter +shader-wallpapers+
  '(("shaders/xmb.glsl" 30) ("shaders/aurora.glsl" 30) ("shaders/stars.glsl" 24) ("shaders/towers.glsl" 24) ("shaders/cubes.glsl" 30) ("shaders/okazz-spheres.glsl" 30) ("shaders/okazz-glyphs.glsl" 30) ("shaders/hex-tunnel.glsl" 30) ("shaders/liquify.glsl" 30)))

(define-extension "shader-wallpaper" (:reads (:command) :state 0) (snapshot index event)
  (declare (ignore snapshot))
  (when (and (eq (getf event :type) :command) (equal (getf event :name) "next-shader"))
    (setf index (mod (1+ index) (length +shader-wallpapers+))))
  (destructuring-bind (path fps) (nth index +shader-wallpapers+)
    (values index
            (list (define-command "next-shader" :description "Show the next shader wallpaper.")
                  (bind-key '(:mod :shift) "b" "next-shader" :description "Next shader wallpaper")
                  (shell-surface :shader-wallpaper (ui :stack)
                                 :anchors '(:top :right :bottom :left) :layer :background
                                 :background (list :shader path :fps fps)))
            nil)))
