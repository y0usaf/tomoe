(in-package #:tomoe-user)
(defparameter +policy-displays+ (list (list "DP-4" :icc "/nix/store/ajsjigd9g9267vbjyzzhp89d3q891k44-ls49ag95.icc" :mode (list 5120 1440)) (list "HDMI-A-2" :mode (list 1920 1080 60) :position (list 5120 0))))
(defparameter +policy-settings+ (list :|FOCUS-FOLLOWS-MOUSE| t :|HONOR-XDG-ACTIVATION-WITH-INVALID-SERIAL| t :|POINTER-FOLLOWS-FOCUS| t :|WAIT-FOR-FRAME-COMPLETION| t))
(defparameter +policy-launcher+ (list :|APP-ID| "launcher" :|RATIO| 1/3))
(defparameter +policy-hidden-window+ (list :|APP-ID| "steam_proton" :|TITLE-PREFIX| "Lovely"))
(defparameter +policy-terminal+ (list :|APP-ID| "so.stencil.tern"))
(defparameter +policy-bindings+ (list (list (list :alt) "2" (list "launch" :argv (list "librewolf"))) (list (list :alt) "3" (list "launch" :argv (list "discord"))) (list (list :alt) "4" (list "launch" :argv (list "steam"))) (list (list :alt) "5" (list "launch" :argv (list "obs"))) (list (list :alt) "9" "power" :description "Blank/Restore All Outputs") (list (list :super) "r" (list "launch" :argv (list "sh" "-c" "/nix/store/h91c3al2hksj1c6y904b825qghb7viq0-tern-script-0.2.0/bin/tern-launcher")) :description "Run an Application") (list (list :alt) "e" (list "launch" :argv (list "pcmanfm")) :description "File Manager") (list (list :alt) "q" "close-window" :description "Close Window") (list (list :alt :shift) "e" "quit") (list (list :alt) "g" "screenshot" :description "Screenshot") (list (list :alt :shift) "g" "screenshot-screen" :description "Screenshot Screen") (list nil "XF86AudioLowerVolume" (list "launch" :argv (list "wpctl" "set-volume" "@DEFAULT_AUDIO_SINK@" "5%-"))) (list nil "XF86AudioMicMute" (list "launch" :argv (list "wpctl" "set-mute" "@DEFAULT_AUDIO_SOURCE@" "toggle"))) (list nil "XF86AudioMute" (list "launch" :argv (list "wpctl" "set-mute" "@DEFAULT_AUDIO_SINK@" "toggle"))) (list nil "XF86AudioNext" "track-next") (list nil "XF86AudioPlay" "play-pause") (list nil "XF86AudioPrev" "track-prev") (list nil "XF86AudioRaiseVolume" (list "launch" :argv (list "wpctl" "set-volume" "@DEFAULT_AUDIO_SINK@" "5%+"))) (list nil "XF86MonBrightnessDown" "brightness-down") (list nil "XF86MonBrightnessUp" "brightness-up")))
(defparameter +policy-launches+ (list (list "browser" (list "librewolf")) (list "discord" (list "discord")) (list "files" (list "pcmanfm")) (list "launcher" (list "sh" "-c" "/nix/store/h91c3al2hksj1c6y904b825qghb7viq0-tern-script-0.2.0/bin/tern-launcher")) (list "mic-mute" (list "wpctl" "set-mute" "@DEFAULT_AUDIO_SOURCE@" "toggle")) (list "obs" (list "obs")) (list "steam" (list "steam")) (list "terminal" (list "tern")) (list "volume-down" (list "wpctl" "set-volume" "@DEFAULT_AUDIO_SINK@" "5%-")) (list "volume-mute" (list "wpctl" "set-mute" "@DEFAULT_AUDIO_SINK@" "toggle")) (list "volume-up" (list "wpctl" "set-volume" "@DEFAULT_AUDIO_SINK@" "5%+"))))
(defparameter +layout+ (list :|FLOAT-RATIO| 3/5 :|GAPS| 8 :|RATIOS| (list 1/2 21/32 11/32) :|SEAM| 58 :|TAB-FONT| "Nib" :|TAB-SIZE| 16))
(defparameter +layout-bindings+ (list (list (list :alt) "h" "focus-left" :description "Focus Left Column") (list (list :alt) "l" "focus-right" :description "Focus Right Column") (list (list :alt) "j" "scroll-down" :description "Scroll Deck Down") (list (list :alt) "k" "scroll-up" :description "Scroll Deck Up") (list (list :alt :shift) "h" "swap-columns" :description "Swap Columns") (list (list :alt :shift) "l" "swap-columns") (list (list :alt :shift) "j" "move-down" :description "Move Window Down") (list (list :alt :shift) "k" "move-up" :description "Move Window Up") (list (list :alt) "bracketleft" "to-left" :description "Move Window to Left Column") (list (list :alt) "bracketright" "to-right" :description "Move Window to Right Column") (list (list :alt) "o" "grid" :description "Toggle Even Grid") (list (list :alt) "r" "ratio" :description "Cycle Column Split (16:9+16:9 / 21:9+11:9 / 11:9+21:9)") (list (list :alt) "f" "fullscreen" :description "Toggle Fullscreen") (list (list :super) "space" "floating" :description "Toggle Floating") (list (list :super) "Tab" "next") (list (list :super :shift) "Tab" "previous") (list (list :alt) "t" "terminal" :description "Terminal")))

(in-package #:tomoe-user)

(defun policy--prefix-p (prefix text)
  (and (stringp text) (>= (length text) (length prefix))
       (string= prefix text :end2 (length prefix))))

(defun policy--launch (command)
  (second (assoc command +policy-launches+ :test #'equal)))

(defun policy--terminal (windows)
  (getf (find (getf +policy-terminal+ :app-id) windows
              :key (lambda (window) (getf window :app-id)) :test #'equal)
        :id))

(defun policy--unmanaged-p (snapshot window)
  (let ((properties (rules-for snapshot window)))
    (or (member (getf window :id) (state-value snapshot :wm-exclude)) (cdr (assoc :float properties)) (cdr (assoc :hidden properties)))))

(defun policy--area (snapshot &optional name)
  (let ((areas (context snapshot :workareas)))
    (or (find name areas :test #'equal :key (lambda (area) (getf area :name)))
        (first areas))))

(defun policy--centred (area ratio)
  (let ((width (max 1 (floor (* (getf area :width) ratio))))
        (height (max 1 (floor (* (getf area :height) ratio)))))
    (list (+ (getf area :x) (floor (- (getf area :width) width) 2))
          (+ (getf area :y) (floor (- (getf area :height) height) 2))
          width height)))

(define-extension "session" () (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (append (mapcar (lambda (display) (apply #'configure-output display)) +policy-displays+)
                  (list (apply #'settings +policy-settings+)))
          nil))

(define-extension "rules" () (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (window-rule :launcher
                             :match (lambda (window snapshot)
                                      (declare (ignore snapshot))
                                      (or (equal (getf window :app-id) (getf +policy-launcher+ :app-id)) (and (equal (getf window :app-id) (getf +policy-terminal+ :app-id)) (equal (getf window :title) "launcher"))))
                             :properties '((:float . t)) :reads '(:workareas)
                             :apply (lambda (window snapshot state event)
                                      (declare (ignore event))
                                      (let ((id (getf window :id))
                                            (area (policy--area snapshot)))
                                        (values state
                                                (append (when area
                                                          (list (apply #'place id (policy--centred
                                                                                   area (getf +policy-launcher+ :ratio)))))
                                                        (list (window-properties id :border '(:focused "#00000000" :unfocused "#00000000")) (focus id)))
                                                nil))))
                (window-rule :hidden :app-id (getf +policy-hidden-window+ :app-id)
                             :match (lambda (window snapshot)
                                      (declare (ignore snapshot))
                                      (policy--prefix-p (getf +policy-hidden-window+ :title-prefix)
                                                        (getf window :title)))
                             :properties '((:hidden . t))
                             :apply (lambda (window snapshot state event)
                                      (declare (ignore snapshot event))
                                      (values state (list (hide-window (getf window :id))) nil))))
          nil))

(in-package #:tomoe-user)

(defun deck--state ()
  (list :grid nil :ratio 1 :focus nil
        :columns nil :windows nil
        :fronts (list (cons :left nil) (cons :right nil))
        :floating nil :floats nil :fullscreen nil))

(defun deck--entry (list id) (assoc id list))
(defun deck--layout-entry (layout id) (find id layout :key (lambda (entry) (getf entry :id))))
(defun deck--windows (state) (getf state :windows))
(defun deck--fronts (state) (getf state :fronts))
(defun deck--side-of (state id)
  (let ((entry (deck--entry (getf state :columns) id))) (and entry (cdr entry))))
(defun deck--column (state side)
  (remove-if-not (lambda (id) (eql (deck--side-of state id) side)) (deck--windows state)))
(defparameter +deck--column-commands+
  '("focus-left" "focus-right" "swap-columns" "to-left" "to-right" "ratio"))

(defparameter +deck-commands+
  (list (define-command "focus-left" :description "Focus the left column's front window.")
        (define-command "focus-right" :description "Focus the right column's front window.")
        (define-command "scroll-down" :description "Bring the next window in the column forward.")
        (define-command "scroll-up" :description "Bring the previous window in the column forward.")
        (define-command "swap-columns" :description "Swap the left and right columns.")
        (define-command "move-down" :description "Move the focused window down its column.")
        (define-command "move-up" :description "Move the focused window up its column.")
        (define-command "to-left" :description "Send the focused window to the left column.")
        (define-command "to-right" :description "Send the focused window to the right column.")
        (define-command "grid" :description "Toggle the even tiling grid.")
        (define-command "ratio" :description "Cycle the column split between 16:9, 21:9 and 11:9.")
        (define-command "floating" :description "Toggle the focused window between floating and tiled.")
        (define-command "fullscreen" :description "Toggle the focused window between fullscreen and tiled.")
        (define-command "next" :description "Cycle the deck to the next window.")
        (define-command "previous" :description "Cycle the deck to the previous window.")
        (define-command "terminal" :description "Focus the terminal, or start one when none is open.")))
(defun deck--narrow-p (area)
  (let ((output (and area (getf area :output))))
    (and output (< (* 9 (getf output :width)) (* 21 (getf output :height))))))
(defun deck--front (state side)
  "The window SIDE's deck shows in front. An adopted front counts only while it is
still in that column: a front can outlive its window between a close and the
handoff that replaces it, and a column placed against a front that is gone would
be placed nowhere and show nothing at all."
  (let* ((entry (assoc side (deck--fronts state)))
         (recorded (and entry (cdr entry)))
         (column (deck--column state side)))
    (if (member recorded column) recorded (first column))))
(defun deck--after-front (state order side gone)
  "The window that takes the front of SIDE in ORDER once GONE closes, or NIL when
the column empties. GONE still sits in ORDER, so the entry after it is the window
a scroll would have revealed; at the bottom of the column, the one above it."
  (let* ((column (remove-if-not (lambda (id) (eql (deck--side-of state id) side)) order))
         (index (position gone column))
         (next (and index (nth (if (< (1+ index) (length column)) (1+ index) (max 0 (1- index))) column))))
    (unless (eql next gone) next)))
(defun deck--manageable-p (state id)
  (and (not (member id (getf state :fullscreen))) (not (member id (getf state :floating)))))

(defparameter +deck--tab-gap+ 3
  "Logical pixels between two tab rows in a stack.")
(defparameter +deck--tab-frame+ 3
  "Logical pixels of stack face showing around and between the tab rows.")
(defparameter +deck--tab-pad+ 10
  "Logical pixels of breathing room either side of a tab's title.")
(defparameter +deck--tab-radius+ 3
  "Logical corner radius of a tab row.")
(defparameter +deck--tab-advance+ 0.62
  "Roughly how much of a tab's font size one character covers, for fitting titles.
Titles are also clipped by their row, so a wrong estimate costs space, never an
overflow past the row's edges.")
(defparameter +deck--tab-colours+
  (list :base "#1e1e2e" :text "#cdd6f4" :accent "#89b4fa")
  "Theme colours for the stacks, used until the :THEME publisher carries its own.")

(defun deck--scale (area)
  (let ((output (getf area :output))) (/ (getf output :scale-120 120) 120)))
(defun deck--tab-row-height (area)
  "One tab row's height in screen pixels: a title's line plus room above and below."
  (let ((size (deck--tab-size)))
    (round (* (deck--scale area) (+ size 20)))))
(defun deck--tab-stack-height (state area side)
  "A column's stack height in screen pixels: one row per tab window, the rows
gapped like the cards they name."
  (let ((count (length (deck--tab-windows state side))))
    (when (plusp count)
      (let ((scale (deck--scale area)))
        (+ (* count (deck--tab-row-height area))
           (round (* scale (+ (* 2 +deck--tab-frame+)
                              (* +deck--tab-gap+ (1- count))))))))))
(defun deck--tab-font ()
  (or (getf +layout+ :tab-font) "monospace, Bold"))
(defun deck--tab-size ()
  (or (getf +layout+ :tab-size) 16))
(defun deck--colour (theme key)
  (or (getf theme key) (getf +deck--tab-colours+ key)))
(defun deck--channel (value)
  "A linearised 0-1 sRGB channel from VALUE, 0-255."
  (let ((level (/ value 255.0)))
    (if (> level 0.04045) (expt (/ (+ level 0.055) 1.055) 2.4) (/ level 12.92))))
(defun deck--luminance (colour)
  "Relative luminance of an RRGGBB COLOUR, or NIL when it is not one."
  (when (and (stringp colour) (= (length colour) 7))
    (destructuring-bind (red green blue)
        (loop for i from 1 by 2 repeat 3
              collect (parse-integer colour :start i :end (+ i 2) :radix 16))
      (+ (* 0.2126 (deck--channel red))
         (* 0.7152 (deck--channel green))
         (* 0.0722 (deck--channel blue))))))
(defun deck--contrast (one other)
  "The contrast ratio between two relative luminances."
  (let ((high (max one other)) (low (min one other))) (/ (+ high 0.05) (+ low 0.05))))
(defun deck--on (fill one other)
  "Whichever of ONE and OTHER reads better on FILL."
  (let ((ground (deck--luminance fill))
        (first (deck--luminance one))
        (second (deck--luminance other)))
    (if (and ground first second (> (deck--contrast ground first) (deck--contrast ground second)))
        one
        other)))

(defun deck--mix (one other fraction)
  "ONE blended FRACTION of the way to OTHER, both RRGGBB."
  (if (and (stringp one) (stringp other) (= (length one) 7) (= (length other) 7))
      (format nil "#~{~A~}"
              (loop for i from 1 by 2 repeat 3
                    collect (format nil "~2,'0X"
                                    (round (+ (* (- 1 fraction) (parse-integer one :start i :end (+ i 2) :radix 16))
                                              (* fraction (parse-integer other :start i :end (+ i 2) :radix 16)))))))
      one))
(defun deck--tabs-p (state area)
  "Whether the deck draws tab strips at all: only where it has two tiled columns
on a wide output, so never in the even grid, beside a fullscreen window, or on an
output the deck lays out as one column."
  (and area (not (getf state :grid)) (not (getf state :fullscreen)) (not (deck--narrow-p area))))
(defun deck--tab-windows (state side)
  "The windows of SIDE that get a tab: the column's tiled windows, never a float."
  (remove-if-not (lambda (id) (deck--manageable-p state id)) (deck--column state side)))
(defun deck--tab-strip-p (state area side)
  (and (deck--tabs-p state area) (deck--tab-windows state side)))
(defun deck--tab-title (window)
  "A tab's label: the window title, else its app id, else a word for a bare window."
  (let ((title (getf window :title)) (app (getf window :app-id)))
    (cond ((and (stringp title) (plusp (length title))) title)
          ((and (stringp app) (plusp (length app))) app)
          (t "window"))))
(defun deck--tab-fit (text width)
  "TEXT cut to WIDTH logical pixels at the tab's size, with an ellipsis when it has
to lose something."
  (let* ((limit (max 1 (floor (/ width (* +deck--tab-advance+ (deck--tab-size))))))
         (length (length text)))
    (if (<= length limit)
        text
        (concatenate 'string (subseq text 0 (max 0 (1- limit))) (string (code-char #x2026))))))

(defun deck--column-frame (state area side)
  "The gap-inset box (X Y WIDTH HEIGHT) of SIDE's column on AREA."
  (let* ((gaps (getf +layout+ :gaps))
         (x (+ (getf area :x) gaps))
         (y (+ (getf area :y) gaps))
         (width (- (getf area :width) (* 2 gaps)))
         (height (- (getf area :height) (* 2 gaps)))
         (seam (getf +layout+ :seam))
         (left (deck--left-width state width)))
    (if (eql side :left)
        (list x y left height)
        (list (+ x left seam) y (- width seam left) height))))

(defun deck--tab-strip (state area side)
  "SIDE's stack in screen pixels: the top of the column's own box, where the tab
rows sit and that the column's windows are placed under."
  (let ((output (getf area :output)))
    (destructuring-bind (x y width height) (deck--column-frame state area side)
      (declare (ignore height))
      (list :output (getf output :name) :ox (getf output :x) :oy (getf output :y)
            :x x :y y :width width :height (deck--tab-stack-height state area side)
            :row (deck--tab-row-height area) :scale (deck--scale area)))))

(defun deck--tab-surface (state theme strip side windows)
  "One strip of tabs for SIDE, or NIL when the strip has nothing to show.
WINDOWS are the managed windows the tabs name."
  (let* ((tabs (deck--tab-windows state side))
         (front (deck--front state side))
         (size (deck--tab-size))
         (font (deck--tab-font))
         (width (- (/ (getf strip :width) (getf strip :scale)) (* 2 +deck--tab-frame+)))
         (row-height (/ (getf strip :row) (getf strip :scale)))
         (count (length tabs)))
    (when (plusp count)
      (let ((base (deck--colour theme :base))
            (ink (deck--colour theme :text))
            (accent (deck--colour theme :accent)))
        (shell-surface (if (eql side :left) :deck-tabs-left :deck-tabs-right)
                       (ui :column :gap +deck--tab-gap+ :padding +deck--tab-frame+
                           :radius +deck--tab-radius+
                           :background (deck--mix base ink 0.38)
                           :children
                           (loop for id in tabs
                                 for label = (deck--tab-title
                                              (find id windows :key (lambda (window) (getf window :id))))
                                 for active = (eql id front)
                                 collect
                                 (ui :button :key (intern (format nil "TAB-~D" id) :keyword)
                                     :on-click :tab
                                     :width width :height row-height
                                     :radius +deck--tab-radius+
                                     :align :center :justify :center
                                     :background (if active accent base)
                                     :children
                                     (list (ui :text
                                               :text (deck--tab-fit label (- width (* 2 +deck--tab-pad+)))
                                               :size size :font font
                                               :color (if active (deck--on accent base ink) ink))))))
                       :output (getf strip :output)
                       :anchors '(:top :left)
                       :margin (list (round (- (getf strip :y) (getf strip :oy)) (getf strip :scale))
                                     0 0
                                     (round (- (getf strip :x) (getf strip :ox)) (getf strip :scale)))
                       :width (round (getf strip :width) (getf strip :scale))
                       :height (round (getf strip :height) (getf strip :scale))
                       :layer :top
                       :background "#00000000")))))

(defun deck--current (state)
  "The window a command acts on: the focused one while it is in the window set,
otherwise whatever the deck still shows. A command must never depend on an id a
gone window left behind, or the keys go dead."
  (let ((focus (getf state :focus))
        (windows (deck--windows state)))
    (or (and (member focus windows) focus)
        (deck--front state :left)
        (deck--front state :right)
        (first windows))))

(defun deck--output-of (outputs entry)
  (let ((cx (+ (getf entry :x) (floor (getf entry :width) 2)))
        (cy (+ (getf entry :y) (floor (getf entry :height) 2))))
    (find-if (lambda (output)
               (and (<= (getf output :x) cx (+ (getf output :x) (getf output :width)))
                    (<= (getf output :y) cy (+ (getf output :y) (getf output :height)))))
             outputs)))

(defun deck--area (snapshot)
  "The focused window's output box and the area its exclusive zones leave free."
  (let* ((outputs (context snapshot :outputs))
         (focus (previous-context snapshot :focus))
         (entry (and focus (deck--layout-entry (previous-context snapshot :layout) focus)))
         (output (or (and entry (deck--output-of outputs entry)) (first outputs)))
         (area (and output (policy--area snapshot (getf output :name)))))
    (when area
      (list* :output output area))))

(defun deck--left-width (state width)
  "How wide the left column is, out of WIDTH less the seam between the columns."
  (floor (* (- width (getf +layout+ :seam)) (nth (1- (getf state :ratio)) (getf +layout+ :ratios)))))

(defun deck--grid-columns (state)
  (ceiling (sqrt (count-if (lambda (id) (deck--manageable-p state id)) (deck--windows state)))))

(defun deck--grid-extra (cols)
  "How much wider than a window gap the middle of an even grid is: it splits on
the deck's seam, so whatever stands there stays between the columns."
  (if (evenp cols) (- (getf +layout+ :seam) (getf +layout+ :gaps)) 0))

(defun deck--grid-cell (cols width)
  (floor (- width (* (1- cols) (getf +layout+ :gaps)) (deck--grid-extra cols)) cols))

(defun deck--split (state width)
  "Where the seam starts within the tiled WIDTH, or NIL when the deck has no
middle seam: a grid with an odd number of columns."
  (if (getf state :grid)
      (let ((cols (deck--grid-columns state)))
        (when (and (plusp cols) (evenp cols))
          (let ((half (/ cols 2)))
            (+ (* half (deck--grid-cell cols width)) (* (1- half) (getf +layout+ :gaps))))))
      (deck--left-width state width)))

(defun deck--seam (state area)
  "The free strip in the deck's middle seam on AREA, a window gap in from each
side; NIL on a narrow output, while a window is fullscreen, in a grid with an
odd number of columns, or when the seam is no wider than two gaps."
  (let* ((gaps (getf +layout+ :gaps))
         (x (and area (+ (getf area :x) gaps)))
         (split (and area (deck--split state (- (getf area :width) (* 2 gaps)))))
         (inner (- (getf +layout+ :seam) (* 2 gaps))))
    (unless (or (null split) (getf state :fullscreen) (deck--narrow-p area) (not (plusp inner)))
      (list :output (getf (getf area :output) :name)
            :x (+ x split gaps) :y (+ (getf area :y) gaps)
            :width inner :height (- (getf area :height) (* 2 gaps))))))

(defun deck--box (state area id)
  "Where ID goes, or NIL when it is not in the window set."
  (let* ((gaps (getf +layout+ :gaps))
         (x (+ (getf area :x) gaps)) (y (+ (getf area :y) gaps))
         (width (- (getf area :width) (* 2 gaps))) (height (- (getf area :height) (* 2 gaps))))
    (cond
      ((not (member id (deck--windows state))) nil)
      ((member id (getf state :fullscreen))
       (let ((output (getf area :output)))
         (list (getf output :x) (getf output :y) (getf output :width) (getf output :height))))
      ((member id (getf state :floating)) (cdr (deck--entry (getf state :floats) id)))
      ((getf state :grid)
       (let* ((wins (remove-if-not (lambda (candidate) (deck--manageable-p state candidate))
                                   (deck--windows state)))
              (index (position id wins))
              (count (length wins))
              (cols (ceiling (sqrt count)))
              (rows (ceiling count cols))
              (cell-width (deck--grid-cell cols width))
              (cell-height (floor (- height (* (1- rows) gaps)) rows))
              (column (mod index cols))
              (row (floor index cols)))
         (list (+ x (* column (+ cell-width gaps)) (if (>= column (/ cols 2)) (deck--grid-extra cols) 0))
               (+ y (* row (+ cell-height gaps)))
               cell-width cell-height)))
      ((deck--narrow-p area)
       (let* ((column (remove-if-not (lambda (candidate) (deck--manageable-p state candidate))
                                     (deck--windows state)))
              (front (or (position (deck--current state) column) 0))
              (index (position id column)))
         (when index
           (list x (+ y (* (- index front) (+ height gaps))) width height))))
      (t
       (let* ((side (deck--side-of state id))
              (column (deck--column state side))
              (front (position (deck--front state side) column))
              (index (position id column))
              (strip (and side (deck--tab-strip-p state area side)
                          (+ (deck--tab-stack-height state area side) gaps))))
         (when (and side index front)
           (destructuring-bind (column-x column-y column-width column-height)
               (deck--column-frame state area side)
             (let ((card-y (+ column-y (or strip 0)))
                   (card-height (max 1 (- column-height (or strip 0)))))
               (list column-x (+ card-y (* (- index front) (+ card-height gaps)))
                     column-width card-height)))))))))

(defun deck--places (managed state area)
  "One place effect per managed window: exactly one owner, no duplicates."
  (let ((windows (deck--windows state)))
    (loop for window in managed
          for id = (getf window :id)
          for box = (deck--box state area id)
          append (list (if box
                           (apply #'place id (append box (list t)))
                           (place id 0 0 (max 1 (getf window :width))
                                  (max 1 (getf window :height)) nil)))
          append (list (fullscreen id (and box (member id windows)
                                           (member id (getf state :fullscreen))
                                           t))))))

(defun deck--focus-window (state id)
  (setf (getf state :focus) id)
  state)

(defun deck--adopt-front (state side id)
  (let ((fronts (deck--fronts state)))
    (setf (cdr (assoc side fronts)) id))
  state)

(defun deck--rotate (state direction)
  "Scroll the side holding the current window; the deck wraps."
  (let* ((id (deck--current state))
         (side (deck--side-of state id))
         (column (deck--column state side)))
    (when (and side column)
      (let* ((index (position id column))
             (target (nth (mod (+ index direction) (length column)) column)))
        (deck--adopt-front state side target)
        (deck--focus-window state target)))))

(defun deck--reorder (state direction)
  "Swap the current window with its neighbour in its own column's order."
  (let* ((id (deck--current state))
         (side (deck--side-of state id))
         (column (deck--column state side))
         (index (position id column))
         (other (and index (>= (+ index direction) 0) (nth (+ index direction) column)))
         (order (deck--windows state))
         (mine (and other (position id order)))
         (theirs (and other (position other order))))
    (when (and other mine theirs)
      (setf (nth mine order) other
            (nth theirs order) id))
    state))

(defun deck--reorder-all (state direction)
  (let* ((order (deck--windows state))
         (index (position (deck--current state) order))
         (target (and index (+ index direction))))
    (when (and target (< -1 target (length order)))
      (rotatef (nth index order) (nth target order)))
    state))

(defun deck--cycle (state direction)
  "Focus the next or previous window in the window set, wrapping."
  (let* ((order (deck--windows state))
         (id (deck--current state))
         (index (or (position id order) 0))
         (target (and order (nth (mod (+ index direction) (length order)) order))))
    (when target
      (let ((side (deck--side-of state target)))
        (when side (deck--adopt-front state side target)))
      (deck--focus-window state target))))

(defun deck--swap-columns (state)
  (dolist (entry (getf state :columns))
    (setf (cdr entry) (if (eql (cdr entry) :left) :right :left)))
  (let ((fronts (deck--fronts state)))
    (let ((left (cdr (assoc :left fronts))) (right (cdr (assoc :right fronts))))
      (setf (cdr (assoc :left fronts)) right (cdr (assoc :right fronts)) left)))
  state)

(defun deck--to-column (state side)
  "Send the current window to SIDE, where it becomes that deck's front."
  (let* ((id (deck--current state))
         (previous (deck--side-of state id)))
    (when (and id (deck--manageable-p state id))
      (when (and previous (not (eql previous side)) (eql id (deck--front state previous)))
        (let* ((column (deck--column state previous))
               (index (position id column))
               (next (and index (nth (mod (1+ index) (length column)) column))))
          (deck--adopt-front state previous (if (and next (not (eql next id))) next nil))))
      (setf (cdr (assoc id (getf state :columns))) side)
      (deck--adopt-front state side id))
    state))

(defun deck--set-fullscreen (state id on)
  (setf (getf state :fullscreen) (remove id (getf state :fullscreen)))
  (when on
    (setf (getf state :floating) (remove id (getf state :floating)))
    (push id (getf state :fullscreen))
    (deck--focus-window state id))
  state)

(defun deck--toggle-floating (state area)
  (let ((id (deck--current state)))
    (cond
      ((null id))
      ((member id (getf state :fullscreen)) (deck--set-fullscreen state id nil))
      ((member id (getf state :floating))
       (setf (getf state :floating) (remove id (getf state :floating))
             (getf state :floats) (remove id (getf state :floats) :key #'car)))
      (area
       (push id (getf state :floating))
       (push (cons id (policy--centred area (getf +layout+ :float-ratio))) (getf state :floats))))
    state))

(defun deck--command (state area command)
  (cond
    ((and (deck--narrow-p area)
          (or (member command +deck--column-commands+ :test #'equal)
              (member command '("scroll-down" "scroll-up" "move-down" "move-up") :test #'equal)))
     (cond ((equal command "scroll-down") (deck--cycle state 1))
           ((equal command "scroll-up") (deck--cycle state -1))
           ((equal command "move-down") (deck--reorder-all state 1))
           ((equal command "move-up") (deck--reorder-all state -1))))
    ((equal command "focus-left")
     (deck--focus-window state (or (deck--front state :left) (deck--current state))))
    ((equal command "focus-right")
     (deck--focus-window state (or (deck--front state :right) (deck--current state))))
    ((equal command "scroll-down") (deck--rotate state 1))
    ((equal command "scroll-up") (deck--rotate state -1))
    ((equal command "next") (deck--cycle state 1))
    ((equal command "previous") (deck--cycle state -1))
    ((equal command "swap-columns") (deck--swap-columns state))
    ((equal command "move-down") (deck--reorder state 1))
    ((equal command "move-up") (deck--reorder state -1))
    ((equal command "to-left") (deck--to-column state :left))
    ((equal command "to-right") (deck--to-column state :right))
    ((equal command "grid") (setf (getf state :grid) (not (getf state :grid))))
    ((equal command "ratio")
     (setf (getf state :ratio) (1+ (mod (getf state :ratio) (length (getf +layout+ :ratios))))))
    ((equal command "floating") (deck--toggle-floating state area))
    ((equal command "fullscreen")
     (let ((id (deck--current state)))
       (when id (deck--set-fullscreen state id (not (member id (getf state :fullscreen)))))))
    (t state)))

(defun deck--request (state id request requested)
  (case request
    (:fullscreen (deck--set-fullscreen state id requested))
    (:unfullscreen (deck--set-fullscreen state id nil))
    (:activate
     (let ((side (deck--side-of state id)))
       (when (and side (deck--manageable-p state id)) (deck--adopt-front state side id))
       (deck--focus-window state id))))
  state)

(defun deck--tab-click (state element)
  "Bring the window a tab names to the front of its column and focus it. ELEMENT
is the tab's hit key, TAB-ID."
  (let ((id (and (stringp element)
                 (>= (length element) 4)
                 (string= "tab-" element :end2 4)
                 (parse-integer element :start 4 :junk-allowed t))))
    (when (and id (member id (deck--windows state)))
      (let ((side (deck--side-of state id)))
        (when (and side (deck--manageable-p state id)) (deck--adopt-front state side id)))
      (deck--focus-window state id)))
  state)

(defun deck--raises (state current)
  (mapcar #'raise-window
          (append (remove current (reverse (getf state :floating)))
                  (when (member current (getf state :floating)) (list current))
                  (getf state :fullscreen))))

(define-extension "deck"
    (:reads (:windows :rules :data :outputs :workareas :layout :focus :command :button :pointer :request
             :settings :ui)
     :state (deck--state) :admission t)
    (snapshot state event)
  (let* ((windows (remove-if (lambda (window) (policy--unmanaged-p snapshot window))
                             (context snapshot :windows)))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (focused (previous-context snapshot :focus))
         (area (deck--area snapshot))
         (type (getf event :type))
         (command (and (eq type :command) (equal (getf event :owner) "deck")
                       (getf event :name)))
         (terminal (and (equal command "terminal") (policy--terminal windows))))
    (when (eq type :unmap)
      (let ((gone (getf event :id)))
        (dolist (side '(:left :right))
          (when (eql gone (cdr (assoc side (getf state :fronts))))
            (let ((next (deck--after-front state (deck--windows state) side gone)))
              (setf (cdr (assoc side (getf state :fronts))) next)
              (when (and next (eql gone (getf state :focus)))
                (setf (getf state :focus) next)))))))
    (setf (getf state :windows) (remove-if-not (lambda (id) (member id ids)) (getf state :windows))
          (getf state :columns) (remove-if-not (lambda (entry) (member (car entry) ids))
                                               (getf state :columns))
          (getf state :fronts)
          (loop for entry in (getf state :fronts)
                collect (if (and (cdr entry) (not (member (cdr entry) ids)))
                            (cons (car entry) nil)
                            entry))
          (getf state :floating) (remove-if-not (lambda (id) (member id ids)) (getf state :floating))
          (getf state :floats) (remove-if-not (lambda (entry) (member (car entry) ids))
                                              (getf state :floats))
          (getf state :fullscreen) (remove-if-not (lambda (id) (member id ids))
                                                  (getf state :fullscreen)))
    (unless (member (getf state :focus) ids) (setf (getf state :focus) nil))
    (when (and focused (not (eql focused (getf state :focus)))
               (member focused (deck--windows state))
               (deck--manageable-p state focused))
      (let ((side (deck--side-of state focused)))
        (when side
          (deck--adopt-front state side focused)
          (deck--focus-window state focused))))
    (dolist (window windows)
      (let ((id (getf window :id)))
        (unless (or (deck--side-of state id) (member id (getf state :floating)))
          (let* ((focus-side (and focused (deck--side-of state focused)))
                 (left (length (deck--column state :left)))
                 (right (length (deck--column state :right)))
                 (side (or focus-side (if (<= left right) :left :right))))
            (push (cons id side) (getf state :columns))
            (setf (getf state :windows) (append (getf state :windows) (list id)))
            (deck--adopt-front state side id)
            (setf (getf state :focus) id)
            (when (getf window :fullscreen-requested)
              (deck--set-fullscreen state id t))))))
    (when command
      (deck--command state area command))
    (when terminal
      (deck--request state terminal :activate nil))
    (when (and (member type '(:metadata :request)) (member (getf event :id) ids))
      (deck--request state (getf event :id) (getf event :request) (getf event :requested)))
    (when (or (and (eq type :button) (eql (getf event :state) :pressed))
              (and (eq type :pointer) (eq (getf event :state) :enter) (getf event :moved)
                   (setting snapshot :focus-follows-mouse)))
      (let ((id (getf event :id)))
        (when (and (integerp id) (member id (deck--windows state)))
          (when (and (deck--manageable-p state id) (deck--side-of state id))
            (deck--adopt-front state (deck--side-of state id) id))
          (deck--focus-window state id))))
    (when (and (eq type :ui) (equal (getf event :command) "tab"))
      (deck--tab-click state (getf event :element)))
    (let ((current (deck--current state)))
      (values state
              (append (mapcar (lambda (binding) (apply #'bind-key binding))
                              (if (deck--narrow-p area)
                                  (remove-if (lambda (binding)
                                               (member (string-downcase (third binding))
                                                       +deck--column-commands+ :test #'equal))
                                             +layout-bindings+)
                                  +layout-bindings+))
                      (when area (deck--places windows state area))
                      (list (publish-state :deck-seam (deck--seam state area)))
                      (let ((theme (state-value snapshot :theme)))
                        (loop for side in '(:left :right)
                              for strip = (and area (deck--tab-strip-p state area side)
                                               (deck--tab-strip state area side))
                              when strip
                                collect (deck--tab-surface state theme strip side windows)))
                      (deck--raises state current)
                      +deck-commands+
                      (list (focus current :raise nil)))
              (when (and (equal command "terminal") (null terminal))
                (list (apply #'launch (policy--launch "terminal"))))))))

(in-package #:tomoe-user)

(defparameter +user-commands+
  (list (define-command "power" :description "Blank or restore every output.")
        (define-command "screenshot" :description "Screenshot a region of an output.")
        (define-command "screenshot-screen" :description "Screenshot a whole output.")
        (define-command "play-pause" :description "Pause or resume the playing media.")
        (define-command "track-next" :description "Play the next track.")
        (define-command "track-prev" :description "Play the previous track.")
        (define-command "brightness-up" :description "Raise the backlight by five percent.")
        (define-command "brightness-down" :description "Lower the backlight by five percent.")))

(define-extension "user" (:reads (:command :focus) :state nil) (snapshot state event)
  (declare (ignore state snapshot))
  (let ((command (and (eq (getf event :type) :command) (getf event :name))))
    (values nil
            (append (mapcar (lambda (binding) (apply #'bind-key binding)) +policy-bindings+)
                    +user-commands+)
            (cond
              ((null command) nil)
              ((equal command "power") (list (output-power :toggle)))
              ((equal command "screenshot") (list (screenshot)))
              ((equal command "screenshot-screen") (list (screenshot :screen)))
              ((equal command "play-pause") (list (media-control :play-pause)))
              ((equal command "track-next") (list (media-control :next)))
              ((equal command "track-prev") (list (media-control :previous)))
              ((equal command "brightness-up") (list (adjust-brightness 5)))
              ((equal command "brightness-down") (list (adjust-brightness -5)))))))

(defparameter +hud+ (list :|EMOJI-FONT| "Noto Color Emoji" :|FONT| "monospace, Bold" :|HEIGHT| 30 :|ICON-FONT| "Symbols Nerd Font" :|SHADOW| 3))
(in-package #:tomoe-user)

(defun hud--now ()
  (/ (float (get-internal-real-time) 1d0) internal-time-units-per-second))

(defun hud--since (at)
  (if at (* 1000 (- (hud--now) at)) most-positive-fixnum))

(defun hud--decay (at ms)
  (let ((elapsed (hud--since at)))
    (if (< elapsed ms) (- 1 (/ elapsed ms)) 0)))

(defun hud--c (p key)
  (or (getf p key)
      (theme (case key
               (:bg :base) ((:fg :color15) :text) (:color1 :red) (:color2 :green) (:color3 :yellow)
               (:color4 :blue) (:color5 :mauve) (:color6 :sky) (:color8 :surface2) (t :accent)))))

(defun hud--hex2 (n) (format nil "~2,'0X" (max 0 (min 255 (round n)))))

(defun hud--alpha (hex aa) (if (= (length hex) 7) (concatenate 'string hex aa) hex))

(defun hud--fade (hex fraction) (hud--alpha hex (hud--hex2 (* 255 fraction))))

(defun hud--mix (a b f)
  (cond ((<= f 0) a)
        ((and (= (length a) 7) (= (length b) 7))
         (format nil "#~{~A~}"
                 (loop for i from 1 by 2 repeat 3
                       collect (hud--hex2 (+ (* (- 1 f) (parse-integer a :start i :end (+ i 2) :radix 16))
                                             (* f (parse-integer b :start i :end (+ i 2) :radix 16)))))))
        (t b)))

(defun hud--text (text size color &optional (font (getf +hud+ :font)))
  (ui :text :text text :size size :color color :font font))

(defun hud--icon (code size color)
  (hud--text (string (code-char code)) size color (getf +hud+ :icon-font)))

(defun hud--dim (p &optional (fraction 0.6)) (hud--fade (hud--c p :fg) fraction))

(defun hud--off (p) (hud--mix (hud--c p :bg) (hud--c p :fg) 0.14))

(defun hud--heat (percent accent p)
  (cond ((>= percent 90) (hud--c p :color1)) ((>= percent 70) (hud--c p :color3)) (t accent)))

(defun hud--frame (p accent flash content &key height)
  (let* ((s (getf +hud+ :shadow))
         (edge (hud--mix accent (hud--c p :fg) flash))
         (body (ui :row :height height :align :stretch
                   :background (hud--alpha (hud--c p :bg) "eb")
                   :border 1 :border-color (hud--alpha edge (if (plusp flash) "ff" "b3"))
                   :children (list (ui :rect :width 3 :background edge) content))))
    (ui :stack :children (list (ui :row :padding (list s 0 0 s) :align :stretch
                                   :children (list (ui :rect :grow 1 :background (hud--mix (hud--c p :bg) edge 0.6))))
                               (ui :row :padding (list 0 s s 0) :children (list body))))))

(defun hud--panel (p accent children &key (flash 0))
  (hud--frame p accent flash (ui :row :align :center :gap 8 :padding '(0 10 0 8) :children children)
              :height (getf +hud+ :height)))

(defparameter +hud-theme+ (list :|BORDER| 2 :|FOCUS-FLASH| 220 :|PALETTE-COMMAND| "cat /run/memory/gtk.css" :|PALETTE-FILE| "/run/memory/gtk.css" :|TICK| 33))
(in-package #:tomoe-user)

(defun theme--words (line)
  (let ((words nil) (start nil))
    (dotimes (i (1+ (length line)) (nreverse words))
      (if (and (< i (length line)) (not (member (char line i) '(#\Space #\Tab #\; #\Return))))
          (unless start (setf start i))
          (when start (push (subseq line start i) words) (setf start nil))))))

(defun theme--hex-p (value)
  (and (member (length value) '(4 7 9)) (char= (char value 0) #\#)
       (every (lambda (c) (digit-char-p c 16)) (subseq value 1))))

(defun theme--resolve (name table)
  (loop with key = name
        repeat 8
        do (let ((value (cdr (assoc key table :test #'equal))))
             (cond ((null value) (return nil))
                   ((char= (char value 0) #\@) (setf key (subseq value 1)))
                   ((theme--hex-p value) (return value))
                   (t (return nil))))))

(defun theme--palette (css)
  (let ((table nil))
    (with-input-from-string (in css)
      (loop for line = (read-line in nil) while line
            do (let ((words (theme--words line)))
                 (when (and (= 3 (length words)) (equal (first words) "@define-color"))
                   (push (cons (second words) (third words)) table)))))
    (loop for (name) in (reverse table)
          for value = (theme--resolve name table)
          when value append (list (intern (string-upcase name) :keyword) value))))

(defun theme--mode (p)
  (let ((bg (getf p :bg)))
    (if (and bg (= (length bg) 7)
             (> (loop for i from 1 by 2
                      for weight in '(0.2126 0.7152 0.0722)
                      sum (* weight (parse-integer bg :start i :end (+ i 2) :radix 16)))
                127.5))
        :light
        :dark)))

(define-extension "theme" (:reads (:focus) :state (list :palette nil :watch nil :focus nil :focus-at nil))
    (snapshot state event)
  (let ((type (getf event :type)) (name (getf event :name)) (focus (context snapshot :focus)))
    (cond ((and (eq type :exec) (member name '(:palette :reread)) (eql (getf event :code) 0))
           (setf (getf state :palette) (or (theme--palette (getf event :stdout)) (getf state :palette))
                 (getf state :watch) t))
          ((and (eq type :watch) (eq name :palette))
           (setf (getf state :palette) (or (theme--palette (getf event :content)) (getf state :palette)))))
    (unless (equal focus (getf state :focus))
      (setf (getf state :focus) focus (getf state :focus-at) (when focus (hud--now))))
    (let* ((fixed (getf +hud-theme+ :palette))
           (p (or fixed (getf state :palette)))
           (flash (hud--decay (getf state :focus-at) (getf +hud-theme+ :focus-flash))))
      (values state
              (append (unless fixed (list (exec-async (if (getf state :watch) :reread :palette)
                                                          (getf +hud-theme+ :palette-command))))
                      (when (getf state :watch) (list (watch-file :palette (getf +hud-theme+ :palette-file))))
                      (when (plusp flash) (list (interval :tick (getf +hud-theme+ :tick))))
                      (when p (list (publish-state :hud-palette p)
                                    (publish-state :theme-mode (theme--mode p))
                                    (publish-state :theme (list :base (hud--c p :bg) :text (hud--c p :fg)
                                                                :accent (hud--c p :accent) :error (hud--c p :color1)
                                                                :surface1 (hud--c p :color8)
                                                                :subtext0 (hud--mix (hud--c p :bg) (hud--c p :fg) 0.78)))))
                      (list (settings :border (list :width (getf +hud-theme+ :border) :radius 0
                                                    :focused (hud--mix (hud--c p :accent) (hud--c p :fg) flash)
                                                    :unfocused (hud--c p :color8)))))
              nil))))

(defparameter +glass+ (list :|OFFSET| 4 :|PASSES| 3))
(in-package #:tomoe-user)

(defun glass--margin (passes offset)
  (min 4096 (truncate (* (expt 2 passes) (+ offset 2)))))

(define-extension "glass" (:reads (:windows) :state nil) (snapshot state event)
  (declare (ignore event))
  (values state
          (cons (settings :blur (list* :enabled t
                                       :anti-artifact-margin
                                       (glass--margin (getf +glass+ :passes) (getf +glass+ :offset))
                                       +glass+))
                (loop for window in (context snapshot :windows)
                      collect (window-properties (getf window :id) :blur t)))
          nil))

(defparameter +cassette+ (list :|CENTER-BETWEEN| (list :time :date) :|COLORS| (list :|DARK| (list :|AMBER| "#F5A833" :|COUNTER| "#0B0B0CCC" :|COUNTER-GHOST| "#2B2A27" :|DIM| "#9A958A" :|FACE| "#00000000" :|GHOST| "#2A2D25" :|GREEN| "#7DB343" :|GROOVE| "#FFFFFF1F" :|HOLE| "#FFFFFF38" :|INK| "#ECE7DC" :|LCD| "#1A1C17" :|LED-OFF| "#FFFFFF1A" :|ORANGE| "#ED5C1C" :|PAPER| "#00000033" :|RED| "#E5482A" :|RIM| "#FFFFFF2E" :|SHADE| "#00000066" :|SHADOW| "#00000059" :|SHINE| "#FFFFFF14" :|TAPE| "#9C7B5E" :|TEAL| "#2BB0A8") :|LIGHT| (list :|AMBER| "#F5A833" :|COUNTER| "#0B0B0CCC" :|COUNTER-GHOST| "#2B2A27" :|DIM| "#6E6A62" :|FACE| "#FFFFFF00" :|GHOST| "#B3B9A6" :|GREEN| "#5E8F2E" :|GROOVE| "#0000001F" :|HOLE| "#00000038" :|INK| "#292928" :|LCD| "#C2C8B6" :|LED-OFF| "#0000001A" :|ORANGE| "#ED5C1C" :|PAPER| "#FFFFFF33" :|RED| "#D63C1A" :|RIM| "#0000002E" :|SHADE| "#0000001F" :|SHADOW| "#00000026" :|SHINE| "#FFFFFF59" :|TAPE| "#5A4636" :|TEAL| "#218C87")) :|DATE| "%a %d %b" :|EDGES| (list :top :bottom) :|FONT| "Nib" :|HOT| 80 :|INSET| 84 :|MEDIA-WIDTH| 34 :|MODULES| (list :media :cpu :memory :time :date :gpu :vram :frame) :|SAMPLE| 1000 :|SCALE| 1.5d0 :|SEGMENTS| 10 :|SPINE-TITLE| 12 :|TIME| "%H:%M"))
(in-package #:tomoe-user)
(defun probe--sample (probe previous system) (let ((sample (copy-list previous))) (ecase probe (:cpu (let ((total (getf system :cpu-total)) (idle (getf system :cpu-idle))) (when total (let ((spent (- total (getf sample :total total)))) (setf (getf sample :percent) (if (plusp spent) (max 0 (min 100 (round (* 100 (- spent (- idle (getf sample :idle idle)))) spent))) (getf sample :percent 0)) (getf sample :total) total (getf sample :idle) idle (getf sample :temp) (getf system :cpu-temperature)))))) (:memory (let ((total (getf system :memory-total)) (available (getf system :memory-available))) (when (and total (plusp total)) (setf (getf sample :percent) (round (* 100 (- total available)) total) (getf sample :used) (round (- total available) 1024))))) (:gpu (let ((gpu (first (getf system :gpus)))) (cond (gpu (setf (getf sample :percent) (max 0 (min 100 (getf gpu :busy))) (getf sample :used) (getf gpu :vram-used) (getf sample :total) (getf gpu :vram-total) (getf sample :temp) (getf gpu :temperature) (getf sample :failures) 0)) (system (setf (getf sample :failures) (1+ (getf sample :failures 0)))))))) sample))
(defun probe--gpu-p (gpu) (and (getf gpu :percent) (< (getf gpu :failures 0) 3)))
(in-package #:tomoe-user)

(defun cassette--px (n) (max 1 (round (* n (getf +cassette+ :scale)))))

(defvar *cassette-mode* :dark)

(defun cassette--c (key) (getf (getf (getf +cassette+ :colors) *cassette-mode*) key))

(defun cassette--text (text color)
  (ui :text :text text :size (cassette--px 11) :line-height (cassette--px 11) :color color :font (getf +cassette+ :font)))

(defun cassette--engraved (text color)
  (ui :stack :children (list (ui :column :padding '(1 0 0 0) :children (list (cassette--text text (cassette--c :shine))))
                             (cassette--text text color))))

(defun cassette--ghost (text) (map 'string (lambda (c) (if (digit-char-p c) #\8 c)) text))

(defun cassette--window (text &key (face (cassette--c :lcd)) (rim (cassette--c :rim)) (ghost (cassette--c :ghost))
                                   (ink (cassette--c :ink)) (shown text))
  (ui :column :background face :border 1 :border-color rim
      :children (list (ui :rect :height 1 :background rim)
                      (ui :row :height (cassette--px 11) :align :center :padding (list 0 (cassette--px 3) 0 (cassette--px 3))
                          :children (list (ui :stack :children (list (cassette--text (cassette--ghost text) ghost)
                                                                     (cassette--text shown ink))))))))

(defun cassette--zone (x)
  (cond ((> x 0.84) (cassette--c :red)) ((> x 0.6) (cassette--c :amber)) (t (cassette--c :green))))

(defun cassette--ladder (fraction)
  (let* ((count (getf +cassette+ :segments))
         (lit (round (* count (max 0 (min 1 fraction))))))
    (ui :row :gap (cassette--px 1) :align :center
        :children (loop for i below count
                        collect (ui :rect :width (cassette--px 4) :height (cassette--px 8)
                                    :background (if (< i lit) (cassette--zone (/ (1+ i) count)) (cassette--c :led-off)))))))

(defun cassette--led (on)
  (ui :rect :width (cassette--px 3) :height (cassette--px 3) :background (if on (cassette--c :orange) (cassette--c :led-off))))

(defun cassette--gauge (label percent detail &optional hot)
  (let ((percent (max 0 (min 100 (round (or percent 0))))))
    (list (cassette--engraved label (cassette--c :ink))
          (cassette--window (format nil "~3D%" percent))
          (cassette--ladder (/ percent 100))
          (when detail (cassette--engraved detail (if hot (cassette--c :orange) (cassette--c :dim)))))))

(defun cassette--degrees (value) (if (realp value) (format nil "~2D°" (max 0 (min 99 (round value)))) "--°"))

(defun cassette--gib (mib) (if (realp mib) (format nil "~4,1FG" (/ mib 1024d0)) "--.-G"))

(defun cassette--hot-p (value) (and (realp value) (>= value (getf +cassette+ :hot))))

(defun cassette--fit (text width)
  (let ((clean (string-upcase (remove-if (lambda (c) (> (char-code c) 126)) text))))
    (format nil "~vA" width (if (> (length clean) width) (concatenate 'string (subseq clean 0 (- width 2)) "..") clean))))

(defun cassette--reel (amount)
  (let* ((box (cassette--px 12))
         (hub (cassette--px 4))
         (size (+ hub (* 2 (round (* (- box hub) (max 0 (min 1 amount))) 2)))))
    (ui :stack :width box :height box
        :children (list (ui :row :width box :height box :justify :center :align :center
                            :children (list (ui :circular-progress :size size :thickness (max 1 (/ (- size hub) 2)) :value 1
                                                :color (cassette--c :tape) :track (cassette--c :tape))))
                        (ui :row :width box :height box :justify :center :align :center
                            :children (list (ui :circular-progress :size hub :thickness 1 :value 1
                                                :color (cassette--c :ink) :track (cassette--c :ink))))))))

(defun cassette--media (snapshot)
  (let* ((service (service-state snapshot :mpris))
         (status (getf service :status))
         (title (getf service :title))
         (artist (getf service :artist))
         (length (getf service :length))
         (position (getf service :position))
         (played (if (and (realp length) (plusp length) (realp position)) (max 0 (min 1 (/ position length))) 0)))
    (when (and (stringp title) (plusp (length title)) (member status '("Playing" "Paused") :test #'equal))
      (list (cassette--led (equal status "Playing"))
            (ui :row :height (cassette--px 12) :align :center :gap (cassette--px 3) :padding (list 0 (cassette--px 4) 0 0)
                :background (cassette--c :paper) :border 1 :border-color (cassette--c :rim)
                :children (list (ui :rect :width (cassette--px 2) :height (- (cassette--px 12) 2) :background (cassette--c :orange))
                                (ui :rect :width (cassette--px 2) :height (- (cassette--px 12) 2) :background (cassette--c :teal))
                                (cassette--reel (- 1 played))
                                (cassette--text (cassette--fit (if (and (stringp artist) (plusp (length artist)))
                                                                   (format nil "~A - ~A" artist title)
                                                                   title)
                                                               (getf +cassette+ :media-width))
                                                (cassette--c :ink))
                                (cassette--reel played)))))))

(defun cassette--counter ()
  (let ((time (clock-text (getf +cassette+ :time))))
    (list (cassette--window time :face (cassette--c :counter) :rim (cassette--c :ink) :ghost (cassette--c :counter-ghost)
                                 :ink (cassette--c :amber)
                                 :shown (if (oddp (parse-integer (clock-text "%S"))) (substitute #\Space #\: time) time)))))

(defun cassette--module (module state snapshot)
  (let ((cpu (getf state :cpu)) (memory (getf state :memory)) (gpu (getf state :gpu)))
    (ecase module
      (:time (cassette--counter))
      (:date (list (cassette--engraved (string-upcase (clock-text (getf +cassette+ :date))) (cassette--c :ink))))
      (:media (cassette--media snapshot))
      (:cpu (cassette--gauge "CPU" (getf cpu :percent) (cassette--degrees (getf cpu :temp)) (cassette--hot-p (getf cpu :temp))))
      (:memory (cassette--gauge "MEM" (getf memory :percent) (cassette--gib (getf memory :used))))
      (:gpu (when (probe--gpu-p gpu)
              (cassette--gauge "GPU" (getf gpu :percent) (cassette--degrees (getf gpu :temp)) (cassette--hot-p (getf gpu :temp)))))
      (:vram (when (probe--gpu-p gpu)
               (cassette--gauge "VRAM" (if (plusp (or (getf gpu :total) 0)) (round (* 100 (or (getf gpu :used) 0)) (getf gpu :total)) 0)
                                (cassette--gib (getf gpu :used)))))
      (:battery (let ((battery (service-state snapshot :battery)))
                  (cassette--gauge (if (getf battery :charging) "CHG" "BAT") (getf battery :percent) nil)))
      (:network (let* ((network (service-state snapshot :network)) (ssid (getf network :ssid)) (online (getf network :connected)))
                  (list (cassette--led online)
                        (cassette--engraved (cassette--fit (cond ((and online (stringp ssid) (plusp (length ssid))) ssid) (online "online") (t "offline")) 16)
                                            (cassette--c :ink)))))
      (:frame (let ((battery (state-value snapshot :frame-battery)))
                (when (getf battery :percent)
                  (cassette--gauge (if (equal (getf battery :status) "Discharging") "VR" "VR+") (getf battery :percent) nil)))))))

(defun cassette--grille ()
  (let ((hole (cassette--px 1)))
    (ui :column :gap hole
        :children (loop repeat 4
                        collect (ui :row :gap hole
                                    :children (loop repeat 6 collect (ui :rect :width hole :height hole :background (cassette--c :hole))))))))

(defun cassette--groove ()
  (ui :row :children (list (ui :rect :width 1 :height (cassette--px 10) :background (cassette--c :groove))
                           (ui :rect :width 1 :height (cassette--px 10) :background (cassette--c :shine)))))

(defun cassette--plate (items grille edge)
  (let* ((parts (loop for (item . rest) on (remove nil items)
                      collect (ui :row :gap (cassette--px 4) :align :center :children (remove nil item))
                      when rest collect (cassette--groove)))
         (body (ui :row :height (cassette--px 16) :align :center :gap (cassette--px 6)
                   :padding (list 0 (cassette--px 6) 0 (cassette--px 6))
                   :children (append (when (member grille '(:start :both)) (list (cassette--grille)))
                                     parts
                                     (when (member grille '(:end :both)) (list (cassette--grille)))))))
    (when parts
      (let ((plate (ui :column :align :stretch :background (cassette--c :face) :border 1 :border-color (cassette--c :ink)
                       :children (list (ui :rect :height 1 :background (cassette--c :shine))
                                       body
                                       (ui :rect :height 1 :background (cassette--c :shade))))))
        (if (eq edge :top)
            (ui :column :align :stretch
                :children (list plate (ui :row :height (cassette--shadow) :padding (list 0 0 0 (cassette--shadow))
                                          :children (list (ui :rect :grow 1 :height (cassette--shadow) :background (cassette--c :shadow))))))
            plate)))))

(defun cassette--shadow () (cassette--px 2))

(defun cassette--height (edge) (+ (cassette--px 16) 4 (if (eq edge :top) (cassette--shadow) 0)))

(defun cassette--split ()
  (let ((pair (getf +cassette+ :center-between)))
    (loop for (a b) on (getf +cassette+ :modules)
          for index from 1
          when (and b (eq a (first pair)) (eq b (second pair))) return index)))
(in-package #:tomoe-user)

(defun spine--parts (text)
  (loop with start = 0
        for i = (position-if (lambda (c) (member c '(#\: #\Space))) text :start start)
        for part = (subseq text start i)
        when (plusp (length part)) collect part
        while i do (setf start (1+ i))))

(defun spine--text (text color &optional (size 11))
  (ui :text :text text :size (cassette--px size) :line-height (cassette--px size) :color color
      :font (getf +cassette+ :font)))

(defun spine--groove ()
  (ui :rect :height 1 :background (cassette--c :groove)))

(defun spine--ladder (fraction)
  (let* ((count (getf +cassette+ :segments))
         (lit (round (* count (max 0 (min 1 fraction))))))
    (ui :column :gap (cassette--px 1) :align :center
        :children (loop for i from (1- count) downto 0
                        collect (ui :rect :width (cassette--px 8) :height (cassette--px 3)
                                    :background (if (< i lit) (cassette--zone (/ (1+ i) count)) (cassette--c :led-off)))))))

(defun spine--gauge (label percent detail &optional hot)
  (let ((percent (max 0 (min 100 (round (or percent 0))))))
    (ui :column :align :center :gap (cassette--px 2)
        :children (remove nil
                          (list (spine--text label (cassette--c :dim) 7)
                                (spine--ladder (/ percent 100))
                                (ui :row :align :end
                                    :children (list (spine--text (format nil "~D" percent) (cassette--c :ink))
                                                    (spine--text "%" (cassette--c :dim) 7)))
                                (when detail (spine--text detail (if hot (cassette--c :orange) (cassette--c :dim)) 7)))))))

(defun spine--counter ()
  (ui :column :align :stretch :gap (cassette--px 1)
      :children (loop for part in (spine--parts (clock-text (getf +cassette+ :time)))
                      collect (cassette--window part :face (cassette--c :counter) :rim (cassette--c :rim)
                                                     :ghost (cassette--c :counter-ghost) :ink (cassette--c :amber)))))

(defun spine--date ()
  (destructuring-bind (&optional day date month &rest rest)
      (spine--parts (string-upcase (clock-text (getf +cassette+ :date))))
    (declare (ignore rest))
    (ui :column :align :center :gap (cassette--px 2)
        :children (remove nil
                          (list (when day (spine--text day (cassette--c :dim) 7))
                                (when date (spine--text date (cassette--c :ink)))
                                (when month (spine--text month (cassette--c :dim) 7)))))))

(defun spine--stacked (text color)
  (ui :column :align :center
      :children (loop for c across (string-right-trim " " text)
                      collect (spine--text (string c) color 8))))

(defun spine--media (snapshot)
  (let* ((service (service-state snapshot :mpris))
         (status (getf service :status))
         (title (getf service :title))
         (length (getf service :length))
         (position (getf service :position))
         (played (if (and (realp length) (plusp length) (realp position)) (max 0 (min 1 (/ position length))) 0)))
    (when (and (stringp title) (plusp (length title)) (member status '("Playing" "Paused") :test #'equal))
      (ui :column :align :center :gap (cassette--px 4)
          :children (list (cassette--led (equal status "Playing"))
                          (cassette--reel (- 1 played))
                          (spine--stacked (cassette--fit title (getf +cassette+ :spine-title)) (cassette--c :ink))
                          (cassette--reel played))))))

(defun spine--module (module state snapshot)
  (let ((cpu (getf state :cpu)) (memory (getf state :memory)) (gpu (getf state :gpu)))
    (ecase module
      (:time (spine--counter))
      (:date (spine--date))
      (:media (spine--media snapshot))
      (:cpu (spine--gauge "CPU" (getf cpu :percent) (cassette--degrees (getf cpu :temp)) (cassette--hot-p (getf cpu :temp))))
      (:memory (spine--gauge "MEM" (getf memory :percent) (cassette--gib (getf memory :used))))
      (:gpu (when (probe--gpu-p gpu)
              (spine--gauge "GPU" (getf gpu :percent) (cassette--degrees (getf gpu :temp)) (cassette--hot-p (getf gpu :temp)))))
      (:vram (when (probe--gpu-p gpu)
               (spine--gauge "VRAM" (if (plusp (or (getf gpu :total) 0)) (round (* 100 (or (getf gpu :used) 0)) (getf gpu :total)) 0)
                             (cassette--gib (getf gpu :used)))))
      (:battery (let ((battery (service-state snapshot :battery)))
                  (spine--gauge (if (getf battery :charging) "CHG" "BAT") (getf battery :percent) nil)))
      (:network (let ((online (getf (service-state snapshot :network) :connected)))
                  (ui :column :align :center :gap (cassette--px 3)
                      :children (list (cassette--led online)
                                      (spine--text (if online "NET" "OFF") (cassette--c :dim) 7)))))
      (:frame (let ((battery (state-value snapshot :frame-battery)))
                (when (getf battery :percent)
                  (spine--gauge (if (equal (getf battery :status) "Discharging") "VR" "VR+") (getf battery :percent) nil)))))))

(defun spine--cell (item)
  (ui :row :justify :center :padding (list (cassette--px 6) 0 (cassette--px 6) 0) :children (list item)))

(defun spine--run (items)
  (ui :column :height 0 :grow 1 :align :stretch :justify :center
      :children (loop for (item . rest) on (remove nil items)
                      collect (spine--cell item)
                      when rest collect (spine--groove))))

(defun spine--tray-height (tray)
  "Logical height of the tray slot that TRAY, a published :shelf-tray request
of (:count N :size PX), asks for at the top of the spine."
  (* (getf tray :count 0) (getf tray :size 0)))

(defun spine--body (items &optional (tray-height 0))
  (ui :column :align :stretch :background (cassette--c :face) :border 1 :border-color (cassette--c :rim)
      :children (remove nil
                        (list (when (plusp tray-height)
                                (ui :rect :height tray-height :background (cassette--c :counter)))
                              (spine--cell (cassette--grille))
                              (spine--run items)
                              (spine--cell (cassette--grille))))))

(defun spine--covered-p (snapshot box)
  "Whether a placed window overlaps BOX. Tiled windows never reach the seam, so
this is a float or a fullscreen window, which the spine then sits beneath."
  (loop for entry in (context snapshot :layout)
        thereis (and (getf entry :visible)
                     (< (getf entry :x) (+ (getf box :x) (getf box :width)))
                     (< (getf box :x) (+ (getf entry :x) (getf entry :width)))
                     (< (getf entry :y) (+ (getf box :y) (getf box :height)))
                     (< (getf box :y) (+ (getf entry :y) (getf entry :height))))))

(defun spine--fullscreen-p (snapshot output)
  (loop for entry in (context snapshot :layout)
        thereis (and (getf entry :visible) (getf entry :fullscreen)
                     (<= (getf entry :x) (getf output :x) (+ (getf entry :x) (getf entry :width) -1))
                     (<= (getf entry :y) (getf output :y) (+ (getf entry :y) (getf entry :height) -1)))))

(defun spine--plates (items output focal snapshot)
  (let ((split (or (cassette--split) (length items)))
        (layer (if (spine--fullscreen-p snapshot output) :bottom :top)))
    (loop for edge in '(:top :bottom)
          for plate = (cassette--plate (if (eq edge :top) (subseq items 0 split) (nthcdr split items)) :both edge)
          for height = (cassette--height edge)
          when plate
            collect (apply #'shell-surface (intern (format nil "~A-~A" edge (getf output :name)) :keyword) plate
                           :output (getf output :name) :layer layer
                           :background "#00000000" :blur t :click-through t
                           (if focal
                               (list :anchors '(:top)
                                     :margin (list (- (+ (getf focal :top) (if (eq edge :top) 0 (getf focal :height)))
                                                      (round height 2))
                                                   0 0 0))
                               (list :anchors (list edge) :margin (if (eq edge :top) '(-1 0 0 0) '(0 0 -1 0))
                                     :exclusive-zone (1- height)))))))

(define-extension "bar" (:reads (:system :services :data :outputs :layout)
                         :state (list :cpu nil :memory nil :gpu nil))
    (snapshot state event)
  (when (and (eq (getf event :type) :timer) (eq (getf event :name) :sample))
    (let ((system (context snapshot :system)))
      (dolist (probe '(:cpu :memory :gpu))
        (setf (getf state probe) (probe--sample probe (getf state probe) system)))))
  (let* ((*cassette-mode* (state-value snapshot :theme-mode :dark))
         (seam (state-value snapshot :deck-seam))
         (output (and seam (find (getf seam :output) (context snapshot :outputs)
                                 :key (lambda (o) (getf o :name)) :test #'equal)))
         (scale (and output (/ (getf output :scale-120 120) 120)))
         (narrow (remove-if-not (lambda (o) (deck--narrow-p (list :output o))) (context snapshot :outputs)))
         (focal (state-value snapshot :focal-frame))
         (tray-height (spine--tray-height (state-value snapshot :shelf-tray)))
         (items (when narrow
                  (loop for module in (getf +cassette+ :modules)
                        collect (cassette--module module state snapshot))))
         (hot (loop for probe in '(:cpu :gpu) thereis (cassette--hot-p (getf (getf state probe) :temp)))))
    (values state
            (append (list (interval :sample (getf +cassette+ :sample))
                          (publish-state :hud-hot (and hot t)))
                    (when (and output (plusp tray-height))
                      (list (publish-state :shelf-tray-rect
                                           (list :output (getf seam :output)
                                                 :x (getf seam :x) :y (+ (getf seam :y) (round scale))
                                                 :width (getf seam :width)
                                                 :height (round (* tray-height scale))))))
                    (when output
                      (list (shell-surface :spine (spine--body (loop for module in (getf +cassette+ :modules)
                                                                     collect (spine--module module state snapshot))
                                                               tray-height)
                                           :output (getf output :name) :anchors '(:top :left)
                                           :width (round (getf seam :width) scale) :height (round (getf seam :height) scale)
                                           :margin (list (round (- (getf seam :y) (getf output :y)) scale) 0 0
                                                         (round (- (getf seam :x) (getf output :x)) scale))
                                           :layer (if (spine--covered-p snapshot seam) :bottom :top)
                                           :background "#00000000" :blur t
                                           :click-through t :slide t)))
                    (loop for o in narrow
                          append (spine--plates items o (and (equal (getf focal :output) (getf o :name)) focal) snapshot)))
            nil)))

(defparameter +shader-wallpaper+ (list :|BIND| (list (list :alt :shift) "b" "shader-wallpaper-menu" :description "Choose Wallpaper") :|PREVIEW| (list :|BORDER| 4 :|GAP| 24 :|HEIGHT| 144 :|WIDTH| 256) :|SHADERS| (list (list "shaders/cassette-squares.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/cassette-led.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/hex-spiral.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/dot-corridor.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/su-ei.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/glass-mound.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/llama-herd.glsl" 240 "/run/current-system/sw/bin/true") (list "shaders/llama-herd-night.glsl" 240 "/run/current-system/sw/bin/true") (list "shaders/liquify.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/okazz-spheres.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/balatro.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/xmb.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/aurora.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/stars.glsl" 24 "/run/current-system/sw/bin/true") (list "shaders/towers.glsl" 24 "/run/current-system/sw/bin/true") (list "shaders/cubes.glsl" 30 "/run/current-system/sw/bin/true") (list "shaders/okazz-glyphs.glsl" 30 "/run/current-system/sw/bin/true"))))
(in-package #:tomoe-user)

(defun shader-wallpaper--menu (shaders chosen outputs)
  (destructuring-bind (&key width height gap border) (getf +shader-wallpaper+ :preview)
    (let ((count (length shaders)))
      (list* (shell-surface :shader-menu (ui :column :on-click :cancel :background "#00000066")
                            :anchors '(:top :right :bottom :left) :layer :overlay
                            :background "#00000000" :blur t)
             (keyboard-grab)
             (bind-key nil "Left" (list :callback "up")) (bind-key nil "h" (list :callback "up"))
             (bind-key nil "Right" (list :callback "down")) (bind-key nil "l" (list :callback "down"))
             (bind-key nil "Return" (list :callback "select"))
             (bind-key nil "Escape" (list :callback "cancel"))
             (loop for output in outputs
                   for scale = (getf output :scale-120 120)
                   for output-width = (floor (* (getf output :width) 120) scale)
                   for output-height = (floor (* (getf output :height) 120) scale)
                   for tile-width = (min width (floor (- output-width (* (1+ count) gap)) count))
                   for tile-height = (floor (* height tile-width) width)
                   for left = (floor (- output-width (- (* count (+ tile-width gap)) gap)) 2)
                   for top = (floor (- output-height tile-height) 2)
                   nconc (loop for (path fps) in shaders
                               for index from 0
                               collect (shell-surface
                                        (intern (format nil "SHADER-PREVIEW-~A-~D" (getf output :name) index)
                                                :keyword)
                                        (ui :stack :width tile-width :height tile-height
                                            :border (if (= index chosen) border 0)
                                            :border-color (theme :accent)
                                            :on-click (intern (format nil "SELECT-~D" index) :keyword)
                                            :on-hover (intern (format nil "HOVER-~D" index) :keyword))
                                        :output (getf output :name) :anchors '(:top :left)
                                        :margin (list top 0 0 (+ left (* index (+ tile-width gap))))
                                        :width tile-width :height tile-height :layer :overlay
                                        :background (list :shader path :fps fps))))))))

(define-extension "shader-wallpaper" (:reads (:command :ui :outputs) :state nil) (snapshot state event)
  (let* ((shaders (getf +shader-wallpaper+ :shaders))
         (count (length shaders)))
    (destructuring-bind (&key (index 0) menu) state
      (when (equal (getf event :owner) "shader-wallpaper")
        (if (equal (if (eq (getf event :type) :command)
                       (getf event :name)
                       (getf event :command))
                   "shader-wallpaper-menu")
            (setf menu index)
            (multiple-value-bind (action choice) (and menu (menu-choice event menu count))
              (case action
                (:select (setf index choice menu nil))
                (:cancel (setf menu nil))
                (:move (setf menu choice))))))
      (setf index (mod index count) menu (and menu (mod menu count)))
      (let ((shown (nth (or menu index) shaders))
            (command (third (nth index shaders))))
        (values (list :index index :menu menu)
                (list* (define-command "shader-wallpaper-menu"
                         :description "Choose the shader wallpaper.")
                       (apply #'bind-key (getf +shader-wallpaper+ :bind))
                       (shell-surface :shader-wallpaper (ui :stack)
                                      :anchors '(:top :right :bottom :left) :layer :background
                                      :background (list :shader (first shown) :fps (second shown)))
                       (append (when command (list (exec-async :theme command)))
                               (when menu (shader-wallpaper--menu shaders menu (context snapshot :outputs)))))
                nil)))))

(defparameter +peek+ (list :|BIND| (list (list :alt) "p" "toggle-peek" :description "Peek at the Desktop")))
(in-package #:tomoe-user)

(define-extension "peek" (:reads (:command :windows) :state nil) (snapshot hidden event)
  (let ((live (mapcar (lambda (window) (getf window :id)) (context snapshot :windows))))
    (setf hidden (cond ((and (eq (getf event :type) :command)
                             (equal (getf event :name) "toggle-peek"))
                        (unless hidden live))
                       ((subsetp live hidden) (intersection hidden live))))
    (values hidden
            (list* (define-command "toggle-peek" :description "Hide every window, or bring them back.")
                   (apply #'bind-key (getf +peek+ :bind))
                   (when hidden (cons (focus nil) (mapcar #'hide-window hidden))))
            nil)))

(defparameter +sounds+ (list :|BUTTON| "sound.wav" :|BUTTON-GAIN| -23.3d0 :|CLOSE| "sound.wav" :|CLOSE-GAIN| -6 :|KEY| (list "sound.wav" "sound.wav" "sound.wav") :|KEY-GAIN| -18))
(in-package #:tomoe-user)
(define-extension "sounds" () (snapshot state event) (declare (ignore snapshot event)) (values state (list (sound :key (getf +sounds+ :key) :gain (getf +sounds+ :key-gain)) (sound :button (getf +sounds+ :button) :gain (getf +sounds+ :button-gain)) (sound :close (getf +sounds+ :close) :gain (getf +sounds+ :close-gain))) nil))

(in-package #:tomoe-user)

(defparameter +special-bindings+ '(("grave" :term))
  "(KEY NAME [:park nil]) entries: Mod+KEY toggles special NAME, Mod+Shift+KEY parks the focused window in it or returns a parked one to the tiling. With :park nil only window rules put windows in NAME.")

(defparameter +special-commands+
  (list (define-command "toggle-special"
          :description "Show or hide a special workspace."
          :arguments '((:name :string :required t :description "Special workspace name.")))
        (define-command "park-special"
          :description "Park a window in a special workspace, or return it to the tiling."
          :arguments '((:name :string :required t :description "Special workspace name.")
                       (:window :window :description "Window to park.")))))

(define-extension "special" (:reads (:windows :rules :outputs :workareas :focus :command :button) :state nil)
    (snapshot state event)
  (let* ((ids (mapcar (lambda (window) (getf window :id)) (context snapshot :windows)))
         (spaces (loop for (name . space) in (getf state :spaces)
                       collect (list name :shown (getf space :shown)
                                      :windows (remove-if-not (lambda (id) (member id ids))
                                                              (getf space :windows)))))
         (output (first (context snapshot :outputs)))
         (area (or (find (getf output :name) (context snapshot :workareas)
                         :test #'equal :key (lambda (entry) (getf entry :name)))
                   output))
         (focused (getf state :focused))
         (name (and (eq (getf event :type) :command) (getf event :name)))
         (arguments (and (eq (getf event :type) :command) (getf event :arguments))))
    (labels ((space (name)
               (or (assoc name spaces)
                   (car (push (list name :shown nil :windows nil) spaces))))
             (present (space)
               (setf focused (car (last (getf (rest space) :windows)))))
             (parked (id)
               (some (lambda (space) (member id (getf (rest space) :windows))) spaces))
             (park (name id)
               (unless (parked id)
                 (let ((space (space name)))
                   (setf (getf (rest space) :windows) (append (getf (rest space) :windows) (list id)))
                   (if (getf (rest space) :shown) (present space) (setf focused nil)))))
             (unpark (id)
               (dolist (space spaces)
                 (setf (getf (rest space) :windows) (remove id (getf (rest space) :windows))))
               (when (eql focused id) (setf focused nil))))
      (dolist (id ids)
        (let* ((rules (rules-for snapshot id))
               (name (cdr (or (assoc :special rules) (assoc "special" rules :test #'equal)))))
          (when (stringp name) (park (intern (string-upcase name) :keyword) id))))
      (let ((target (and (equal name "park-special")
                         (or (getf arguments :window) (context snapshot :focus)))))
        (dolist (entry +special-bindings+)
          (let ((space-name (intern (string-upcase (second entry)) :keyword)))
            (when (equal (getf arguments :name) (string-downcase (second entry)))
              (cond
                ((equal name "toggle-special")
                 (let ((space (space space-name)))
                   (setf (getf (rest space) :shown) (not (getf (rest space) :shown)))
                   (if (getf (rest space) :shown) (present space) (setf focused nil))))
                ((and (equal name "park-special") (member target ids))
                 (if (parked target) (unpark target) (park space-name target))))))))
      (when (eq (getf event :type) :button)
        (when (eq (getf event :state) :pressed)
          (setf focused (find (getf event :id)
                              (loop for space in spaces when (getf (rest space) :shown)
                                    append (getf (rest space) :windows))))))
      (unless (member focused ids) (setf focused nil))
      (let ((effects nil))
        (dolist (space spaces)
          (destructuring-bind (&key shown windows) (rest space)
            (dolist (id windows)
              (if (and shown area)
                  (let ((width (floor (* (getf area :width) 3) 4))
                        (height (floor (* (getf area :height) 3) 4)))
                    (push (place id (+ (getf area :x) (floor (- (getf area :width) width) 2))
                                 (+ (getf area :y) (floor (- (getf area :height) height) 2))
                                 width height)
                          effects)
                    (push (raise-window id) effects))
                  (push (hide-window id) effects)))))
        (when focused (push (focus focused) effects))
        (push (publish-state :wm-exclude
                             (loop for space in spaces append (getf (rest space) :windows)))
              effects)
        (dolist (command +special-commands+) (push command effects))
        (loop for (key name . options) in +special-bindings+
              do (push (bind-key '(:mod) key (invoke "toggle-special" :name (string-downcase name)))
                       effects)
                 (when (getf options :park t)
                   (push (bind-key '(:mod :shift) key
                                   (invoke "park-special" :name (string-downcase name)))
                         effects)))
        (values (list :spaces spaces :focused focused) (nreverse effects) nil)))))
(setf +special-bindings+ '(("t" :taobao :park nil)))
(define-extension "baobot-chromium" (:reads ()) (snapshot state event)
  (declare (ignore snapshot event))
  (values state
          (list (window-rule :baobot-chromium :app-id "baobot-chromium" :properties '((:special . "taobao"))))
          nil))

(define-extension "cursor-trail" (:reads () :state nil) (snapshot state event)
  (declare (ignore snapshot state event))
  (values nil
          (list (shell-surface :cursor-trail (ui :stack)
                               :anchors '(:top :right :bottom :left)
                               :layer :overlay
                               :click-through t
                               :background '(:shader "shaders/trail-pixels.glsl" :fps 60 :idle 0.5)))
          nil))

(define-extension "night-light" (:reads (:command)) (snapshot state event)
  (declare (ignore snapshot))
  (values state
          (list (define-command "toggle-night-light" :description "Toggle the night light filter.")
                (bind-key (list :alt) "n" "toggle-night-light" :description "Toggle Night Light"))
          (when (and (eq (getf event :type) :command)
                     (equal (getf event :name) "toggle-night-light"))
            (list (launch "/nix/store/rym3fdk9j00isc9jfk25xk84j82wgl92-night-light-toggle")))))

(in-package #:tomoe-user)

(defun mado-menu--effects (keys)
  (append (sheet-dialog :menu (append (loop for (label description) in keys
                                            collect (list label description))
                                      '(("Esc" "Leave headset mode, as Mod+V does")))
                        :title "Headset mode" :dismiss :mado-menu-close)
          (list (bind-key nil "Escape" "mado-menu-close"))
          (loop for (nil nil . binds) in keys
                append (loop for (modifiers key command release) in binds
                             collect (apply #'bind-key modifiers key command
                                            (when release (list :release release)))))))

(in-package #:tomoe-user)

(defparameter +mado-send+ "/nix/store/3ywrcga5cklp2aj04jb4873x60z6l65q-mado-send-0.1.0/bin/mado-send")

(defparameter +mado-guard+ "/nix/store/j1rqzannnv4chcq1p0pcr92n7qhiwkp6-mado-guard/bin/mado-guard")

(defparameter +mado-look+ "/nix/store/z0xqdcslxa3fbsaz5vyfhpxcx2my2lzs-mado-look/bin/mado-look")

(defparameter +mado-monitor+ '("MADO-1" 3840 2160 144))

(defparameter +mado-screen+ "DP-4")

(defparameter +mado-bitrate+ 80)

(defparameter +mado-keys+
  '(("W A S D" "Hold to move the panel you look at around you"
     (nil "w" "mado-move-up" "mado-stop-up") (nil "a" "mado-move-left" "mado-stop-left")
     (nil "s" "mado-move-down" "mado-stop-down") (nil "d" "mado-move-right" "mado-stop-right"))
    ("← → ↑ ↓" "Hold to turn or tilt it"
     (nil "Left" "mado-move-turn-left" "mado-stop-turn-left") (nil "Right" "mado-move-turn-right" "mado-stop-turn-right")
     (nil "Up" "mado-move-tilt-up" "mado-stop-tilt-up") (nil "Down" "mado-move-tilt-down" "mado-stop-tilt-down"))
    ("- =" "Hold to push it away or pull it near"
     (nil "minus" "mado-move-farther" "mado-stop-farther") (nil "equal" "mado-move-nearer" "mado-stop-nearer"))
    ("Q E" "Hold to rotate it"
     (nil "q" "mado-move-rotate-left" "mado-stop-rotate-left") (nil "e" "mado-move-rotate-right" "mado-stop-rotate-right"))
    ("[ ]" "Hold to shrink or grow it"
     (nil "bracketleft" "mado-move-smaller" "mado-stop-smaller") (nil "bracketright" "mado-move-bigger" "mado-stop-bigger"))
    ("G" "Hold to carry a panel with your head" (nil "g" "mado-grab" "mado-release"))
    ("C" "Centre the headset panels and reset them" (nil "c" "mado-home"))
    ("Shift C" "Make the main panel follow your head, or set it down" ((:shift) "c" "mado-follow"))
    ("O" "Curve or flatten the panels" (nil "o" "mado-curve"))
    ("M" "Use the headset as the only monitor, or bring the physical one back" (nil "m" "mado-solo"))
    ("X" "Show or hide the SteamVR dashboard" (nil "x" "mado-dashboard"))
    ("P" "Switch what's behind the panels: a colour or the SteamVR environment" (nil "p" "mado-backdrop"))
    ("B" "Switch the panel background between black and white" (nil "b" "mado-background"))
    ("H" "Show or hide the visor: clock, battery, link and frame rate" (nil "h" "mado-hud"))
    ("R" "Show or hide the room around you" (nil "r" "mado-passthrough"))
    ("2 3" "Toggle still-screen refresh and the gaze region"
     (nil "2" "mado-still") (nil "3" "mado-gaze"))))

(defun mado--solo (solo)
  (when solo
    (destructuring-bind (name width height hz) +mado-monitor+
      (list (virtual-output name :mode (list width height hz))
            (configure-output +mado-screen+ :disabled t)
            (service :guard (list +mado-guard+))
            (bind-key '(:alt) "space" "mado-look" :description "Bring the window you look at to the centre")))))

(defparameter +mado-actions+
  '("menu" "menu-close" "look" "home" "grab" "release" "follow" "dashboard" "curve" "solo"
    "backdrop" "background" "screen" "still" "gaze" "hud" "passthrough"
    "move-up" "move-down" "move-left" "move-right" "move-turn-left" "move-turn-right"
    "move-tilt-up" "move-tilt-down" "move-farther" "move-nearer" "move-rotate-left"
    "move-rotate-right" "move-smaller" "move-bigger")
  "Headset-mode actions. Each is a command prefixed with MADO-, and each held action
also has a MADO-STOP- command that releases it.")

(defparameter +mado-holds+
  '("move-up" "move-down" "move-left" "move-right" "move-turn-left" "move-turn-right"
    "move-tilt-up" "move-tilt-down" "move-farther" "move-nearer" "move-rotate-left"
    "move-rotate-right" "move-smaller" "move-bigger"))

(defun mado--command (action)
  "The command name an ACTION runs."
  (concatenate 'string "mado-" action))

(defparameter +mado-commands+
  (loop for action in +mado-actions+
        append (list (define-command (mado--command action)
                        :description (format nil "Headset mode: ~A." action)))
        when (member action +mado-holds+ :test #'equal)
          append (list (define-command (mado--command (concatenate 'string "stop" (subseq action 4)))
                          :description (format nil "Headset mode: stop ~A." (subseq action 5))))))

(defun mado--action (name)
  "The headset action a command NAME carries, or NIL for another extension's."
  (and (stringp name)
       (> (length name) 5)
       (string= "mado-" name :end2 5)
       (subseq name 5)))

(define-extension "mado" (:reads (:command :ui :data) :state '(:flat nil :solo nil :mode nil :backdrop 0)) (snapshot state event)
  (let* ((command (and (equal (getf event :owner) "mado")
                       (mado--action (if (eq (getf event :type) :command)
                                         (getf event :name)
                                         (getf event :command)))))
         (flat (getf state :flat))
         (solo (getf state :solo))
         (backdrop (getf state :backdrop))
         (theme (state-value snapshot :theme-mode :dark))
         (theme-changed (not (eq theme (getf state :theme))))
         (colour (if theme-changed (if (eq theme :light) "ffffff" "black") (getf state :colour)))
         (initial-colour (or (getf state :initial-colour) colour))
         (mode (cond ((equal command "menu") (not (getf state :mode)))
                     ((equal command "menu-close") nil)
                     (t (getf state :mode)))))
    (cond ((equal command "curve") (setf flat (not flat)))
          ((equal command "solo") (setf solo (not solo)))
          ((equal command "backdrop") (setf backdrop (mod (1+ backdrop) 2)))
          ((equal command "background")
           (setf colour (if (equal colour "black") "ffffff" "black") backdrop 0))
          ((equal command "screen") (setf solo nil)))
    (values (list :flat flat :solo solo :mode mode :backdrop backdrop
                  :theme theme :colour colour :initial-colour initial-colour)
            (append +mado-commands+
                    (list (service :send (list +mado-send+ "--output" (first +mado-monitor+) "--output" +mado-screen+
                                               "--ppd" "40" "--fps" (write-to-string (fourth +mado-monitor+))
                                               "--bitrate" (write-to-string +mado-bitrate+)
                                               "--listen" "0.0.0.0:9757" "--look" "--backdrop" initial-colour))
                          (bind-key '(:mod) "v" "mado-menu" :description "Enter or leave headset mode"))
                    (when (getf state :theme)
                      (list (exec-async :backdrop (format nil "~A ctl backdrop ~A" +mado-send+
                                                          (if (zerop backdrop) colour "environment")))))
                    (when mode (mado-menu--effects +mado-keys+))
                    (mado--solo solo))
            (cond ((equal command "look") (list (launch +mado-look+)))
                  ((member command '("home" "grab" "release" "follow" "dashboard") :test #'equal)
                   (list (launch +mado-send+ "ctl" command)))
                  ((and (stringp command) (> (length command) 5)
                        (member (subseq command 0 5) '("move-" "stop-") :test #'equal))
                   (list (launch +mado-send+ "ctl" (subseq command 0 4) (subseq command 5))))
                  ((and (member command '("menu" "menu-close") :test #'equal) (not mode))
                   (list (launch +mado-send+ "ctl" "stop")))
                  ((equal command "curve") (list (launch +mado-send+ "ctl" "curve" (if flat "flat" "eye"))))
                  ((member command '("still" "gaze" "hud" "passthrough") :test #'equal)
                   (list (launch +mado-send+ "ctl" command "toggle")))))))

(in-package #:tomoe-user)

(defparameter +cross-output+ "MADO-1")

(defparameter +cross-slot+ '(1920 1080))

(defparameter +cross-tile+ '(960 540))

(defparameter +cross-gap+ 8)

(defparameter +cross-bindings+
  '(((:alt) "h" "left" "Focus the window to the left")
    ((:alt) "l" "right" "Focus the window to the right")
    ((:alt) "k" "up" "Focus the workspace above")
    ((:alt) "j" "down" "Focus the workspace below")
    ((:alt :shift) "h" "carry-left" "Carry the window left")
    ((:alt :shift) "l" "carry-right" "Carry the window right")
    ((:alt :shift) "k" "carry-up" "Carry the window to the workspace above")
    ((:alt :shift) "j" "carry-down" "Carry the window to the workspace below")))

(defun cross--row (ids mark)
  (list :ids ids :mark mark))

(defun cross--unmanaged-p (snapshot id)
  (let ((properties (rules-for snapshot id)))
    (or (cdr (assoc :float properties)) (cdr (assoc :hidden properties)))))

(defun cross--area (snapshot)
  (find +cross-output+ (context snapshot :workareas) :test #'equal
        :key (lambda (area) (getf area :name))))

(defun cross--prune (row keep)
  (let* ((ids (getf row :ids))
         (mark (getf row :mark))
         (index (or (position mark ids) 0))
         (kept (remove-if-not (lambda (id) (member id keep)) ids)))
    (cross--row kept
                (cond ((member mark kept) mark)
                      ((find-if (lambda (id) (member id kept)) ids :start (min (1+ index) (length ids))))
                      (t (find-if (lambda (id) (member id kept)) ids :end (min index (length ids)) :from-end t))))))

(defun cross--normalize (rows row)
  (let ((kept nil) (focal 0))
    (loop for r in rows
          for i from 0
          when (or (getf r :ids) (= i row))
            do (when (= i row) (setf focal (length kept)))
               (push r kept))
    (setf kept (nreverse kept))
    (unless (and kept (null (getf (first kept) :ids)))
      (push (cross--row nil nil) kept)
      (incf focal))
    (when (getf (car (last kept)) :ids)
      (setf kept (append kept (list (cross--row nil nil)))))
    (values kept (min focal (1- (length kept))))))

(defun cross--insert (row id)
  (let* ((ids (getf row :ids))
         (index (position (getf row :mark) ids))
         (at (if index (1+ index) (length ids))))
    (cross--row (append (subseq ids 0 at) (list id) (nthcdr at ids)) id)))

(defun cross--replace (rows row new)
  (loop for r in rows for i from 0 collect (if (= i row) new r)))

(defun cross--step (row delta)
  (let* ((ids (getf row :ids))
         (index (position (getf row :mark) ids)))
    (if index
        (cross--row ids (nth (max 0 (min (1- (length ids)) (+ index delta))) ids))
        row)))

(defun cross--swap (row delta)
  (let* ((ids (copy-list (getf row :ids)))
         (mark (getf row :mark))
         (index (position mark ids))
         (other (and index (+ index delta))))
    (when (and other (< -1 other (length ids)))
      (rotatef (nth index ids) (nth other ids)))
    (cross--row ids mark)))

(defun cross--carry (rows row delta)
  (let ((target (+ row delta))
        (id (getf (nth row rows) :mark)))
    (if (or (null id) (< target 0) (>= target (length rows)))
        (values rows row)
        (cross--normalize
         (loop for r in rows
               for i from 0
               collect (cond ((= i row) (cross--prune r (remove id (getf r :ids))))
                             ((= i target) (cross--insert r id))
                             (t r)))
         target))))

(defun cross--slot (dx dy area)
  (destructuring-bind (tw th) +cross-tile+
    (let* ((mw (min (first +cross-slot+) (getf area :width)))
           (mh (min (second +cross-slot+) (getf area :height)))
           (left (+ (getf area :x) (floor (- (getf area :width) mw) 2)))
           (top (+ (getf area :y) (floor (- (getf area :height) mh) 2)))
           (beside (if (minusp dx) (+ left (* dx tw)) (+ left mw (* (1- dx) tw))))
           (row (if (minusp dy) (+ top (* dy th)) (+ top mh (* (1- dy) th)))))
      (cond ((and (zerop dx) (zerop dy)) (list left top mw mh))
            ((zerop dy) (list beside (+ top (floor (- mh th) 2)) tw th))
            ((> (abs dx) 1) (list beside row tw th))
            (t (list (+ left (floor (- mw tw) 2) (* dx tw)) row tw th))))))

(defun cross--places (snapshot rows row area focused)
  (let ((windows (context snapshot :windows))
        (output (find +cross-output+ (context snapshot :outputs) :test #'equal
                      :key (lambda (o) (getf o :name)))))
    (loop for r in rows
          for dy from (- row)
          for ids = (getf r :ids)
          append (loop for id in ids
                       for dx from (- (or (position (getf r :mark) ids) 0))
                       for window = (find id windows :key (lambda (w) (getf w :id)))
                       for full = (and output (eql id focused) (getf window :fullscreen-requested))
                       append (list (fullscreen id (and full t))
                                    (if full
                                        (place id (getf output :x) (getf output :y)
                                               (getf output :width) (getf output :height))
                                        (destructuring-bind (x y w h) (cross--slot dx dy area)
                                          (place id (+ x +cross-gap+) (+ y +cross-gap+)
                                                 (max 1 (- w (* 2 +cross-gap+)))
                                                 (max 1 (- h (* 2 +cross-gap+)))))))
                       when full collect (raise-window id)))))

(defun cross--at (rows row area x y)
  (loop for r in rows
        for dy from (- row)
        for ids = (getf r :ids)
        thereis (loop for id in ids
                      for dx from (- (or (position (getf r :mark) ids) 0))
                      thereis (destructuring-bind (sx sy w h) (cross--slot dx dy area)
                                (and (<= sx x) (< x (+ sx w)) (<= sy y) (< y (+ sy h)) id)))))

(defun cross--look (snapshot rows row area focused params)
  (let ((output (find (json-get params "output" nil) (context snapshot :outputs) :test #'equal
                      :key (lambda (o) (getf o :name))))
        (window (find focused (context snapshot :windows) :key (lambda (w) (getf w :id))))
        (x (json-get params "x" nil))
        (y (json-get params "y" nil)))
    (when (and output (equal (getf output :name) +cross-output+) (realp x) (realp y))
      (if (getf window :fullscreen-requested)
          focused
          (cross--at rows row area (+ (getf output :x) x) (+ (getf output :y) y))))))

(defun cross--frame (snapshot area)
  (let ((output (find +cross-output+ (context snapshot :outputs) :test #'equal
                      :key (lambda (o) (getf o :name)))))
    (when output
      (destructuring-bind (x y w h) (cross--slot 0 0 area)
        (flet ((logical (n) (round (* n 120) (getf output :scale-120 120))))
          (list :output +cross-output+
                :left (logical (- x (getf output :x))) :top (logical (- y (getf output :y)))
                :width (logical w) :height (logical h)))))))

(defparameter +cross-commands+
  (list (define-command "left" :description "Focus the window to the left.")
        (define-command "right" :description "Focus the window to the right.")
        (define-command "up" :description "Focus the workspace above.")
        (define-command "down" :description "Focus the workspace below.")
        (define-command "carry-left" :description "Carry the window left.")
        (define-command "carry-right" :description "Carry the window right.")
        (define-command "carry-up" :description "Carry the window to the workspace above.")
        (define-command "carry-down" :description "Carry the window to the workspace below.")))

(define-extension "cross"
    (:reads (:windows :rules :data :workareas :outputs :focus :command :button :request :ipc)
     :state (list :rows nil :row 0 :float nil :input nil)
     :admission t)
    (snapshot state event)
  (let* ((area (cross--area snapshot))
         (excluded (state-value snapshot :wm-exclude))
         (live (remove-if (lambda (id) (member id excluded))
                          (mapcar (lambda (window) (getf window :id)) (context snapshot :windows))))
         (managed (remove-if (lambda (id) (cross--unmanaged-p snapshot id)) live))
         (type (getf event :type))
         (id (getf event :id))
         (command (and (eq type :command) (equal (getf event :owner) "cross")
                       (getf event :name)))
         (look (and (eq type :ipc) (equal (getf event :command) "look")))
         (seen nil)
         (rows (getf state :rows))
         (row (getf state :row))
         (float (getf state :float))
         (input (getf state :input)))
    (if rows
        (setf rows (mapcar (lambda (r) (cross--prune r managed)) rows))
        (let ((focused (previous-context snapshot :focus)))
          (setf rows (list (cross--row managed (if (member focused managed) focused (first managed))))
                row 0)))
    (multiple-value-setq (rows row) (cross--normalize rows (min row (1- (length rows)))))
    (let ((known (loop for r in rows append (getf r :ids))))
      (dolist (new managed)
        (unless (member new known)
          (setf rows (cross--replace rows row (cross--insert (nth row rows) new))
                input nil))))
    (multiple-value-setq (rows row) (cross--normalize rows row))
    (unless (member float live) (setf float nil))
    (unless (member input managed) (setf input nil))
    (when (and (eq type :map) (member id live) (not (member id managed)))
      (setf float id))
    (when (and (eq type :button) (eq (getf event :state) :pressed) (member id live))
      (if (member id managed)
          (setf input id float nil)
          (setf float id)))
    (when (and (integerp id)
               (eq type :request) (eq (getf event :request) :activate))
      (let ((target (position-if (lambda (r) (member id (getf r :ids))) rows)))
        (cond (target
               (multiple-value-setq (rows row)
                 (cross--normalize (cross--replace rows target (cross--row (getf (nth target rows) :ids) id))
                                   target))
               (setf float nil input nil))
              ((member id live) (setf float id)))))
    (when (and look area)
      (setf seen (cross--look snapshot rows row area (unless float (or input (getf (nth row rows) :mark)))
                              (getf event :params)))
      (let ((target (and seen (position-if (lambda (r) (member seen (getf r :ids))) rows))))
        (when target
          (multiple-value-setq (rows row)
            (cross--normalize (cross--replace rows target (cross--row (getf (nth target rows) :ids) seen)) target))
          (setf float nil input nil))))
    (when command
      (setf float nil input nil)
      (let ((focal (nth row rows)))
        (cond ((equal command "left") (setf rows (cross--replace rows row (cross--step focal -1))))
              ((equal command "right") (setf rows (cross--replace rows row (cross--step focal 1))))
              ((equal command "carry-left") (setf rows (cross--replace rows row (cross--swap focal -1))))
              ((equal command "carry-right") (setf rows (cross--replace rows row (cross--swap focal 1))))
              ((equal command "up")
               (multiple-value-setq (rows row) (cross--normalize rows (max 0 (1- row)))))
              ((equal command "down")
               (multiple-value-setq (rows row) (cross--normalize rows (min (1- (length rows)) (1+ row)))))
              ((equal command "carry-up") (multiple-value-setq (rows row) (cross--carry rows row -1)))
              ((equal command "carry-down") (multiple-value-setq (rows row) (cross--carry rows row 1))))))
    (let ((mark (getf (nth row rows) :mark)))
      (when (eql input mark) (setf input nil))
      (values (list :rows rows :row row :float float :input input)
              (when area
                (let ((frame (cross--frame snapshot area)))
                  (append +cross-commands+
                          (loop for (modifiers key name description) in +cross-bindings+
                                collect (bind-key modifiers key name :description description))
                          (cross--places snapshot rows row area (unless float (or input mark)))
                          (when frame (list (publish-state :focal-frame frame)))
                          (list (serve-method "cross_look" :look)
                                (if float (focus float) (focus (or input mark) :raise nil))))))
              (when look (list (ipc-reply seen)))))))

(in-package #:tomoe-user)

(defparameter +frame-battery-output+ "MADO-1")

(defparameter +frame-battery-every+ 60000)

(defparameter +frame-battery-command+
  "ssh -o BatchMode=yes -o ConnectTimeout=3 steam-frame 'for d in /sys/class/power_supply/*; do [ \"$(cat $d/type)\" = Battery ] && exec cat $d/capacity $d/status; done; exit 1'")

(defun frame-battery--parse (text)
  (let* ((newline (position #\Newline text))
         (percent (parse-integer text :end newline :junk-allowed t)))
    (when percent
      (list :percent (max 0 (min 100 percent))
            :status (if newline (string-trim '(#\Space #\Return) (subseq text (1+ newline))) "")))))

(define-extension "frame-battery" (:reads (:outputs) :state (list :tick 0 :battery nil))
    (snapshot state event)
  (let ((headset (find +frame-battery-output+ (context snapshot :outputs) :test #'equal
                       :key (lambda (o) (getf o :name))))
        (tick (getf state :tick 0))
        (battery (getf state :battery)))
    (when (eq (getf event :name) :poll)
      (case (getf event :type)
        (:timer (incf tick))
        (:exec (setf battery (and (eql (getf event :code) 0) (stringp (getf event :stdout))
                                  (frame-battery--parse (getf event :stdout)))))))
    (unless headset (setf battery nil))
    (values (list :tick tick :battery battery)
            (when headset
              (append (list (interval :poll +frame-battery-every+)
                            (exec-async :poll (format nil "~A # ~D" +frame-battery-command+ tick) :timeout 10000))
                      (when battery (list (publish-state :frame-battery battery)))))
            nil)))
(setf +mado-bitrate+ 200)
(setf +mado-keys+
      (append '(("D S C F" "Hold to move the panel you look at around you"
                 (nil "d" "mado-move-up" "mado-stop-up") (nil "s" "mado-move-left" "mado-stop-left")
                 (nil "c" "mado-move-down" "mado-stop-down") (nil "f" "mado-move-right" "mado-stop-right"))
                ("R" "Centre the headset panels" (nil "r" "mado-home"))
                ("Shift R" "Make the main panel follow your head, or set it down" ((:shift) "r" "mado-follow"))
                ("T" "Show or hide the room around you" (nil "t" "mado-passthrough")))
              (remove-if (lambda (group)
                           (some (lambda (bind) (member (third bind) '("mado-move-up" "mado-home" "mado-follow" "mado-passthrough") :test #'equal))
                                 (cddr group)))
                         +mado-keys+)))

(define-extension "bolo" (:reads (:command)) (snapshot state event)
  (declare (ignore snapshot))
  (values state
          (list (define-command "toggle-bolo"
                  :description "Toggle speech-to-text (bolo).")
                (bind-key (list :alt) "m" "toggle-bolo"
                          :description "Toggle speech-to-text (bolo)"))
          (when (and (eq (getf event :type) :command)
                     (equal (getf event :name) "toggle-bolo"))
            (list (launch "/nix/store/j0lf931vacy0skzkczajab0xmvf6qpvn-bolo-0.1.0/bin/bolo")))))

;; The daemon writes its state file on every transition; this watches it
;; and shows a pill while a turn is in flight.
;; Centring has to be a margin: opposing anchors stretch a layer
;; surface, so the offset comes from the output's logical width.
(defun bolo--margin (outputs width)
  (let* ((out (and (listp outputs) (car outputs)))
         (scale (if (and (listp out) (integerp (getf out :scale-120)))
                    (/ (getf out :scale-120) 120)
                    1))
         (logical (if (and (listp out) (integerp (getf out :width)))
                      (/ (getf out :width) scale)
                      width)))
    ;; Margin reads top, right, bottom, left.
    (list 44 0 0 (max 0 (round (- (/ logical 2) (/ width 2)))))))

(define-extension "bolo-state" (:reads (:outputs) :state (list :state "idle"))
    (snapshot state event)
  (let ((current (getf state :state)))
    (when (eq (getf event :type) :watch)
      (setf current (or (getf event :content) "idle")))
    (when (string= current "") (setf current "idle"))
    (values (list :state current)
            (append
              (list (watch-file :state "/run/user/1001/bolo/state"))
              (unless (string= current "idle")
                (list (shell-surface :bolo
                      (ui :row :padding '(5 10 5 10) :gap 4
                          :background "#1e1e2e"
                          :children
                          (list (ui :icon
                                    :name (cond ((string= current "listening") "audio-input-microphone")
                                                ((string= current "transcribing") "process-working")
                                                (t "media-record"))
                                    :size 18
                                    :color (if (string= current "recording") "#f38ba8" "#cdd6f4"))
                                (ui :text :text current :size 12 :color "#cdd6f4")))
                        :width 148 :height 28 :anchors '(:top :left)
                        :margin (bolo--margin (context snapshot :outputs) 148)
                        :layer :overlay :exclusive-zone 0
                        :background "#00000000"))))
            nil)))
