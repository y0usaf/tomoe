(in-package #:tomoe-user)


(defparameter +float-cascade-step+ 32)
(defparameter +float-max-size+ 16384)

(defun float--overlap (a b)
  "True when two (X Y WIDTH HEIGHT) boxes overlap."
  (and (< (first a) (+ (first b) (third b)))
       (< (first b) (+ (first a) (third a)))
       (< (second a) (+ (second b) (fourth b)))
       (< (second b) (+ (second a) (fourth a)))))

(defun float--box (geometry id)
  "The (X Y WIDTH HEIGHT) box recorded for ID, or NIL."
  (cdr (assoc id geometry)))

(defun float--new-box (output window boxes)
  "Centre WINDOW at two thirds of OUTPUT, cascading past the BOXES in use."
  (if (null output)
      (list 0 0 (max 1 (getf window :width)) (max 1 (getf window :height)))
      (let* ((width (max 1 (floor (* (getf output :width) 2) 3)))
             (height (max 1 (floor (* (getf output :height) 2) 3)))
             (step +float-cascade-step+)
             (left (+ (getf output :x) (floor (- (getf output :width) width) 2)))
             (top (+ (getf output :y) (floor (- (getf output :height) height) 2)))
             (room (min 8
                        (floor (max 0 (- (+ (getf output :x) (getf output :width))
                                         left width))
                               step)
                        (floor (max 0 (- (+ (getf output :y) (getf output :height))
                                         top height))
                               step)))
             (start (mod (length boxes) (1+ room))))
        (loop for offset from 0 to room
              for index = (mod (+ start offset) (1+ room))
              for box = (list (+ left (* index step)) (+ top (* index step)) width height)
              unless (find-if (lambda (other) (float--overlap box other)) boxes)
                return box
              finally (return (list (+ left (* start step)) (+ top (* start step))
                                    width height))))))

(defun float--drag-mode (event)
  "The grab mode a Super button press asks for, or NIL."
  (when (logtest 64 (or (getf event :modifiers) 0))
    (case (getf event :button)
      (272 :move)
      (273 :resize))))

(defun float--size (value)
  (max 1 (min +float-max-size+ value)))

(defun float--motion (drag event)
  "DRAG's origin box advanced to the pointer position EVENT reports."
  (let* ((x (getf event :x))
         (y (getf event :y))
         (dx (if x (- x (getf drag :x)) (or (getf event :dx) 0)))
         (dy (if y (- y (getf drag :y)) (or (getf event :dy) 0))))
    (destructuring-bind (origin-x origin-y origin-width origin-height) (getf drag :box)
      (ecase (getf drag :mode)
        (:move (list (+ origin-x dx) (+ origin-y dy) origin-width origin-height))
        (:resize (list origin-x origin-y
                       (float--size (+ origin-width dx))
                       (float--size (+ origin-height dy))))))))

(define-extension "float"
    (:reads (:windows :outputs :focus :key :button :grab)
     :state (list :geometry nil :released nil :drag nil :focus nil))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (resolved (context snapshot :focus))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (geometry (loop for entry in (getf state :geometry)
                         when (member (first entry) ids) collect entry))
         (released (intersection (getf state :released) ids))
         (drag (getf state :drag))
         (focused (getf state :focus))
         (new-ids (remove-if (lambda (id)
                               (or (float--box geometry id) (member id released)))
                             ids)))
    (unless (member (getf drag :id) ids) (setf drag nil))
    (when new-ids (setf focused (car (last new-ids))))
    (case (getf event :type)
      (:button
       (let ((id (getf event :id)))
         (if (eq (getf event :state) :pressed)
             (progn
               (when (member id ids) (setf focused id))
               (let ((mode (float--drag-mode event)))
                 (when (and mode (float--box geometry id))
                   (setf drag (list :id id :mode mode
                                    :x (or (getf event :x) 0) :y (or (getf event :y) 0)
                                    :box (float--box geometry id))))))
             (setf drag nil))))
      (:grab
       (when (and drag (eql (getf event :id) (getf drag :id)))
         (setf geometry (cons (cons (getf drag :id) (float--motion drag event))
                              (remove (getf drag :id) geometry :key #'first)))))
      (:key
       (when (and (equal (getf event :owner) "float")
                  (equal (getf event :command) "release")
                  (member focused ids))
         (setf geometry (remove focused geometry :key #'first)
               released (adjoin focused released))
         (when (and drag (eql (getf drag :id) focused)) (setf drag nil)))))
    (let ((boxes nil) (places nil))
      (dolist (window windows)
        (let ((id (getf window :id)))
          (unless (member id released)
            (let ((box (float--box geometry id)))
              (unless box
                (setf box (float--new-box output window boxes)
                      geometry (cons (cons id box) geometry)))
              (push box boxes)
              (push (apply #'place id (append box (list t))) places)))))
      (setf focused (or (and (member focused ids) focused)
                        (and (member resolved ids) resolved)
                        (first ids)))
      (values (list :geometry geometry :released released :drag drag :focus focused)
              (when (or output windows)
                (append (nreverse places)
                        (when windows (list (focus focused)))
                        (when drag (list (grab (getf drag :id) (getf drag :mode))))
                        (list (bind-key '(:control :super) "q" :release))))
              nil))))
