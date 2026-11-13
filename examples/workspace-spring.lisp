(in-package #:tomoe-user)

(defparameter +spring-gap+ 6 "Room around each tile.")
(defparameter +spring-tags+ '("1" "2" "3") "The Super key that switches each.")

(defparameter +spring-names+
  (loop for number from 1 to (length +spring-tags+)
        collect (format nil "spring-tag-~(~R~)" number))
  "The command each tag's Super key runs. Command names end in letters, not digits.")

(defparameter +spring-commands+
  (loop for name in +spring-names+ for number from 1
        collect (define-command name
                  :description (format nil "Show desktop tag ~D." number))))

(defun spring--tag (command)
  "The tag number COMMAND shows, or NIL for another command."
  (and (stringp command)
       (let ((index (position command +spring-names+ :test #'equal)))
         (and index (1+ index)))))

(define-extension "workspace-spring"
    (:reads (:windows :outputs :focus :command)
     :state (list :tag 1 :assigned nil))
    (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (ids (mapcar (lambda (window) (getf window :id)) windows))
         (assigned (loop for entry in (getf state :assigned)
                         when (member (car entry) ids) collect entry))
         (tag (getf state :tag)))
    (dolist (id (remove-if (lambda (id) (assoc id assigned)) ids))
      (push (cons id tag) assigned))
    (when (eq (getf event :type) :command)
      (let ((number (spring--tag (getf event :name))))
        (when number (setf tag number))))
    (let* ((visible (loop for window in windows
                          when (eql (cdr (assoc (getf window :id) assigned)) tag)
                            collect window))
           (visible-ids (mapcar (lambda (window) (getf window :id)) visible))
           (focused (let ((resolved (context snapshot :focus)))
                      (cond ((member resolved visible-ids) resolved)
                            (visible-ids (first visible-ids))
                            (t nil))))
           (count (length visible))
           (width (and output (plusp count)
                       (max 1 (floor (- (getf output :width)
                                        (* +spring-gap+ (1+ count))) count))))
           (height (and output (max 1 (- (getf output :height) (* 2 +spring-gap+))))))
      (values (list :tag tag :assigned assigned)
              (append (when output
                        (loop for window in windows
                              for id = (getf window :id)
                              for index = (position window visible)
                              collect (if index
                                          (place id (+ (getf output :x) +spring-gap+
                                                       (* index (+ +spring-gap+ width)))
                                                 (+ (getf output :y) +spring-gap+)
                                                 width height)
                                          (place id 0 0 (max 1 (getf window :width))
                                                 (max 1 (getf window :height)) nil))))
                      (when windows (list (focus focused)))
                      (list (settings :animations
                                      '(:window-switch (:spring (:damping-ratio 3/5
                                                                 :stiffness 160))
                                        :window-close (:ease (:duration-ms 90
                                                              :curve :ease-out-quad))))
                            (bind-key '(:super) "1" (first +spring-names+))
                            (bind-key '(:super) "2" (second +spring-names+))
                            (bind-key '(:super) "3" (third +spring-names+)))
                      +spring-commands+)
              nil))))
