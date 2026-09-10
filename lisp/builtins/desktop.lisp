(in-package #:tomoe-user)

;; The host does not recognize these names. Each unit can be unmounted or
;; replaced with the same API available to a config file.
(define-extension "tiles" (:reads (:windows :outputs) :state 8) (snapshot gap event)
  (declare (ignore event))
  (let* ((windows (context snapshot :windows))
         (output (first (context snapshot :outputs)))
         (count (length windows)))
    (values gap
            (when (and output windows)
              (let* ((width (getf output :width))
                     (height (getf output :height))
                     (gap (min gap (floor width (1+ (* 2 count))) (floor height 3)))
                     (available (- width (* gap (1+ count)))))
                (loop for window in windows for i from 0
                      for left = (floor (* available i) count)
                      for right = (floor (* available (1+ i)) count)
                      collect (place (getf window :id)
                                     (+ (getf output :x) gap (* i gap) left)
                                     (+ (getf output :y) gap)
                                     (max 1 (- right left)) (max 1 (- height (* gap 2)))))))
            nil)))

(define-extension "focus" (:reads (:windows :button :key) :state nil) (snapshot focused event)
  (let ((ids (mapcar (lambda (w) (getf w :id)) (context snapshot :windows))))
    (unless (member focused ids) (setf focused (first ids)))
    (case (getf event :type)
      (:map (setf focused (getf event :id)))
      (:button (setf focused (let ((id (getf event :id))) (when (member id ids) id))))
      (:key
       (when (and (equal (getf event :owner) "focus") (equal (getf event :command) "next"))
         (setf focused (or (second (member focused ids)) (first ids))))))
    (values focused (list (focus focused) (bind-key '(:super) "Tab" :next)) nil)))

(define-extension "commands" (:reads (:key :focus) :state nil) (snapshot state event)
  (declare (ignore state))
  (let ((focused (context snapshot :focus))
        (command (when (and (eq (getf event :type) :key) (equal (getf event :owner) "commands"))
                   (getf event :command))))
    (values nil
            (list (bind-key '(:super) "Return" :terminal)
                  (bind-key '(:super) "q" :close)
                  (bind-key '(:super :shift) "r" :reload)
                  (bind-key '(:super :shift) "Escape" :quit))
            (cond ((equal command "terminal") (list (launch "foot")))
                  ((and (equal command "close") focused) (list (close-window focused)))
                  ((equal command "reload") (list (reload)))
                  ((equal command "quit") (list (quit)))))))
