(in-package #:tomoe-user)


(defun hotkey-label (binding)
  "Old overlay spelling: \"Mod + Shift + /\"."
  (let ((declared (getf binding :declared)) (key (getf binding :keysym)))
    (format nil "~{~A~^ + ~}"
            (append (loop for (bit name) in '((128 "Mod") (64 "Super") (4 "Ctrl") (8 "Alt") (1 "Shift"))
                          when (logtest bit declared) collect name)
                    (list (cond ((equal key "Return") "Enter") ((equal key "slash") "/")
                                ((equal key "comma") ",") ((equal key "period") ".")
                                ((equal key "space") "Space") ((equal key "minus") "-")
                                ((equal key "equal") "=")
                                (t (if (plusp (length key))
                                       (concatenate 'string (string-upcase (subseq key 0 1))
                                                    (subseq key 1))
                                       key))))))))

(defparameter +hotkey-order+
  '("Run an Application" "Close Window" "Toggle Fullscreen" "Exit"
    "Show Important Hotkeys" "Focus Next Window" "Focus Previous Window"
    "Switch to Workspace 1-9" "Move Window to Workspace 1-9")
  "The old init.lua declaration order of the shipped bindings, for the hotkey sheet.")

(defparameter +session-commands+
  (list (define-command "launch"
          :description "Run a program and its arguments."
          :arguments '((:argv :argv :required t :description "Program and arguments.")))
        (define-command "reload"
          :description "Reload every extension source.")
        (define-command "quit"
          :description "Stop the session."
          :arguments '((:confirm :boolean :description "Ask before quitting.")))
        (define-command "toggle-hotkeys"
          :description "Show or hide the sheet of bound hotkeys.")))

(defun hotkey-rows (bindings)
  "The described BINDINGS as hotkey sheet rows, in +HOTKEY-ORDER+, then declaration order."
  (loop for binding in (stable-sort (sort (copy-list bindings) #'< :key (lambda (b) (getf b :order)))
                                    #'< :key (lambda (b)
                                               (or (position (getf b :description) +hotkey-order+
                                                             :test #'equal)
                                                   (length +hotkey-order+))))
        when (getf binding :description)
          collect (list (hotkey-label binding) (getf binding :description))))

(define-extension "commands" (:reads (:command :ui :focus :bindings :settings :data)
                               :state '(:exit nil :overlay nil))
    (snapshot state event)
  (let* ((command (and (equal (getf event :owner) "commands")
                       (if (eq (getf event :type) :command)
                           (getf event :name)
                           (getf event :command))))
         (arguments (and (eq (getf event :type) :command) (getf event :arguments)))
         (exit (cond ((equal command "quit") t)
                     ((member command '("exit-confirm" "exit-cancel") :test #'equal) nil)
                     (t (getf state :exit))))
         (overlay (cond ((equal command "toggle-hotkeys") (not (getf state :overlay)))
                        ((equal command "hotkeys-dismiss") nil)
                        (t (getf state :overlay)))))
    (values (list :exit exit :overlay overlay)
            (append +session-commands+
                    (list (bind-key '(:mod) "q" "close-window" :description "Close Window")
                          (bind-key '(:mod :shift) "e" (invoke "quit" :confirm t)
                                    :description "Exit")
                          (bind-key '(:mod :shift) "slash" "toggle-hotkeys"
                                    :description "Show Important Hotkeys"))
                    (when exit
                      (confirm-dialog :exit "Are you sure you want to exit tomoe?"
                                      :confirm :exit-confirm :cancel :exit-cancel
                                      :theme (state-value snapshot :theme)))
                    (when (and overlay (not exit))
                      (sheet-dialog :hotkeys (hotkey-rows (context snapshot :bindings))
                                    :title "Important Hotkeys" :dismiss :hotkeys-dismiss
                                    :theme (state-value snapshot :theme))))
            (cond ((equal command "launch") (list (apply #'launch (getf arguments :argv))))
                  ((equal command "reload") (list (reload)))
                  ((and (equal command "quit") (not (getf arguments :confirm))) (list (quit)))
                  ((equal command "exit-confirm") (list (quit)))))))

(defun wm-commands (count)
  "The window manager's commands. COUNT is the live workspace count."
  (list (define-command "focus-window"
          :description "Focus a window, switching to its workspace."
          :arguments '((:window :window :description "Window to focus.")))
        (define-command "focus-next-window"
          :description "Focus the next window on the active workspace.")
        (define-command "focus-previous-window"
          :description "Focus the previous window on the active workspace.")
        (define-command "close-window"
          :description "Ask a window to close."
          :arguments '((:window :window :description "Window to close.")))
        (define-command "toggle-fullscreen"
          :description "Toggle a window between fullscreen and tiled."
          :arguments '((:window :window :description "Window to toggle.")))
        (define-command "switch-workspace"
          :description "Show another workspace."
          :arguments (list (list :workspace :workspace :required t :maximum count
                                 :description "Workspace to show.")))
        (define-command "move-window-to-workspace"
          :description "Send a window to another workspace."
          :arguments (list (list :workspace :workspace :required t :maximum count
                                 :description "Workspace to send it to.")
                           (list :window :window :description "Window to send.")))
        (define-command "move-window"
          :description "Move a window to a physical position."
          :arguments '((:window :window :description "Window to move.")
                       (:x :integer :required t :description "Left edge.")
                       (:y :integer :required t :description "Top edge.")))
        (define-command "resize-window"
          :description "Resize a window."
          :arguments '((:window :window :description "Window to resize.")
                       (:width :integer :required t :description "New width.")
                       (:height :integer :required t :description "New height.")))))

(define-extension "wm"
    (:reads (:windows :rules :data :outputs :workareas :layout :focus :button :pointer :command
             :request :settings)
     :state nil :admission t)
    (snapshot state event)
  (let* ((excluded (state-value snapshot :wm-exclude))
         (windows (remove-if (lambda (window) (member (getf window :id) excluded))
                             (context snapshot :windows)))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (settings (state-value snapshot :wm-settings))
         (gap (getf settings :gaps 8))
         (count (getf settings :workspace-count 9))
         (honor (getf settings :honor-client-fullscreen nil))
         (outputs (context snapshot :outputs))
         (workareas (context snapshot :workareas))
         (output (first outputs))
         (area (or (find (getf output :name) workareas :test #'equal
                                                       :key (lambda (entry) (getf entry :name)))
                   output))
         (layout (context snapshot :layout))
         (input-layout (previous-context snapshot :layout))
         (old-ids (getf state :ids))
         (removed (set-difference old-ids ids))
         (new-ids (set-difference ids old-ids))
         (active (getf state :active 1))
         (workspaces nil)
         (full (remove-if-not (lambda (entry) (member (getf entry :id) ids))
                              (getf state :fullscreen)))
         (boxes (remove-if-not (lambda (entry) (member (getf entry :id) ids))
                               (getf state :boxes)))
         (stack (remove-if-not (lambda (id) (member id ids)) (getf state :stacking)))
         (visible (remove-if-not (lambda (id) (member id ids)) (getf state :visible)))
         (focused (getf state :focused))
         (type (getf event :type))
         (geometry-key (list outputs workareas gap count))
         (retired-full nil)
         (commands nil))
    (unless (and (integerp gap) (<= 0 gap 16384)) (error "Invalid gap: ~S" gap))
    (unless (and (integerp count) (<= 1 count 128)) (error "Invalid workspace count: ~S" count))
    (unless (or (null honor) (eq honor t)) (error "Invalid honor-client-fullscreen: ~S" honor))
    (setf active (max 1 (min count active))
          workspaces
          (loop for index below count
                collect (remove-if-not (lambda (id) (member id ids))
                                       (nth index (getf state :workspaces)))))
    (labels ((workspace (number) (nth (1- number) workspaces))
             (append-window (number id)
               (setf (nth (1- number) workspaces)
                     (append (workspace number) (list id))))
             (window-box (id) (find id boxes :key (lambda (entry) (getf entry :id))))
             (full-box (id) (find id full :key (lambda (entry) (getf entry :id))))
             (remember-box (id x y width height)
               (let ((box (list :id id :x x :y y :width (max 1 width) :height (max 1 height))))
                 (setf boxes (cons box (remove id boxes :key (lambda (entry) (getf entry :id)))))
                 box))
             (raise-id (id)
               (when (member id visible)
                 (setf stack (append (remove id stack) (list id)))))
             (show-id (id)
               (unless (member id visible)
                 (push id visible)
                 (setf stack (append (remove id stack) (list id)))))
             (hide-id (id)
               (setf visible (remove id visible) stack (remove id stack)))
             (focus-id (id)
               (setf focused id)
               (when id (raise-id id)))
             (current-focus ()
               (let ((previous (previous-context snapshot :focus)))
                 (if (not (eql previous (getf state :focused)))
                     (when (member previous ids) previous)
                     (when (member focused ids) focused))))
             (properties (id) (rules-for snapshot id))
             (property (properties name)
               (let ((entry (or (assoc name properties)
                                (assoc (string-downcase (symbol-name name)) properties :test #'equal))))
                 (values (cdr entry) (not (null entry)))))
             (output-for (id name)
               (or (and name (find name outputs :test #'equal
                                               :key (lambda (entry) (getf entry :name))))
                   (let ((box (and (member id old-ids)
                                   (find id input-layout :key (lambda (entry) (getf entry :id))))))
                     (when (and box (getf box :visible))
                       (let ((x (+ (getf box :x) (/ (getf box :width) 2)))
                             (y (+ (getf box :y) (/ (getf box :height) 2))))
                         (find-if (lambda (entry)
                                    (and (<= (getf entry :x) x)
                                         (< x (+ (getf entry :x) (getf entry :width)))
                                         (<= (getf entry :y) y)
                                         (< y (+ (getf entry :y) (getf entry :height)))))
                                  outputs))))
                   output))
             (arrange ()
               (if (null area)
                   (setf visible nil stack nil)
                   (let* ((tiled (remove-if #'full-box (workspace active)))
                          (x (+ (getf area :x) gap)) (y (+ (getf area :y) gap))
                          (width (- (getf area :width) (* 2 gap)))
                          (height (- (getf area :height) (* 2 gap))))
                     (loop for (id . rest) on tiled do
                       (cond ((null rest) (remember-box id x y width height))
                             ((>= width height)
                              (let ((half (floor (- width gap) 2)))
                                (remember-box id x y half height)
                                (incf x (+ half gap)) (decf width (+ half gap))))
                             (t
                              (let ((half (floor (- height gap) 2)))
                                (remember-box id x y width half)
                                (incf y (+ half gap)) (decf height (+ half gap)))))
                       (show-id id))
                     (dolist (id (workspace active))
                       (when (full-box id) (show-id id) (raise-id id))))))
             (set-fullscreen (id on &optional name)
               (if on
                   (let ((target (output-for id name)))
                     (unless target (return-from set-fullscreen nil))
                     (let ((box (remember-box id (getf target :x) (getf target :y)
                                              (getf target :width) (getf target :height))))
                       (setf full (cons (copy-list box)
                                        (remove id full :key (lambda (entry) (getf entry :id))))))
                     (show-id id)
                     (focus-id id))
                   (setf full (remove id full :key (lambda (entry) (getf entry :id)))))
               (arrange))
             (switch-workspace (number)
               (unless (and (<= 1 number count) (/= number active))
                 (return-from switch-workspace nil))
               (dolist (id (workspace active)) (hide-id id))
               (setf active number)
               (arrange)
               (focus-id (car (last (workspace active)))))
             (move-window (number id)
               (when (and id (member id ids) (<= 1 number count) (/= number active))
                 (setf (nth (1- active) workspaces) (remove id (workspace active)))
                 (hide-id id)
                 (append-window number id)
                 (arrange)
                 (focus-id (car (last (workspace active))))))
             (reveal (id)
               (let ((number (loop for wins in workspaces for index from 1
                                   when (member id wins) return index)))
                 (when number
                   (unless (= number active) (switch-workspace number))
                   (focus-id id))))
             (move-box (id x y width height)
               (let ((box (and id (window-box id))))
                 (when box
                   (remember-box id (or x (getf box :x)) (or y (getf box :y))
                                 (min 16384 (max 1 (or width (getf box :width))))
                                 (min 16384 (max 1 (or height (getf box :height))))))))
             (cycle-focus (direction)
               (let* ((wins (workspace active))
                      (index (or (position (current-focus) wins) 0)))
                 (when wins (focus-id (nth (mod (+ index direction) (length wins)) wins))))))
      (dolist (id ids)
        (unless (or (member id new-ids) (some (lambda (wins) (member id wins)) workspaces))
          (setf full (remove id full :key (lambda (entry) (getf entry :id))))
          (append-window active id)))
      (unless (and (eq type :command) (equal "toggle-fullscreen" (getf event :name)))
        (setf full
              (remove-if (lambda (entry)
                           (let ((resolved (find (getf entry :id) layout
                                                 :key (lambda (window) (getf window :id)))))
                             (when (and resolved (not (getf resolved :fullscreen)))
                               (setf retired-full t))))
                         full)))
      (unless (member focused ids) (setf focused nil))
      (when (or removed retired-full (eq type :mount)
                (not (equal geometry-key (getf state :geometry-key))))
        (loop for wins in workspaces for number from 1
              unless (= number active) do (dolist (id wins) (hide-id id)))
        (arrange))
      (when (and removed (workspace active)) (focus-id (car (last (workspace active)))))
      (dolist (window windows)
        (let ((id (getf window :id)))
          (when (member id new-ids)
            (let* ((props (properties id))
                   (requested-workspace (property props :workspace))
                   (number (and (realp requested-workspace) (floor requested-workspace)))
                   (target (if (and number (<= 1 number count)) number active)))
              (append-window target id)
              (if (/= target active)
                  (hide-id id)
                  (progn
                    (if (or (property props :fullscreen)
                            (and honor (or (getf window :fullscreen-requested)
                                           (getf window :fullscreen))))
                        (set-fullscreen id t
                                        (when (and honor (not (property props :fullscreen)))
                                          (getf window :output)))
                        (arrange))
                    (multiple-value-bind (value present) (property props :focus)
                      (unless (or (and present (null value))
                                  (and focused (full-box focused)))
                        (focus-id id)))))))))
      (case type
        (:pointer
         (when (and (eq (getf event :state) :enter) (getf event :moved)
                    (setting snapshot :focus-follows-mouse) (member (getf event :id) visible))
           (setf focused (getf event :id))))
        (:button
         (when (eq :pressed (getf event :state))
           (let ((id (getf event :id)))
             (focus-id (when (member id visible) id)))))
        (:command
         (let* ((arguments (getf event :arguments))
                (name (getf event :name))
                (id (getf arguments :window)))
           (cond
             ((equal name "focus-window") (reveal id))
             ((equal name "focus-next-window") (cycle-focus 1))
             ((equal name "focus-previous-window") (cycle-focus -1))
             ((equal name "close-window") (push (close-window id) commands))
             ((equal name "toggle-fullscreen")
              (when id (set-fullscreen id (null (full-box id)))))
             ((equal name "switch-workspace") (switch-workspace (getf arguments :workspace)))
             ((equal name "move-window-to-workspace")
              (move-window (getf arguments :workspace) id))
             ((equal name "move-window")
              (move-box id (getf arguments :x) (getf arguments :y) nil nil))
             ((equal name "resize-window")
              (move-box id nil nil (getf arguments :width) (getf arguments :height))))))
        ((:metadata :request)
         (let ((id (getf event :id)) (request (getf event :request)))
           (when (member id ids)
             (case request
               (:fullscreen
                (if (getf event :requested)
                    (when honor (set-fullscreen id t (getf event :output)))
                    (set-fullscreen id nil)))
               (:unfullscreen (set-fullscreen id nil))
               (:close (push (close-window id) commands))
               (:activate
                (loop for wins in workspaces for number from 1
                      when (member id wins) do (switch-workspace number) (return))
                (focus-id id)))))))
      (let ((effects (list (motion-contract :hover t))))
        (dolist (id ids)
          (let ((box (or (full-box id) (window-box id)))
                (shown (and area (member id visible) t)))
            (when box
              (push (place id (getf box :x) (getf box :y)
                           (getf box :width) (getf box :height) shown) effects))
            (push (if shown (show-window id) (hide-window id)) effects)
            (push (fullscreen id (not (null (full-box id)))) effects)
            (push (maximize id nil) effects)))
        (dolist (id stack) (when (member id visible) (push (raise-window id) effects)))
        (push (focus (when (member focused visible) focused) :raise nil) effects)
        (dolist (effect (wm-commands count)) (push effect effects))
        (dolist (binding '(("f" "toggle-fullscreen" "Toggle Fullscreen")
                           ("j" "focus-next-window" "Focus Next Window")
                           ("k" "focus-previous-window" "Focus Previous Window")))
          (destructuring-bind (key command description) binding
            (push (bind-key '(:mod) key command :description description) effects)))
        (loop for number from 1 to (min count 9)
              do
          (push (bind-key '(:mod) (princ-to-string number)
                          (invoke "switch-workspace" :workspace number)
                          :description (when (= number 1) "Switch to Workspace 1-9")) effects)
          (push (bind-key '(:mod :shift) (princ-to-string number)
                          (invoke "move-window-to-workspace" :workspace number)
                          :description (when (= number 1) "Move Window to Workspace 1-9")) effects))
        (push (publish-state :wm-state
                             (list :active active :workspaces
                                   (loop for wins in workspaces for number from 1
                                         collect (list :id number :windows (length wins)))))
              effects)
        (let ((value (json-object
                      (cons "active" active)
                      (cons "workspaces"
                            (apply #'json-array
                                   (loop for wins in workspaces for number from 1
                                         collect (json-object (cons "id" number)
                                                              (cons "windows" (length wins)))))))))
          (push (serve-state "wm_state" value) effects)
          (push (announce "wm_state" value) effects))
        (values (list :active active :workspaces workspaces :fullscreen full
                      :focused focused :ids ids :boxes boxes :stacking stack
                      :visible visible :geometry-key geometry-key)
                (nreverse effects) commands)))))

(defparameter +drag-contract+
  (motion-contract :drag '(:bound 1048576 :min-size 64 :max-size 16384))
  "A Super+drag moves or resizes a window by pure arithmetic the native core can
carry alone, so the pointer never waits for Lisp while a window follows it.")

(define-extension "drag" (:reads (:windows :layout :button :grab :settings) :state nil) (snapshot state event)
  (labels ((drag-box (state dx dy)
             (flet ((bound (value) (min 1048576 (max -1048576 value)))
                    (size (value) (min 16384 (max 64 value))))
               (if (eq (getf state :mode) :move)
                   (list (bound (+ (getf state :base-x) dx))
                         (bound (+ (getf state :base-y) dy))
                         (getf state :base-width) (getf state :base-height))
                   (list (getf state :base-x) (getf state :base-y)
                         (size (+ (getf state :base-width) dx))
                         (size (+ (getf state :base-height) dy)))))))
  (let* ((layout (context snapshot :layout))
         (windows (context snapshot :windows))
         (ids (loop for window in windows
                    when (getf window :buffered t) collect (getf window :id)))
         (type (getf event :type))
         (id (getf state :id))
         (generation (getf state :buffer-generation))
         (modifiers (getf event :modifiers))
         (target (find (getf event :id) layout :key (lambda (window) (getf window :id)))))
    (cond
      ((and state (or (not (member id ids))
                      (not (eql generation
                                (getf (find id windows :key (lambda (w) (getf w :id)))
                                      :buffer-generation)))))
       (values nil (list +drag-contract+) nil))
      ((and (eq type :button) (eq (getf event :state) :pressed) target
            (member (getf event :id) ids) (member (getf event :button) '(272 273))
            (integerp modifiers)
            (logtest (ecase (setting snapshot :mod) (:shift 1) (:control 4) (:alt 8) (:super 64))
                     modifiers))
       (let* ((window-id (getf event :id))
             (buffer-generation (getf (find window-id windows :key (lambda (w) (getf w :id)))
                                      :buffer-generation))
             (mode (if (eql (getf event :button) 272) :move :resize))
             (box (list (getf target :x) (getf target :y)
                        (getf target :width) (getf target :height))))
         (values (list :id window-id :mode mode :buffer-generation buffer-generation
                       :origin-x (getf event :x) :origin-y (getf event :y)
                       :base-x (first box) :base-y (second box)
                       :base-width (third box) :base-height (fourth box)
                       :x (first box) :y (second box) :width (third box) :height (fourth box))
                 (list +drag-contract+
                       (grab window-id mode :buffer-generation buffer-generation)
                       (apply #'place window-id box)
                       (fullscreen window-id nil))
                 nil)))
      ((and (eq type :button) (eq (getf event :state) :released) state (eql (getf event :id) id))
       (values nil (list +drag-contract+) nil))
      ((and (eq type :grab) state (eql (getf event :id) id))
       (let* ((dx (- (getf event :x) (getf state :origin-x)))
              (dy (- (getf event :y) (getf state :origin-y)))
              (box (drag-box state dx dy)))
         (values (list :id id :mode (getf state :mode) :buffer-generation generation
                       :origin-x (getf state :origin-x) :origin-y (getf state :origin-y)
                       :base-x (getf state :base-x) :base-y (getf state :base-y)
                       :base-width (getf state :base-width) :base-height (getf state :base-height)
                       :x (first box) :y (second box) :width (third box) :height (fourth box))
                 (list +drag-contract+
                       (grab id (getf state :mode) :buffer-generation generation)
                       (apply #'place id box)
                       (fullscreen id nil))
                 nil)))
      (state
       (values state
               (list +drag-contract+
                     (grab id (getf state :mode) :buffer-generation generation)
                     (place id (getf state :x) (getf state :y)
                            (getf state :width) (getf state :height) t)
                     (fullscreen id nil))
               nil))
      (t (values nil (list +drag-contract+) nil))))))

(define-extension "config-error" (:reads (:config-error :data) :state nil) (snapshot hidden event)
  (let* ((failure (context snapshot :config-error))
         (serial (getf failure :serial))
         (timer (and serial (intern (format nil "HIDE-~D" serial) :keyword)))
         (hidden (if (and timer (eq (getf event :type) :timer) (eq (getf event :name) timer)) serial hidden)))
    (values hidden
            (when (and serial (not (eql serial hidden)))
              (cons (once timer 5000) (toast :config-error (getf failure :message) :urgent t
                                               :theme (state-value snapshot :theme))))
            nil)))

(defun screencast-request (snapshot event)
  "The immediate answer to a screencast EVENT as (ANSWER VALUE), or a menu as
(:menu ITEMS PICKS)."
  (let ((app-id (getf event :app-id)) (items nil) (picks nil))
    (dolist (window (context snapshot :windows))
      (when (equal app-id (getf window :app-id))
        (let ((rule (assoc :screencast (rules-for snapshot (getf window :id)))))
          (cond ((and rule (null (cdr rule))) (return-from screencast-request (list :deny nil)))
                ((stringp (cdr rule)) (return-from screencast-request (list :output (cdr rule))))))))
    (when (getf event :monitor)
      (dolist (output (context snapshot :outputs))
        (push (format nil "output ~A (~Dx~D)" (getf output :name) (getf output :width) (getf output :height))
              items)
        (push (list :output (getf output :name)) picks)))
    (when (getf event :window)
      (dolist (window (context snapshot :windows))
        (push (format nil "window ~A" (if (equal (getf window :title) "") (getf window :app-id)
                                          (getf window :title)))
              items)
        (push (list :window (getf window :id)) picks)))
    (case (length items)
      (0 (list :deny nil))
      (1 (first picks))
      (t (list :menu (nreverse items) (nreverse picks))))))

(define-extension "screencast" (:reads (:screencast :windows :outputs :rules :command :ui :data) :state nil)
    (snapshot state event)
  (let ((queue (getf state :queue)) (selected (getf state :selected 0)) (commands nil))
    (flet ((answer (token pick) (push (screencast-answer token (first pick) (second pick)) commands)))
      (if (eq (getf event :type) :screencast)
          (let ((result (screencast-request snapshot event)))
            (if (eq (first result) :menu)
                (setf queue (append queue (list (list :token (getf event :token)
                                                      :app-id (getf event :app-id)
                                                      :items (second result) :picks (third result)))))
                (answer (getf event :token) result)))
          (when (and queue (equal (getf event :owner) "screencast"))
            (let ((request (first queue)))
              (multiple-value-bind (action index)
                  (menu-choice event selected (length (getf request :items)))
                (case action
                  (:select (answer (getf request :token) (nth index (getf request :picks)))
                   (setf queue (rest queue) selected 0))
                  (:cancel (answer (getf request :token) (list :deny nil))
                   (setf queue (rest queue) selected 0))
                  (:move (setf selected index))))))))
    (values (list :queue queue :selected selected)
            (when queue
              (let ((request (first queue)))
                (menu-dialog :screencast (getf request :items) selected
                             :title (format nil "Share screen with ~A"
                                            (if (equal (getf request :app-id) "") "an application"
                                                (getf request :app-id)))
                             :theme (state-value snapshot :theme))))
            commands)))

(defun %wallpaper-directory (directory)
  "DIRECTORY without its trailing slashes."
  (let ((path directory))
    (loop while (and (plusp (length path))
                     (char= (char path (1- (length path))) #\/))
          do (setf path (subseq path 0 (1- (length path)))))
    path))

(defun %wallpaper-file-p (path)
  (or (and (search ".png" path :start2 (max 0 (- (length path) 4)) :test #'string-equal) t)
      (and (search ".jpg" path :start2 (max 0 (- (length path) 4)) :test #'string-equal) t)
      (and (search ".jpeg" path :start2 (max 0 (- (length path) 5)) :test #'string-equal) t)))

(defun wallpaper--pick (directory current)
  "A random PNG or JPEG under DIRECTORY other than CURRENT, or CURRENT when there is no other."
  (let ((files (remove current
                       (loop for path in (%list-directory directory)
                             for length = (length path)
                             when (or (and (>= length 4)
                                           (string-equal (subseq path (- length 4)) ".png"))
                                      (and (>= length 4)
                                           (string-equal (subseq path (- length 4)) ".jpg"))
                                      (and (>= length 5)
                                           (string-equal (subseq path (- length 5)) ".jpeg")))
                               collect path)
                       :test #'equal)))
    (if files (nth (random (length files)) files) current)))

(define-extension "wallpaper" (:reads (:data :command) :state nil) (snapshot state event)
  (let* ((settings (state-value snapshot :wallpaper-settings))
         (directory (getf settings :directory))
         (bind (getf settings :bind))
         (command (and (eq (getf event :type) :command) (getf event :name)))
         (path (and directory
                    (if (or (not (equal directory (getf state :directory)))
                            (equal command "next-wallpaper"))
                        (wallpaper--pick directory (getf state :path))
                        (getf state :path)))))
    (values (list :directory directory :path path)
            (append (list (define-command "next-wallpaper"
                            :description "Show another wallpaper from the configured directory."))
                    (when bind (list (apply #'bind-key bind)))
                    (when path
                      (list (shell-surface :wallpaper (ui :stack)
                                           :anchors '(:top :right :bottom :left) :layer :background
                                           :background (list :image path :fit (getf settings :fit :cover))))))
            nil)))

(define-extension "screenshot-clipboard" (:reads (:screenshot) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (values state nil
          (when (eq (getf event :type) :screenshot)
            (list (clipboard-copy (getf event :path) "image/png" :delete t)))))

(defparameter +screenshot-glsl+
  "void mainImage(out vec4 color, in vec2 p) {
    vec4 dim = vec4(0.0, 0.0, 0.0, iData[1].y);
    vec2 lo = min(iSelection[0].xy, iSelection[0].zw), hi = max(iSelection[0].xy, iSelection[0].zw);
    vec2 edge = vec2(iData[1].x);
    if (iSelection[3].x < 0.5) color = dim;
    else if (all(greaterThanEqual(p, lo)) && all(lessThan(p, hi))) color = vec4(0.0);
    else if (all(greaterThanEqual(p, lo - edge)) && all(lessThan(p, hi + edge)))
        color = vec4(iData[0].rgb * iData[0].a, iData[0].a);
    else color = dim;
    vec4 mark = markvec(p);
    if (mark.a > 0.0) color = mark + color * (1.0 - mark.a);
}
"
  "The selector every look falls back to: the output dimmed but for the
selection, which a two-pixel accent frame rings, and the annotation marks in the
colours they were drawn in over it.")

(defun screenshot-rgba (hex &optional (alpha 1))
  "#RRGGBB as four reals from 0 to 1, ALPHA last."
  (append (loop for i from 1 below 7 by 2
                collect (/ (parse-integer hex :start i :end (+ i 2) :radix 16) 255d0))
          (list alpha)))

(defparameter +screenshot-theme+
  '(:base "#1c1c21" :text "#ffffff" :accent "#7aa1f7" :surface1 "#45454f")
  "The selector's colours when no :theme is published.")

(defparameter +screenshot-keys+ '("a" "t" "f" "u" "r" "m")
  "The keys the selector takes for an arrow, a text label, a freehand stroke,
undoing the last mark, going back to selecting a region and freezing the screen
as it is shown, in order.")

(defun screenshot-palette (ink)
  "The colours a mark can take, each with the key that picks it. INK comes first,
so a mark starts in the theme's text colour, and follows it a red, an amber, a
blue and a green that read on a light and a dark theme alike."
  (list (cons "1" ink) (cons "2" "#ff5d5d") (cons "3" "#ffc857")
        (cons "4" "#57c8ff") (cons "5" "#6ee7a8")))

(defun screenshot-mark-palette (mark)
  "MARK's :PALETTE colours paired with its :PALETTE-KEYS, in order."
  (loop for colour in (getf mark :palette)
        for key in (getf mark :palette-keys)
        collect (cons key colour)))

(defun screenshot-default-look (published)
  "Today's selector in PUBLISHED's :theme colours: +SCREENSHOT-GLSL+ over the
output, the key hint in a plate ringed by the accent, and the marks in the
theme's text colour."
  (let* ((palette (or published +screenshot-theme+))
         (accent (theme :accent palette))
         (ink (theme :text palette))
         (colours (screenshot-palette ink)))
    (list :glsl +screenshot-glsl+
          :data (append (screenshot-rgba accent) (list 2 0.4d0 0 0)
                        (screenshot-rgba ink))
          :hint (list :ink ink
                      :plate (theme :base palette)
                      :rim accent :rim-width 2
                      :key (theme :surface1 palette)
                      :font "sans" :size 17 :padding 12 :margin 32 :radius 0)
          :mark (list :ink ink :width 3 :head 18 :size 22 :font "sans"
                      :palette (mapcar #'cdr colours)
                      :palette-keys (mapcar #'car colours))
          :motion (list :enabled nil))))

(defun screenshot-hint (selector hint keys palette)
  "The keys the open SELECTOR takes, set as HINT styles them: one line of Pango
markup, each key on a chip of HINT's :KEY colour, the tool holding the pointer's
chip in HINT's ink so the mode shows in the chrome, each colour a mark can take
as a swatch with the key that picks it and the one in hand inked, and that tool's
own keys beside undo, reselect, freeze, capture and cancel. While the colour and
size keys still change the mark just drawn, an inked chip says so beside the tool
key that lets it go. The tool keys are named from KEYS so a look that changes
them is not described by the old ones."
  (labels ((key (label &optional active)
             (format nil "<span background='~A'~A> ~A </span>"
                     (if active (getf hint :ink) (getf hint :key))
                     (let ((ink (if active (getf hint :plate) (getf hint :key-ink))))
                       (if ink (format nil " foreground='~A'" ink) ""))
                     label))
         (name (index fallback)
           (let ((key (nth index keys)))
             (string-upcase (if (and (stringp key) (plusp (length key))) key fallback))))
         (tool (index fallback tool)
           (key (name index fallback) (eq tool (getf selector :tool))))
         (editing (tool)
           (let ((mark (getf selector :editing)))
             (if mark
                 (format nil "~A ~A next    "
                         (key (format nil "editing last ~A"
                                      (case mark (:arrow "arrow") (:free "stroke") (t "label")))
                              t)
                         (case tool
                           (:arrow (key (name 0 "A")))
                           (:text (key (name 1 "T")))
                           (t (key (name 2 "F")))))
                 "")))
         (size (tool)
           (case tool
             (:arrow (format nil "~A ~A size    ~A head" (key "[") (key "]") (key "H")))
             ((:text :free) (format nil "~A ~A size" (key "[") (key "]")))
             (t nil)))
         (tail ()
           (format nil "~A undo    ~A reselect    ~A freeze    ~A capture    ~A cancel"
                   (key (name 3 "U")) (key (name 4 "R")) (key (name 5 "M"))
                   (key "Enter") (key "Esc")))
         (swatches ()
           (let ((active (getf selector :colour 0)))
             (format nil "~{~A~^ ~}"
                     (loop for (colour-key . colour) in palette
                           for index from 1
                           collect (format nil "<span background='~A'>   </span>~A"
                                           colour (key (string-upcase colour-key)
                                                       (eql index active)))))))
         (keys-line (tool)
           (cond ((getf selector :typing)
                  (format nil "Type the label    ~A    ~A keep    ~A discard    ~A undo"
                          (size :text) (key "Enter") (key "Esc") (key "Ctrl+Z")))
                 ((not (getf selector :selected))
                  (format nil "Drag to select    ~A whole screen    ~A cancel"
                          (key "Space") (key "Esc")))
                 (t (let ((shape (unless (eq tool :select) (size tool))))
                      (format nil "~A~A arrow    ~A text    ~A freehand    ~A~A    ~A"
                              (editing tool)
                              (tool 0 "A" :arrow) (tool 1 "T" :text) (tool 2 "F" :free)
                              (if shape (format nil "~A    " shape) "")
                              (swatches) (tail)))))))
    (ui :column :padding (getf hint :rim-width 0) :background (getf hint :rim)
        :radius (getf hint :radius 0)
        :children
        (list (ui :column :padding (getf hint :padding 12) :background (getf hint :plate)
                  :radius (max 0 (- (getf hint :radius 0) (getf hint :rim-width 0)))
                  :children
                  (list (ui :text :markup t :size (getf hint :size 17) :font (getf hint :font "sans")
                            :color (getf hint :ink)
                            :text (keys-line (getf selector :tool)))))))))

(defun screenshot-surfaces (selector look)
  "The open SELECTOR drawn in LOOK: its shader over the whole output, reading the
selection as iSelection and the marks through MARKED, and its key hint."
  (let ((output (getf selector :output)) (hint (getf look :hint))
        (mark (getf look :mark)))
    (list (shell-surface :screenshot-look (ui :stack)
                         :output output :anchors '(:top :right :bottom :left)
                         :layer :screenshot :click-through t :background
                         (list (if (getf look :glsl) :glsl :shader)
                               (or (getf look :glsl) (getf look :shader))
                               :data (getf look :data) :clock 0 :until 0))
          (shell-surface :screenshot-hint
                         (screenshot-hint selector hint (getf mark :keys)
                                          (screenshot-mark-palette mark))
                         :output output :anchors '(:bottom) :margin (getf hint :margin 32)
                         :layer :screenshot :click-through t :background "#00000000"
                         :blur (getf hint :blur)))))

(defun screenshot-mark-settings (mark)
  "The compositor settings a LOOK's :MARK gives the marks: the INK they start in
and the PALETTE colours with the PALETTE-KEYS that pick them, the stroke WIDTH,
the arrow HEAD, the label SIZE and FONT, and the KEYS that pick each tool."
  (when mark
    (settings :screenshot-mark
              (list :ink (getf mark :ink "#ffffffff")
                    :width (getf mark :width 3)
                    :head (getf mark :head 18)
                    :size (getf mark :size 22)
                    :font (list (getf mark :font "sans"))
                    :keys (copy-list (or (getf mark :keys) +screenshot-keys+))
                    :palette (copy-list (getf mark :palette))
                    :palette-keys (copy-list (getf mark :palette-keys))))))

(define-extension "screenshot-theme" (:reads (:screenshot :data) :state nil) (snapshot state event)
  (let ((state (if (eq (getf event :type) :selector)
                   (and (getf event :open) (copy-list event))
                   state))
        (look (or (state-value snapshot :screenshot-look)
                  (screenshot-default-look (state-value snapshot :theme)))))
    (values state
            (remove nil
                    (append (list (settings :screenshot-motion
                                            (getf look :motion (list :enabled nil)))
                                  (screenshot-mark-settings (getf look :mark)))
                            (when state (screenshot-surfaces state look))))
            nil)))

(defparameter +notification-theme+ '(:base "#1e1e2e" :text "#cdd6f4" :subtext0 "#a6adc8")
  "The notification card colours when no :theme is published.")

(define-extension "notification-popups" (:reads (:services :outputs :data) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((published (state-value snapshot :theme))
         (service (service-state snapshot :notifications))
         (notifications (and (getf service :available)
                             (getf service :notifications)))
         (outputs (context snapshot :outputs))
         (estimates
           (loop for output in outputs collect
             (let* ((scale (max 1 (getf output :scale-120 120)))
                    (width (ceiling (* 320 scale) 120))
                    (summary-height (ceiling (* 91 scale) 600))
                    (body-height (ceiling (* 78 scale) 600))
                    (padding (ceiling (* 20 scale) 120))
                    (inner-gap (ceiling (* 4 scale) 120))
                    (stack-gap (ceiling (* 8 scale) 120))
                    (height (+ (* 2 summary-height) (* 8 body-height)
                               padding inner-gap stack-gap)))
               (list :area (* width height)))))
         (per-surface-limit
           (or (loop for estimate in estimates
                     minimize (floor (* 16 1024 1024) (getf estimate :area)))
               64))
         (aggregate-area (loop for estimate in estimates sum (getf estimate :area)))
         (aggregate-limit (if (plusp aggregate-area)
                              (floor (* 32 1024 1024) aggregate-area)
                              64))
         (summary-lines 2)
         (body-lines 8)
         (visible-nodes-per-card (+ 1 1 summary-lines 1 body-lines))
         (ui-card-limit (floor (1- 512) visible-nodes-per-card))
         (copy-nodes-per-card 512)
         (copy-card-limit
           (floor 32768 (* (max 1 (length outputs)) copy-nodes-per-card)))
         (card-limit (max 0 (min 64 per-surface-limit aggregate-limit
                                    ui-card-limit copy-card-limit))))
    (labels ((preview-lines (value line-limit character-limit)
               (let* ((end (min (length value) character-limit))
                      (limit (max 1 line-limit))
                      (start 0)
                      (lines nil))
                 (loop for index below end
                       when (char= (char value index) #\Newline)
                         do (push (subseq value start index) lines)
                            (setf start (1+ index))
                            (when (= (length lines) limit)
                              (return)))
                 (when (< (length lines) limit)
                   (push (subseq value start end) lines))
                 (nreverse lines)))
             (text (value size color)
               (ui :text :text value :size size :color color))
             (text-block (value line-limit character-limit size color)
               (let ((lines (preview-lines value line-limit character-limit)))
                 (if (= 1 (length lines))
                     (list (text (first lines) size color))
                     (list (ui :column :gap 0
                               :children (mapcar (lambda (line) (text line size color))
                                                  lines))))))
             (card (notification)
               (let* ((app (getf notification :app))
                      (summary (getf notification :summary))
                      (body (getf notification :body))
                      (urgent (getf notification :urgent))
                      (palette (or published
                                   (if urgent
                                       (list* :base "#45222c" +notification-theme+)
                                       +notification-theme+))))
                 (ui :column :width 320 :padding 10 :gap 4
                     :background (theme :base palette)
                     :border (if (and urgent published) 2 0) :border-color (theme :error palette)
                     :children
                     (append
                      (text-block (if (plusp (length summary)) summary app)
                                  summary-lines 256 14 (theme :text palette))
                      (when (plusp (length body))
                        (text-block body body-lines 512 12 (theme :subtext0 palette))))))))
      (values nil
              (when (and (listp notifications) notifications (plusp card-limit))
                (list
                 (shell-surface :notifications
                   (ui :column :gap 8
                       :children (loop for notification in notifications
                                        for count from 1 to card-limit
                                        collect (card notification)))
                   :anchors '(:top) :margin 8 :layer :top
                   :exclusive-zone 0 :background "#00000000" :placement :view)))
              nil))))
