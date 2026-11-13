(in-package #:tomoe-user)

(defparameter +spring-gap+ 6 "Room around each tile.")
(defparameter +spring-tags+ '("1" "2" "3") "The Super key that switches each.")

(defun spring--tag (command)
  (when (and (stringp command) (> (length command) 3) (string= "tag" command :end2 3))
    (let ((number (parse-integer command :start 3 :junk-allowed t)))
      (when (and number (<= 1 number (length +spring-tags+))) number))))

(define-extension "workspace-spring"
    (:reads (:windows :outputs :focus :key)
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
    (when (and (eq (getf event :type) :key) (equal (getf event :owner) "workspace-spring"))
      (let ((number (spring--tag (getf event :command))))
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
                            (bind-key '(:super) "1" :tag1)
                            (bind-key '(:super) "2" :tag2)
                            (bind-key '(:super) "3" :tag3)))
              nil))))
