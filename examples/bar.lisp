(in-package #:tomoe-user)

(defun bar--truncate (text limit)
  (cond ((or (null text) (equal text "")) "")
        ((<= (length text) limit) text)
        (t (concatenate 'string (subseq text 0 (1- limit)) "…"))))

(defun bar--button (label command &key (px 10) (py 5) background)
  (ui :button :on-click command :padding (list py px py px)
      :background (or background (theme :surface0))
      :children (list (ui :text :text label :size (theme :font-size) :color (theme :text)))))

(defun bar--text (text)
  (ui :text :text text :size (theme :font-size) :color (theme :text)))

(defun bar--mpris (player method)
  (spawn (list "dbus-send" "--session" "--type=method_call"
               (format nil "--dest=org.mpris.MediaPlayer2.~A" player)
               "/org/mpris/MediaPlayer2" (format nil "org.mpris.MediaPlayer2.Player.~A" method))))

(defun bar--volume (stdout)
  "(PERCENT MUTED) from `wpctl get-volume` output."
  (let ((start (position-if #'digit-char-p stdout)))
    (list (if start (round (* 100 (let ((*read-default-float-format* 'double-float))
                                    (read-from-string stdout t nil :start start))))
              100)
          (and (search "MUTED" stdout) t))))

(defun bar--panel (name anchors width height children)
  (shell-surface name
                 (ui :row :grow 1 :padding (make-list 4 :initial-element (theme :panel-padding)) :background (theme :base)
                     :children (list (ui :column :grow 1 :gap 8 :children children)))
                 :anchors anchors :width width :height height :margin (list (theme :bar-height) 0 0 0)
                 :layer :overlay :exclusive-zone 0 :background (theme :base)))

(defparameter +bar-volume-command+ "wpctl get-volume @DEFAULT_AUDIO_SINK@")

(define-extension "bar" (:reads (:services :data :outputs :ui)
                         :state '(:media nil :volume nil :level (100 nil) :run 0 :action nil))
    (snapshot state event)
  (let* ((mpris (service-state snapshot :mpris))
         (player (getf mpris :player-name ""))
         (playing (equal (getf mpris :status) "Playing"))
         (media (getf state :media)) (volume (getf state :volume))
         (level (getf state :level)) (run (getf state :run)) (action (getf state :action))
         (command (and (eq (getf event :type) :ui) (getf event :command)))
         (commands nil))
    (flet ((volume-action (shell) (setf action shell run (1+ run))))
      (cond ((equal command "media") (setf media (not media)))
            ((equal command "media-close") (setf media nil))
            ((equal command "volume") (setf volume (not volume)))
            ((equal command "volume-close") (setf volume nil))
            ((equal command "previous") (push (bar--mpris player "Previous") commands))
            ((equal command "play-pause") (push (bar--mpris player "PlayPause") commands))
            ((equal command "next") (push (bar--mpris player "Next") commands))
            ((equal command "stop") (push (bar--mpris player "Stop") commands))
            ((equal command "volume-down") (volume-action "wpctl set-volume @DEFAULT_AUDIO_SINK@ 5%-"))
            ((equal command "volume-up") (volume-action "wpctl set-volume -l 1.5 @DEFAULT_AUDIO_SINK@ 5%+"))
            ((equal command "mute") (volume-action "wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle"))
            ((and (eq (getf event :type) :exec) (eql (getf event :code) 0))
             (setf level (bar--volume (getf event :stdout)) action nil))))
    (destructuring-bind (percent muted) level
      (let* ((title (if (equal (getf mpris :title "") "") player (getf mpris :title)))
             (right (list (if (equal player "")
                              (bar--text "")
                              (bar--button (format nil "~A  ~A" (if playing "󰎈" "󰏤") (bar--truncate title 22))
                                           :media :py 2 :background (theme (if media :surface1 :surface0))))
                          (bar--button (format nil "~A ~D%" (if muted "󰖁" "󰕾") percent) :volume
                                       :py 2 :background (theme (if volume :surface1 :surface0))))))
        (values (list :media media :volume volume :level level :run run :action action)
                (append
                 (list (interval :clock 1000)
                       (exec-async (intern (format nil "VOLUME-~D" run) :keyword)
                                   (if action (format nil "~A; ~A" action +bar-volume-command+)
                                       +bar-volume-command+))
                       (shell-surface :bar
                                      (bar-layout (list (workspaces-widget snapshot))
                                                  (list (bar--text (clock-text "%H:%M")))
                                                  right)
                                      :anchors '(:top :left :right) :height (theme :bar-height)
                                      :exclusive-zone (theme :bar-height) :background (theme :base)))
                 (when media
                   (list (bar--panel :media-panel '(:top) 340 110
                                     (if (equal player "")
                                         (list (ui :row :children (list (bar--text "No media player active")
                                                                        (ui :spacer)
                                                                        (bar--button "✕" :media-close :px 8 :py 4))))
                                         (list (ui :row :gap 8
                                                   :children (list (ui :column :gap 2
                                                                       :children (list (bar--text (bar--truncate title 32))
                                                                                       (bar--text (let ((artist (bar--truncate (getf mpris :artist) 28)))
                                                                                                    (if (equal artist "") player artist)))
                                                                                       (bar--text (bar--truncate (getf mpris :album) 28))))
                                                                   (ui :spacer)
                                                                   (bar--button "✕" :media-close :px 8 :py 4)))
                                               (ui :row :gap 6
                                                   :children (list (bar--button "󰒮" :previous :px 12)
                                                                   (bar--button (if playing "󰏤" "󰐊") :play-pause :px 12)
                                                                   (bar--button "󰒭" :next :px 12)
                                                                   (bar--button "󰓛" :stop :px 12))))))))
                 (when volume
                   (let ((filled (max 0 (min 22 (round (* (if muted 0 percent) 22) 100)))))
                     (list (bar--panel :volume-panel '(:top :right) 300 120
                                       (list (ui :row :gap 8
                                                 :children (list (bar--text "󰕾  Volume") (ui :spacer)
                                                                 (bar--text (if muted "muted" (format nil "~D%" percent)))))
                                             (bar--text (concatenate 'string
                                                                     (make-string filled :initial-element #\█)
                                                                     (make-string (- 22 filled) :initial-element #\░)))
                                             (ui :row :gap 6
                                                 :children (list (bar--button "−" :volume-down :px 12 :py 4)
                                                                 (bar--button (if muted "󰖁  Unmute" "󰕾  Mute") :mute
                                                                              :px 12 :py 4
                                                                              :background (theme (if muted :red :surface0)))
                                                                 (bar--button "+" :volume-up :px 12 :py 4)
                                                                 (ui :spacer)
                                                                 (bar--button "✕" :volume-close :px 8 :py 4)))))))))
                commands)))))
