(in-package #:tomoe-user)


(defparameter +workspaces-gap+ 6)
(defparameter +workspaces-tags+
  '(("1" . :tag1) ("2" . :tag2) ("3" . :tag3)
    ("4" . :tag4) ("5" . :tag5) ("6" . :tag6)
    ("7" . :tag7) ("8" . :tag8) ("9" . :tag9)))

(defun workspaces--tag (command)
  "The tag number named by a TAGn binding command, or NIL."
  (when (and (stringp command) (> (length command) 3) (string= "tag" command :end2 3))
    (let ((number (parse-integer command :start 3 :junk-allowed t)))
      (when (and number (<= 1 number (length +workspaces-tags+))) number))))

(defun workspaces--tile (output windows)
  "A row of equal-height slices, in map order, inside OUTPUT."
  (let* ((count (length windows))
         (gap +workspaces-gap+)
         (height (max 1 (floor (- (getf output :height) (* gap (1+ count))) count))))
    (loop for window in windows
          for index from 0
          collect (place (getf window :id)
                         (+ (getf output :x) gap)
                         (+ (getf output :y) gap (* index (+ gap height)))
                         (max 1 (- (getf output :width) (* 2 gap)))
                         height))))

(defun workspaces--bindings ()
  (append (loop for entry in +workspaces-tags+
                collect (bind-key '(:super) (car entry) (cdr entry)))
          (list (bind-key '(:super :shift) "Tab" :previous))))

(define-extension "workspaces"
    (:reads (:windows :outputs :focus :key)
     :state (list :tag 1 :previous 1 :assigned nil :recent nil))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (resolved (context snapshot :focus))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (tag (getf state :tag))
         (previous (getf state :previous))
         (assigned (loop for entry in (getf state :assigned)
                         when (member (car entry) ids) collect entry))
         (recent (getf state :recent))
         (focused nil)
         (new-ids (remove-if (lambda (id) (assoc id assigned)) ids)))
    (dolist (id new-ids) (push (cons id tag) assigned))
    (when new-ids
      (let ((new-focused (car (last new-ids))))
        (setf recent (acons tag new-focused (remove tag recent :key #'car)))))
    (when (and (eq (getf event :type) :key) (equal (getf event :owner) "workspaces"))
      (if (equal (getf event :command) "previous")
          (rotatef tag previous)
          (let ((number (workspaces--tag (getf event :command))))
            (when number (setf previous tag tag number)))))
    (let* ((visible (loop for window in windows
                          when (eql (cdr (assoc (getf window :id) assigned)) tag)
                            collect window))
           (visible-ids (mapcar (lambda (window) (getf window :id)) visible))
           (remembered (cdr (assoc tag recent))))
      (setf focused
            (cond ((member remembered visible-ids) remembered)
                  ((member resolved visible-ids) resolved)
                  (t (first visible-ids))))
      (when focused (setf recent (acons tag focused (remove tag recent :key #'car))))
      (values (list :tag tag :previous previous :assigned assigned :recent recent)
              (when (or output windows)
                (append (loop for window in windows
                              for id = (getf window :id)
                              unless (eql (cdr (assoc id assigned)) tag)
                                collect (place id 0 0 (max 1 (getf window :width))
                                               (max 1 (getf window :height)) nil))
                        (when (and output visible) (workspaces--tile output visible))
                        (when windows (list (focus focused)))
                        (workspaces--bindings)))
              nil))))
