(in-package #:tomoe-user)

(defparameter +timing-contract+
  (motion-contract :drag '(:bound 1048576 :min-size 64 :max-size 16384)))

(define-extension "timing-contract" (:reads (:windows :button :grab) :state nil)
    (snapshot state event)
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
                 (list +timing-contract+
                       (grab (getf event :id) :move)
                       (apply #'place (getf event :id) slot))
                 nil)))
      ((and (eq type :grab) state (eql (getf event :id) id))
       (let ((x (+ (getf state :base-x) (- (getf event :x) (getf state :origin-x))))
             (y (+ (getf state :base-y) (- (getf event :y) (getf state :origin-y)))))
         (values (list* :x x :y y (loop for (key value) on state by #'cddr
                                        unless (member key '(:x :y)) append (list key value)))
                 (list +timing-contract+
                       (grab id :move)
                       (place id x y (getf state :width) (getf state :height) t))
                 nil)))
      ((eq type :button) (values nil (list +timing-contract+) nil))
      (state
       (values state
               (list +timing-contract+
                     (grab id :move)
                     (place id (getf state :x) (getf state :y)
                            (getf state :width) (getf state :height) t))
               nil))
      (t (values state
                 (append (list +timing-contract+
                               (configure-output "HEADLESS-1" :mode '(1280 720 240)))
                         (loop for window in windows for index from 0
                               collect (apply #'place (getf window :id) (timing-slot index))))
                 nil)))))
