(in-package #:tomoe-user)

(defun timing-slot (index)
  (list (+ 100 (* 340 index)) 100 320 240))

(define-extension "timing-policy" (:reads (:windows :button :grab) :state nil) (snapshot state event)
  (let* ((windows (context snapshot :windows))
         (type (getf event :type))
         (id (getf state :id))
         (target (find (getf event :id) windows :key (lambda (window) (getf window :id)))))
    (cond
      ((and (eq type :button) (eq (getf event :state) :pressed) target
            (eql (getf event :button) 272))
       (let ((slot (timing-slot (position target windows))))
         (values (list :id (getf event :id) :origin-x (getf event :x) :origin-y (getf event :y)
                       :base-x (first slot) :base-y (second slot)
                       :width (third slot) :height (fourth slot))
                 (list (grab (getf event :id) :move) (apply #'place (getf event :id) slot))
                 nil)))
      ((and (eq type :grab) state (eql (getf event :id) id))
       (let ((x (+ (getf state :base-x) (- (getf event :x) (getf state :origin-x))))
             (y (+ (getf state :base-y) (- (getf event :y) (getf state :origin-y)))))
         (values (list* :x x :y y (loop for (key value) on state by #'cddr
                                        unless (member key '(:x :y)) append (list key value)))
                 (list (grab id :move) (place id x y (getf state :width) (getf state :height) t))
                 nil)))
      ((eq type :button) (values nil nil nil))
      (t (values state
                 (append (list (configure-output "HEADLESS-1" :mode '(1280 720 240)))
                         (loop for window in windows for index from 0
                               collect (apply #'place (getf window :id) (timing-slot index))))
                 nil)))))
