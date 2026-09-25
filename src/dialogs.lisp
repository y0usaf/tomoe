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

(defparameter +theme+
  '(:base "#1e1e2e" :mantle "#181825" :crust "#11111b"
    :surface0 "#313244" :surface1 "#45475a" :surface2 "#585b70"
    :text "#cdd6f4" :subtext1 "#bac2de" :subtext0 "#a6adc8"
    :overlay2 "#9399b2" :overlay1 "#7f849c" :overlay0 "#6c7086"
    :rosewater "#f5e0dc" :flamingo "#f2cdcd" :pink "#f5c2e7" :mauve "#cba6f7"
    :red "#f38ba8" :maroon "#eba0ac" :peach "#fab387" :yellow "#f9e2af"
    :green "#a6e3a1" :teal "#94e2d5" :sky "#89dcfe" :sapphire "#74c7ec"
    :blue "#89b4fa" :lavender "#b4befe"
    :accent "#89b4fa" :success "#a6e3a1" :warning "#f9e2af" :error "#f38ba8" :info "#74c7ec"
    :font-size 13 :bar-height 32 :bar-padding 12 :widget-gap 8 :panel-padding 16)
  "Catppuccin Mocha design tokens.")

(defparameter +theme-presets+
  '((:catppuccin-mocha)
    (:catppuccin-latte :base "#eff1f5" :mantle "#e6e9ef" :crust "#dce0e8"
     :surface0 "#ccd0da" :surface1 "#bcc0cc" :surface2 "#acb0be"
     :text "#4c4f69" :subtext1 "#5c5f77" :subtext0 "#6c6f85"
     :red "#d20f39" :green "#40a02b" :blue "#1e66f5" :yellow "#df8e1d" :mauve "#8839ef" :peach "#fe640b"
     :accent "#1e66f5" :success "#40a02b" :warning "#df8e1d" :error "#d20f39")
    (:gruvbox-dark :base "#282828" :mantle "#1d2021" :crust "#1d2021"
     :surface0 "#3c3836" :surface1 "#504945" :surface2 "#665c54"
     :text "#ebdbb2" :subtext1 "#d5c4a1" :subtext0 "#bdae93"
     :red "#fb4934" :green "#b8bb26" :blue "#83a598" :yellow "#fabd2f" :mauve "#d3869b" :peach "#fe8019"
     :accent "#fe8019" :success "#b8bb26" :warning "#fabd2f" :error "#fb4934")
    (:tokyo-night :base "#1a1b26" :mantle "#16161e" :crust "#16161e"
     :surface0 "#292e42" :surface1 "#3b4261" :surface2 "#545c7e"
     :text "#c0caf5" :subtext1 "#a9b1d6" :subtext0 "#9aa5ce"
     :red "#f7768e" :green "#9ece6a" :blue "#7aa2f7" :yellow "#e0af68" :mauve "#bb9af7" :peach "#ff9e64"
     :accent "#7aa2f7" :success "#9ece6a" :warning "#e0af68" :error "#f7768e")
    (:nord :base "#2e3440" :mantle "#2e3440" :crust "#2e3440"
     :surface0 "#3b4252" :surface1 "#434c5e" :surface2 "#4c566a"
     :text "#eceff4" :subtext1 "#e5e9f0" :subtext0 "#d8dee9"
     :red "#bf616a" :green "#a3be8c" :blue "#81a1c1" :yellow "#ebcb8b" :mauve "#b48ead" :peach "#d08770"
     :accent "#81a1c1" :success "#a3be8c" :warning "#ebcb8b" :error "#bf616a"))
  "Named partial overrides of +THEME+.")

(defun theme (key &optional overrides)
  "The token KEY from OVERRIDES (a plist or a preset name) over +THEME+."
  (let ((overrides (if (keywordp overrides)
                       (or (rest (assoc overrides +theme-presets+)) (error "Unknown theme preset ~S." overrides))
                       overrides)))
    (getf overrides key (getf +theme+ key))))

(defun bar-layout (left center right &optional theme)
  "A bar row of three equal sections: LEFT at the start, CENTER centered, RIGHT at the end."
  (flet ((section (children justify)
           (ui :row :grow 1 :gap (theme :widget-gap theme) :justify justify :children children)))
    (ui :row :grow 1 :padding (list 0 (theme :bar-padding theme) 0 (theme :bar-padding theme))
        :children (list (section left :start) (section center :center) (section right :end)))))

(defun workspaces-widget (snapshot &key show-empty (gap 4) theme)
  "Workspace labels from the wm's published :WM-STATE: the active one in the accent
colour, occupied ones in text colour, empty ones only with SHOW-EMPTY. Read :DATA."
  (let ((state (state-value snapshot :wm-state)))
    (ui :row :gap gap
        :children (loop for workspace in (getf state :workspaces)
                        for active = (eql (getf workspace :id) (getf state :active))
                        for occupied = (plusp (getf workspace :windows 0))
                        when (or active occupied show-empty)
                          collect (ui :text :text (princ-to-string (getf workspace :id))
                                      :size (theme :font-size theme)
                                      :color (theme (cond (active :accent) (occupied :text) (t :overlay0))
                                                    theme))))))

(defun clock-text (&optional (format "%H:%M"))
  "The local time formatted by strftime FORMAT."
  (let ((tm (sb-alien:make-alien (sb-alien:unsigned 8) 128))
        (out (sb-alien:make-alien sb-alien:char 256)))
    (unwind-protect
         (sb-alien:with-alien ((now sb-alien:long 0))
           (sb-alien:alien-funcall (sb-alien:extern-alien "tzset" (function sb-alien:void)))
           (setf now (sb-alien:alien-funcall (sb-alien:extern-alien "time" (function sb-alien:long (* t)))
                                             nil))
           (sb-alien:alien-funcall (sb-alien:extern-alien "localtime_r" (function (* t) (* sb-alien:long) (* t)))
                                   (sb-alien:addr now) tm)
           (if (zerop (sb-alien:alien-funcall
                       (sb-alien:extern-alien "strftime" (function sb-alien:unsigned-long (* sb-alien:char)
                                                                   sb-alien:unsigned-long sb-alien:c-string (* t)))
                       out 256 format tm))
               ""
               (sb-alien:cast out sb-alien:c-string)))
      (sb-alien:free-alien tm)
      (sb-alien:free-alien out))))
