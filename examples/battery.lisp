(in-package #:tomoe-user)

(defun battery--percent (service)
  (let ((percent (getf service :percent)))
    (if (integerp percent)
        (max 0 (min 100 percent))
        0)))

(defun battery--icon (service percent)
  (if (getf service :charging)
      "battery-charging"
      (cond ((> percent 80) "battery-full")
            ((> percent 40) "battery-medium")
            ((> percent 15) "battery-low")
            (t "battery-warning"))))

(define-extension "battery-indicator" (:reads (:services :outputs) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((service (service-state snapshot :battery))
         (outputs (context snapshot :outputs))
         (available (and (listp service) (getf service :available)))
         (percent (battery--percent service)))
    (values nil
            (when (and available (listp outputs) outputs)
              (list
               (shell-surface :battery
                 (ui :row :padding '(5 10 5 10) :gap 4 :radius 6
                     :background "#1e1e2e"
                     :children
                     (list (ui :icon :name (battery--icon service percent)
                                :size 18 :color "#cdd6f4")
                           (ui :text :text (format nil "~D%" percent)
                                :size 12 :color "#cdd6f4")))
                 :width 112 :height 28 :anchors '(:top :right) :margin 8
                 :layer :top :exclusive-zone 0 :background "#00000000")))
            nil)))
