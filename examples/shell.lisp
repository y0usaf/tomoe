(in-package #:tomoe-user)

(define-extension "shell-bar" (:reads (:ui) :state 0) (snapshot count event)
  (declare (ignore snapshot))
  (when (and (eq (getf event :type) :ui)
             (equal (getf event :command) "increment"))
    (incf count))
  (values count
    (list
      (shell-surface :bar
        (ui :row :padding '(0 12 0 12) :gap 16 :children
          (list (ui :text :text "Tomoe" :color "#89b4fa")
                (ui :spacer)
                (ui :button :key :counter :on-click :increment
                    :padding '(4 12 4 12) :radius 4 :background "#313244"
                    :children (list (ui :text :text (format nil "Clicks: ~D" count))))))
        :height 36 :exclusive-zone 36)) nil))
