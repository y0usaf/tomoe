(in-package #:tomoe)

(defparameter +dialog-background+ "#1c1c21")
(defparameter +dialog-text+ "#ffffff")
(defparameter +dialog-accent+ "#7aa1f7")
(defparameter +dialog-urgent+ "#bf2e38")
(defparameter +dialog-chip+ "#45454f")

(defun %dialog-box (padding gap children &key (align :center))
  (ui :column :padding 4 :background +dialog-accent+
      :children (list (ui :column :padding padding :gap gap :align align
                          :background +dialog-background+ :children children))))

(defun %dialog-surface (name tree &key backdrop dismiss)
  "A full-output overlay centering TREE; clicks outside run DISMISS."
  (shell-surface name
                 (ui :column :justify :center :align :center :on-click dismiss
                     :background (if backdrop "#00000066" "#00000000")
                     :children (list tree))
                 :anchors '(:top :right :bottom :left) :layer :overlay
                 :background "#00000000"))

(defun confirm-dialog (name text &key (confirm :confirm) (cancel :cancel))
  "Effects for a modal confirm dialog: Enter runs CONFIRM, any other key or a
click runs CANCEL."
  (list (%dialog-surface
         name
         (ui :column :padding 8 :background +dialog-urgent+
             :children
             (list (ui :column :padding 32 :gap 12 :align :center :background +dialog-background+
                       :on-click cancel
                       :children
                       (list (ui :text :text text :size 20 :color +dialog-text+)
                             (ui :row :children
                                 (list (ui :text :text "Press " :size 20 :color +dialog-text+)
                                       (ui :text :text " Enter " :size 20 :color +dialog-text+
                                           :background +dialog-chip+)
                                       (ui :text :text " to confirm." :size 20
                                           :color +dialog-text+)))))))
         :backdrop t :dismiss cancel)
        (keyboard-grab :otherwise cancel)
        (bind-key nil "Return" confirm)))

(defun %menu-command (select index)
  (intern (format nil "~A-~D" (symbol-name select) index) :keyword))

(defun menu-dialog (name items selected &key title (select :select) (cancel :cancel)
                                            (up :up) (down :down) (hover :hover))
  "Effects for a modal menu with SELECTED highlighted. Up/Down or k/j run UP and
DOWN, hovering a row runs HOVER-<index>, Enter runs SELECT, a row click runs
SELECT-<index>, Esc or a click outside runs CANCEL. MENU-CHOICE decodes them."
  (let ((rows (loop for item in items for index from 0
                    for chosen = (= index selected)
                    collect (ui :row :padding '(5 28 5 28)
                                :background (when chosen +dialog-accent+)
                                :on-click (%menu-command select index)
                                :on-hover (%menu-command hover index)
                                :children (list (ui :text :text item :size 18
                                                    :color (if chosen +dialog-background+
                                                               +dialog-text+)))))))
    (list (%dialog-surface
           name
           (%dialog-box 0 8
                        (append (when title
                                  (list (ui :row :justify :center :padding '(28 28 11 28)
                                            :children (list (ui :text :text title :size 24
                                                                :color +dialog-text+)))))
                                rows
                                (list (ui :rect :height 20)))
                        :align :stretch)
           :backdrop t :dismiss cancel)
          (keyboard-grab)
          (bind-key nil "Up" up) (bind-key nil "k" up)
          (bind-key nil "Down" down) (bind-key nil "j" down)
          (bind-key nil "Return" select) (bind-key nil "Escape" cancel))))

(defun menu-choice (event selected count &key (select :select) (cancel :cancel) (up :up) (down :down)
                                               (hover :hover))
  "Interpret a menu event: (values :select index), (values :cancel), or
(values :move index)."
  (let* ((command (getf event :command))
         (prefix (format nil "~(~A~)-" select))
         (hover-prefix (format nil "~(~A~)-" hover)))
    (cond ((not (stringp command)) nil)
          ((equal command (string-downcase select)) (values :select selected))
          ((equal command (string-downcase cancel)) (values :cancel))
          ((equal command (string-downcase up)) (values :move (mod (1- selected) (max 1 count))))
          ((equal command (string-downcase down)) (values :move (mod (1+ selected) (max 1 count))))
          ((and (> (length command) (length hover-prefix))
                (string= hover-prefix command :end2 (length hover-prefix)))
           (let ((index (parse-integer command :start (length hover-prefix) :junk-allowed t)))
             (when (and index (< -1 index count)) (values :move index))))
          ((and (> (length command) (length prefix)) (string= prefix command :end2 (length prefix)))
           (let ((index (parse-integer command :start (length prefix) :junk-allowed t)))
             (when (and index (< -1 index count)) (values :select index)))))))

(defun sheet-dialog (name rows &key title (dismiss :dismiss))
  "Effects for a key-chip sheet of (KEY LABEL) rows; any key or click runs DISMISS."
  (let ((keys (ui :column :gap 8 :align :end
                  :children (loop for (key) in rows
                                  collect (ui :text :text (format nil " ~A " key) :size 17
                                              :color +dialog-text+ :background +dialog-chip+))))
        (names (ui :column :gap 8
                    :children (loop for (nil label) in rows
                                    collect (ui :text :text label :size 17 :color +dialog-text+)))))
    (list (%dialog-surface
           name
           (%dialog-box 28 19
                        (append (when title (list (ui :text :text title :size 24 :color +dialog-text+)))
                                (list (ui :row :gap 20 :align :start :children (list keys names)))))
           :dismiss dismiss)
          (keyboard-grab :otherwise dismiss))))

(defun toast (name text &key urgent)
  "Effects for a toast stacked at the top center of every output."
  (list (shell-surface name
                       (ui :column :padding 4 :background (if urgent +dialog-urgent+ +dialog-accent+)
                           :children (list (ui :column :padding 16 :background +dialog-background+
                                               :children (list (ui :text :text text :size 17
                                                                   :color +dialog-text+)))))
                       :anchors '(:top) :margin '(24 0 0 0) :layer :overlay :stack :toast
                       :background "#00000000")))
