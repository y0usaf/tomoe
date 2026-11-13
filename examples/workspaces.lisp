(in-package #:tomoe-user)


(defparameter +workspaces-gap+ 6)
(defparameter +workspaces-tags+ '("1" "2" "3" "4" "5" "6" "7" "8" "9"))

(defparameter +workspaces-commands+
  (list (define-command "switch-tag"
          :description "Show another tag."
          :arguments '((:tag :integer :required t :description "Tag number, from 1 through 9.")))
        (define-command "previous-tag"
          :description "Go back to the tag shown before this one.")))

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
  (append (loop for tag in +workspaces-tags+ for number from 1
                collect (bind-key '(:super) tag (invoke "switch-tag" :tag number)))
          (list (bind-key '(:super :shift) "Tab" "previous-tag"))))

(define-extension "workspaces"
    (:reads (:windows :outputs :focus :command)
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
    (when (eq (getf event :type) :command)
      (if (equal (getf event :name) "previous-tag")
          (rotatef tag previous)
          (let ((number (getf (getf event :arguments) :tag)))
            (when (and number (<= 1 number (length +workspaces-tags+)))
              (setf previous tag tag number)))))
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
                        +workspaces-commands+
                        (workspaces--bindings)))
              nil))))
