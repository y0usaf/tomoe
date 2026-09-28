(in-package #:tomoe-user)

(defparameter +shader-wallpapers+
  '(("shaders/flow.glsl" 15) ("shaders/stars.glsl" 20) ("shaders/grain.glsl" 12) ("shaders/cubes.glsl" 30)))

(define-extension "shader-wallpaper" (:reads (:key) :state 0) (snapshot index event)
  (declare (ignore snapshot))
  (when (and (eq (getf event :type) :key) (equal (getf event :owner) "shader-wallpaper"))
    (setf index (mod (1+ index) (length +shader-wallpapers+))))
  (destructuring-bind (path fps) (nth index +shader-wallpapers+)
    (values index
            (list (bind-key '(:mod :shift) "b" :next :description "Next shader wallpaper")
                  (shell-surface :shader-wallpaper (ui :stack)
                                 :anchors '(:top :right :bottom :left) :layer :background
                                 :background (list :shader path :fps fps)))
            nil)))
