(in-package #:tomoe-user)

;; The "deck" layout: a 32:9 screen split into two half-columns that are each
;; 16:9. Each column is a deck — its front window fills the half and its other
;; windows stay mapped one slot above or below it, so Mod+j/k slides the stack
;; instead of swapping two windows. Port of the deck chunk in the user's Tomoe
;; Lua config, written against the public extension API: workspaces, columns,
;; deck order, the deck front and floating windows are all extension state, and
;; every geometry change is a `place` effect.
;;
;; Mod is Alt, matching the original. Mod+r cycles the split, Mod+o toggles an
;; even grid, Mod+h/l/j/k drive the columns, Mod+Shift+* move windows, Mod+1..9
;; switch workspace, Super+space floats the focused window.

(defparameter +deck-gaps+ 8)
(defparameter +deck-workspace-count+ 9)
;; 16:9 + 16:9, then 21:9 + 11:9 and its mirror, as fractions of 32:9.
(defparameter +deck-ratios+ (list 1/2 (/ 21 32) (/ 11 32)))
(defparameter +deck-float-numerator+ 3/5)

(defun deck--state ()
  (list :active 1 :grid nil :ratio 1 :focus nil
        :workspaces (loop repeat +deck-workspace-count+ collect nil)
        :columns nil
        :fronts (loop repeat +deck-workspace-count+
                      collect (list (cons :left nil) (cons :right nil)))
        :floating nil :floats nil :fullscreen nil))

(defun deck--entry (list id) (assoc id list))
(defun deck--layout-entry (layout id) (find id layout :key (lambda (entry) (getf entry :id))))
(defun deck--workspace (state) (nth (1- (getf state :active)) (getf state :workspaces)))
(defun deck--fronts (state) (nth (1- (getf state :active)) (getf state :fronts)))
(defun deck--side-of (state id)
  (let ((entry (deck--entry (getf state :columns) id))) (and entry (cdr entry))))
(defun deck--front (state side)
  (let ((entry (assoc side (deck--fronts state)))) (and entry (cdr entry))))
(defun deck--column (state side)
  (remove-if-not (lambda (id) (eql (deck--side-of state id) side)) (deck--workspace state)))
(defun deck--manageable-p (state id)
  (and (not (member id (getf state :fullscreen))) (not (member id (getf state :floating)))))

(defun deck--current (state)
  "The window a command acts on: the focused one while it is on the active
workspace, otherwise whatever that workspace still shows. A command must never
depend on an id a hidden or empty workspace left behind, or the keys go dead."
  (let ((focus (getf state :focus))
        (workspace (deck--workspace state)))
    (or (and (member focus workspace) focus)
        (deck--front state :left)
        (deck--front state :right)
        (first workspace))))

;; --- usable area -------------------------------------------------------------

(defun deck--exclusive-edge (anchors)
  "The edge an exclusive zone applies to, by the layer-shell rule."
  (let ((set (sort (copy-list anchors) #'string< :key #'symbol-name)))
    (cond ((equal set '(:top)) '(:top))
          ((equal set '(:bottom)) '(:bottom))
          ((equal set '(:left)) '(:left))
          ((equal set '(:right)) '(:right))
          ((equal set '(:left :right :top)) '(:top))
          ((equal set '(:bottom :left :right)) '(:bottom))
          ((equal set '(:bottom :left :top)) '(:left))
          ((equal set '(:bottom :right :top)) '(:right))
          (t nil))))

(defun deck--layer-inset (layer edge)
  "What LAYER reserves on EDGE: its exclusive zone plus that edge's margin."
  (let ((zone (getf layer :exclusive-zone))
        (margin (getf layer :margin)))
    (if (and (getf layer :visible) (integerp zone) (plusp zone)
             (member edge (deck--exclusive-edge (getf layer :anchors))))
        (+ zone (ecase edge
                  (:top (first margin)) (:right (second margin))
                  (:bottom (third margin)) (:left (fourth margin))))
        0)))

(defun deck--output-of (outputs entry)
  (let ((cx (+ (getf entry :x) (floor (getf entry :width) 2)))
        (cy (+ (getf entry :y) (floor (getf entry :height) 2))))
    (find-if (lambda (output)
               (and (<= (getf output :x) cx (+ (getf output :x) (getf output :width)))
                    (<= (getf output :y) cy (+ (getf output :y) (getf output :height)))))
             outputs)))

(defun deck--area (snapshot)
  "The focused window's output box and the area its exclusive zones leave free.
A layer surface carries no output name here, so every mapped panel is charged
against the output the layout is drawing on."
  (let* ((outputs (context snapshot :outputs))
         (focus (context snapshot :focus))
         (entry (and focus (deck--layout-entry (context snapshot :layout) focus)))
         (output (or (and entry (deck--output-of outputs entry)) (first outputs))))
    (when output
      (flet ((inset (edge)
               (reduce #'+ (context snapshot :layers) :initial-value 0
                       :key (lambda (layer) (deck--layer-inset layer edge)))))
        (let ((left (inset :left)) (right (inset :right))
              (top (inset :top)) (bottom (inset :bottom)))
          (list :output output
                :x (+ (getf output :x) left) :y (+ (getf output :y) top)
                :width (max 1 (- (getf output :width) left right))
                :height (max 1 (- (getf output :height) top bottom))))))))

;; --- geometry ----------------------------------------------------------------

(defun deck--box (state area id)
  "Where ID goes, or NIL when it is not on the active workspace."
  (let* ((gaps +deck-gaps+)
         ;; Gaps are an outer margin too: the layout draws inside them.
         (x (+ (getf area :x) gaps)) (y (+ (getf area :y) gaps))
         (width (- (getf area :width) (* 2 gaps))) (height (- (getf area :height) (* 2 gaps))))
    (cond
      ((not (member id (deck--workspace state))) nil)
      ((member id (getf state :fullscreen))
       (let ((output (getf area :output)))
         (list (getf output :x) (getf output :y) (getf output :width) (getf output :height))))
      ((member id (getf state :floating)) (cdr (deck--entry (getf state :floats) id)))
      ((not (member id (deck--workspace state))) nil)
      ((getf state :grid)
       (let* ((wins (remove-if-not (lambda (candidate) (deck--manageable-p state candidate))
                                   (deck--workspace state)))
              (index (position id wins))
              (count (length wins))
              (cols (ceiling (sqrt count)))
              (rows (ceiling count cols))
              (cell-width (floor (- width (* (1- cols) gaps)) cols))
              (cell-height (floor (- height (* (1- rows) gaps)) rows))
              (column (mod index cols))
              (row (floor index cols)))
         (list (+ x (* column (+ cell-width gaps))) (+ y (* row (+ cell-height gaps)))
               cell-width cell-height)))
      (t
       (let* ((ratio (nth (1- (getf state :ratio)) +deck-ratios+))
              (left-width (floor (* (- width gaps) ratio)))
              (right-width (- width gaps left-width))
              (side (deck--side-of state id))
              (column (deck--column state side))
              (front (position (deck--front state side) column))
              (index (position id column)))
         (when (and side index front)
           (list (if (eql side :left) x (+ x left-width gaps))
                 (+ y (* (- index front) (+ height gaps)))
                 (if (eql side :left) left-width right-width)
                 height)))))))

(defun deck--places (snapshot state area)
  "One place effect per live window: exactly one owner, no duplicates."
  (let ((active (deck--workspace state)))
    (loop for window in (context snapshot :windows)
          for id = (getf window :id)
          for box = (deck--box state area id)
          append (list (if box
                           (apply #'place id (append box (list t)))
                           (place id 0 0 (max 1 (getf window :width))
                                  (max 1 (getf window :height)) nil)))
          append (when (and box (member id active) (member id (getf state :fullscreen)))
                   (list (fullscreen id t))))))

;; --- commands ----------------------------------------------------------------

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
         (other (and index (nth (+ index direction) column)))
         (order (deck--workspace state))
         (mine (and other (position id order)))
         (theirs (and other (position other order))))
    (when (and other mine theirs)
      (setf (nth mine order) other
            (nth theirs order) id))
    state))

(defun deck--cycle (state direction)
  "Focus the next or previous window on the active workspace, wrapping."
  (let* ((order (deck--workspace state))
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
        ;; Popping the front reveals the window after it, as a scroll would.
        (let* ((column (deck--column state previous))
               (index (position id column))
               (next (and index (nth (mod (1+ index) (length column)) column))))
          (deck--adopt-front state previous (if (and next (not (eql next id))) next nil))))
      (setf (cdr (assoc id (getf state :columns))) side)
      (deck--adopt-front state side id))
    state))

(defun deck--toggle-floating (state area)
  (let ((id (deck--current state)))
    (when id
      (cond
        ((member id (getf state :fullscreen))
         ;; A fullscreen client becomes tiled on the first press, as in Tomoe.
         (setf (getf state :fullscreen) (remove id (getf state :fullscreen))))
        ((member id (getf state :floating))
         (setf (getf state :floating) (remove id (getf state :floating))
               (getf state :floats) (remove id (getf state :floats) :key #'car)))
        (t
         (let* ((width (floor (* (getf area :width) +deck-float-numerator+)))
                (height (floor (* (getf area :height) +deck-float-numerator+)))
                (x (+ (getf area :x) (floor (- (getf area :width) width) 2)))
                (y (+ (getf area :y) (floor (- (getf area :height) height) 2))))
           (push id (getf state :floating))
           (push (cons id (list x y width height)) (getf state :floats))))))
    state))

(defun deck--switch-workspace (state number)
  (when (and (<= 1 number +deck-workspace-count+) (/= number (getf state :active)))
    (setf (getf state :active) number
          ;; The most recent window of the new workspace takes focus, as in Tomoe.
          (getf state :focus) (car (last (deck--workspace state)))))
  state)

(defun deck--move-to-workspace (state number)
  (let ((id (getf state :focus)))
    (when (and id (<= 1 number +deck-workspace-count+) (/= number (getf state :active)))
      (setf (nth (1- (getf state :active)) (getf state :workspaces))
            (remove id (deck--workspace state)))
      (setf (nth (1- number) (getf state :workspaces))
            (append (nth (1- number) (getf state :workspaces)) (list id)))
      ;; The window keeps its column, so returning to that workspace finds it
      ;; where it was; focus falls to whatever the old workspace still shows.
      (deck--focus-window state (car (last (deck--workspace state))))))
  state)

(defun deck--command (state area command)
  (cond
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
     (setf (getf state :ratio) (1+ (mod (getf state :ratio) (length +deck-ratios+)))))
    ((equal command "floating") (deck--toggle-floating state area))
    ((equal command "fullscreen")
     (let ((id (deck--current state)))
       (when id
         (if (member id (getf state :fullscreen))
             (setf (getf state :fullscreen) (remove id (getf state :fullscreen)))
             (progn (setf (getf state :floating) (remove id (getf state :floating)))
                    (push id (getf state :fullscreen)))))))
    ((and (stringp command) (eql 0 (search "workspace-" command)))
     (deck--switch-workspace state (parse-integer command :start 10)))
    ((and (stringp command) (eql 0 (search "move-to-" command)))
     (deck--move-to-workspace state (parse-integer command :start 8)))
    (t state)))

;; --- bindings ----------------------------------------------------------------

(defun deck--bindings ()
  (append
   (list (bind-key '(:alt) "h" :focus-left)
         (bind-key '(:alt) "l" :focus-right)
         (bind-key '(:alt) "j" :scroll-down)
         (bind-key '(:alt) "k" :scroll-up)
         (bind-key '(:alt :shift) "h" :swap-columns)
         (bind-key '(:alt :shift) "l" :swap-columns)
         (bind-key '(:alt :shift) "j" :move-down)
         (bind-key '(:alt :shift) "k" :move-up)
         (bind-key '(:alt) "bracketleft" :to-left)
         (bind-key '(:alt) "bracketright" :to-right)
         (bind-key '(:alt) "o" :grid)
         (bind-key '(:alt) "r" :ratio)
         (bind-key '(:alt) "f" :fullscreen)
         (bind-key '(:super) "space" :floating)
         ;; The shipped focus unit binds Super+Tab, but this unit owns focus
         ;; while it is mounted, so the key has to be answered here.
         (bind-key '(:super) "Tab" :next)
         (bind-key '(:super :shift) "Tab" :previous))
   (loop for number from 1 to +deck-workspace-count+
         append (list (bind-key '(:alt) (princ-to-string number)
                                (intern (format nil "WORKSPACE-~D" number) :keyword))
                      (bind-key '(:alt :shift) (princ-to-string number)
                                (intern (format nil "MOVE-TO-~D" number) :keyword))))))

;; --- extension ---------------------------------------------------------------

(define-extension "deck"
    (:reads (:windows :outputs :layers :layout :focus :key :button)
     :state (deck--state))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (focused (context snapshot :focus))
         (area (deck--area snapshot))
         (type (getf event :type)))
    ;; Forget windows that are gone; ids are reused by the compositor.
    (setf (getf state :workspaces)
          (loop for order in (getf state :workspaces)
                collect (remove-if-not (lambda (id) (member id ids)) order))
          (getf state :columns) (remove-if-not (lambda (entry) (member (car entry) ids))
                                               (getf state :columns))
          (getf state :fronts)
          ;; A deck front that closed is cleared, never removed: the two keys
          ;; have to survive every prune, or adopting a front has nothing to set.
          (loop for fronts in (getf state :fronts)
                collect (loop for entry in fronts
                              collect (if (and (cdr entry) (not (member (cdr entry) ids)))
                                          (cons (car entry) nil)
                                          entry)))
          (getf state :floating) (remove-if-not (lambda (id) (member id ids)) (getf state :floating))
          (getf state :floats) (remove-if-not (lambda (entry) (member (car entry) ids))
                                              (getf state :floats))
          (getf state :fullscreen) (remove-if-not (lambda (id) (member id ids))
                                                  (getf state :fullscreen)))
    (unless (member (getf state :focus) ids) (setf (getf state :focus) nil))
    ;; A click or a focus command from another unit moves focus; adopt it first
    ;; so a window that maps in the same dispatch still takes focus afterwards.
    (when (and focused (not (eql focused (getf state :focus)))
               (member focused (deck--workspace state))
               (deck--manageable-p state focused))
      (let ((side (deck--side-of state focused)))
        (when side
          (deck--adopt-front state side focused)
          (deck--focus-window state focused))))
    ;; A new window joins the focused window's column, or the emptier one, and
    ;; arrives at the front of that deck.
    (dolist (window windows)
      (let ((id (getf window :id)))
        (unless (or (deck--side-of state id) (member id (getf state :floating)))
          (let* ((focus-side (and focused (deck--side-of state focused)))
                 (left (length (deck--column state :left)))
                 (right (length (deck--column state :right)))
                 ;; An even split puts the newcomer on the left, as in Tomoe.
                 (side (or focus-side (if (<= left right) :left :right))))
            (push (cons id side) (getf state :columns))
            (setf (nth (1- (getf state :active)) (getf state :workspaces))
                  (append (nth (1- (getf state :active)) (getf state :workspaces)) (list id)))
            (deck--adopt-front state side id)
            (setf (getf state :focus) id)))))
    (when (and (eq type :key) (equal (getf event :owner) "deck"))
      (deck--command state area (getf event :command)))
    ;; A client request is the policy's to accept; this deck honors it.
    (when (and (eq type :metadata) (eql (getf event :request) :fullscreen)
               (member (getf event :id) ids))
      (pushnew (getf event :id) (getf state :fullscreen)))
    ;; A click focuses the window under the pointer and brings it to the front
    ;; of its deck, the way Tomoe's on_focus_change does. This unit owns focus
    ;; while it is mounted, so click-to-focus has to be answered here too.
    (when (and (eq type :button) (eql (getf event :state) :pressed))
      (let ((id (getf event :id)))
        (when (and (integerp id) (member id (deck--workspace state))
                   (deck--manageable-p state id) (deck--side-of state id))
          (deck--adopt-front state (deck--side-of state id) id)
          (deck--focus-window state id))))
    ;; Close and quit are one-shot commands; only key and button dispatch may
    ;; return them, so the binding names them directly.
    (let* ((command (when (and (eq type :key) (equal (getf event :owner) "deck"))
                      (getf event :command)))
           (current (deck--current state)))
      (values state
              (append (deck--bindings) (deck--places snapshot state area)
                      ;; Focus always names a window the active workspace shows,
                      ;; so no command can leave the keyboard without a target.
                      (list (focus current)))
              (cond ((and (equal command "close") current)
                     (list (close-window current)))
                    (t nil))))))
