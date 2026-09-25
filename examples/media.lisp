(in-package #:tomoe-user)

(defun media--nonempty (value)
  (and (stringp value) (plusp (length value)) value))

(defun media--truncate (value &optional (limit 30))
  (if (<= (length value) limit)
      value
      (if (<= limit 3)
          (subseq value 0 limit)
          (concatenate 'string (subseq value 0 (- limit 1)) "…"))))

(defun media--indicator (status)
  (cond ((string-equal status "Playing") "[>]")
        ((string-equal status "Paused") "[||]")
        (t "[.]")))

(defun media--metadata (service)
  (let* ((player (media--nonempty (getf service :player-name)))
         (title (media--nonempty (getf service :title)))
         (artist (media--nonempty (getf service :artist)))
         (metadata (cond ((and artist title) (format nil "~A – ~A" artist title))
                         (title title)
                         (t player))))
    (media--truncate metadata)))

(defun media--label (service)
  (format nil "~A ~A" (media--indicator (getf service :status))
          (media--metadata service)))

(define-extension "media-label" (:reads (:services :outputs) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((service (service-state snapshot :mpris))
         (outputs (context snapshot :outputs))
         (player (media--nonempty (getf service :player-name)))
         (status (media--nonempty (getf service :status))))
    (values nil
            (when (and (listp outputs) outputs (getf service :available) player status)
              (list
                 (shell-surface :media
                   (ui :row :padding '(6 10 6 10) :gap 8 :radius 6
                       :background "#1e1e2e"
                     :children
                     (list (ui :text :text (media--label service)
                                :size 12 :color "#cdd6f4")))
                 :width 240 :height 28 :anchors '(:top :right) :margin 8 :layer :top
                 :exclusive-zone 0 :background "#00000000")))
            nil)))
