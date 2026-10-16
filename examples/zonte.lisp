(in-package #:tomoe-user)

(defparameter +zonte-wallpaper+ '(:shader "shaders/zonte.glsl" :fps 60))
(defparameter +zonte-glass+ '(:shader "shaders/glass.glsl" :fps 0))
(defparameter +zonte-inset+ 9 "Shadow room around the glass; INSET in glass.glsl.")
(defparameter +zonte-height+ 58)
(defparameter +zonte-gap+ 4)
(defparameter +zonte-width+ 960)
(defparameter +zonte-ink+ "#1d1830e6")
(defparameter +zonte-soft+ "#1d18308c")
(defparameter +zonte-faint+ "#1d183038")

(defun zonte--text (text &key (color +zonte-ink+) (size 11))
  (ui :text :text text :size size :color color))

(defun zonte--section (width children)
  (ui :row :width width :grow (if width 0 1) :align :center :justify :center :gap 10 :children children))

(defun zonte--rule ()
  (ui :separator :orientation :vertical :thickness 1 :height 18 :color +zonte-faint+))

(defun zonte--title (window)
  (let ((title (or (getf window :title) "")))
    (cond ((null window) "")
          ((equal title "") (or (getf window :app-id) ""))
          ((> (length title) 56) (concatenate 'string (subseq title 0 55) "…"))
          (t title))))

(defun zonte--beads (snapshot)
  (let ((state (state-value snapshot :wm-state)))
    (ui :row :gap 9 :align :center
        :children (loop for workspace in (getf state :workspaces)
                        for active = (eql (getf workspace :id) (getf state :active))
                        for occupied = (plusp (getf workspace :windows 0))
                        for size = (cond (active 10) (occupied 6) (t 4))
                        collect (ui :rect :width size :height size :radius (/ size 2)
                                    :background (cond (active +zonte-ink+)
                                                      (occupied +zonte-soft+)
                                                      (t +zonte-faint+)))))))

(define-extension "zonte" (:reads (:data :windows :focus) :state nil) (snapshot state event)
  (declare (ignore event))
  (let ((focused (find (context snapshot :focus) (context snapshot :windows)
                       :key (lambda (window) (getf window :id)))))
    (values
     state
     (list
      (interval :clock 1000)
      (shell-surface :wallpaper (ui :stack)
                     :anchors '(:top :right :bottom :left) :layer :background
                     :background +zonte-wallpaper+)
      (shell-surface :strip
                     (ui :row :grow 1 :align :center
                         :padding (list (+ +zonte-inset+ 8) +zonte-inset+ (+ +zonte-inset+ 8) +zonte-inset+)
                         :children (list (zonte--section 170 (list (zonte--text (clock-text "%H:%M") :size 22)
                                                                   (zonte--text (string-downcase (clock-text "%a %d"))
                                                                                :color +zonte-soft+)))
                                         (zonte--rule)
                                         (zonte--section nil (list (zonte--text (zonte--title focused))))
                                         (zonte--rule)
                                         (zonte--section 190 (list (zonte--beads snapshot)))))
                     :anchors '(:top) :width +zonte-width+ :height +zonte-height+
                     :margin (list +zonte-gap+ 0 0 0) :layer :overlay :background +zonte-glass+))
     nil)))
