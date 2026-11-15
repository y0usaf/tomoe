(in-package #:tomoe-user)

;; A bar and a panel whose content changes every 33 ms and then holds still. A
;; shell surface that keeps what it drew repaints only the pixels that changed,
;; so neither is uploaded again for one digit; the settled frame must still
;; match what the same content draws in one pass.
(define-extension "ui-damage" (:reads () :state '(:tick 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let* ((tick (if (and (eq (getf event :type) :timer)
                        (eq (getf event :name) :ui-damage))
                   (min 50 (1+ (getf state :tick)))
                   (getf state :tick)))
         (value (float (/ tick 50) 1.0d0))
         (counter (format nil "~2,'0D" tick)))
    (values (list :tick tick)
            (list (interval :ui-damage 33)
                  (shell-surface :ui-damage-bar
                                 (ui :row :grow 1 :gap 8 :padding '(4 8 4 8)
                                     :background "#1e1e2e"
                                     :children (list (ui :text :text counter
                                                         :size 13 :color "#cdd6f4")
                                                     (ui :progress :value value :width 120
                                                         :height 6 :color "#89b4fa"
                                                         :track "#313244")
                                                     (ui :spacer)
                                                     (ui :text :text "settled" :size 13
                                                         :color "#a6e3a1")))
                                 :anchors '(:bottom :left :right) :height 24
                                 :layer :overlay :background "#1e1e2e")
                  (shell-surface :ui-damage-panel
                                 (ui :row :grow 1 :gap 10 :padding 12
                                     :background "#181825"
                                     :children (list (ui :text :text counter
                                                         :size 15 :color "#cdd6f4")
                                                     (ui :progress :value value :width 240
                                                         :height 8 :color "#89b4fa"
                                                         :track "#313244")
                                                     (ui :spacer)
                                                     (ui :text :text "静的 عربى Ωμέγα"
                                                         :size 15 :color "#a6e3a1")))
                                 :anchors '(:top :left) :width 1272 :height 648
                                 :layer :top :background "#181825"))
            nil)))
