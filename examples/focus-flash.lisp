(in-package #:tomoe-user)

(defparameter +flash-scale+ 23/25 "How far the newly focused window shrinks.")
(defparameter +flash-tint+ "#ffffff59" "And how far it flashes white.")
(defparameter +flash-hold-ms+ 90 "One timer separates the flash from the spring back.")

(define-extension "focus-flash" (:reads (:windows :focus)
                                 :state (list :focused nil :flashing nil))
    (snapshot state event)
  (let* ((focused (context snapshot :focus))
         (windows (mapcar (lambda (window) (getf window :id)) (context snapshot :windows)))
         (flashing (cond ((eq (getf event :type) :timer) nil)
                         ((and focused (not (eql focused (getf state :focused)))) focused)
                         (t (getf state :flashing)))))
    (values (list :focused focused :flashing flashing)
            (append (list (settings :animations
                                    '(:window-scale (:spring (:damping-ratio 11/20
                                                              :stiffness 420))
                                      :window-tint (:ease (:duration-ms 80)))))
                    (when (and flashing (member flashing windows))
                      (list (window-properties flashing
                                               :scale +flash-scale+ :tint +flash-tint+)
                            (once :release +flash-hold-ms+))))
            nil)))
