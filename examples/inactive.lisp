(in-package #:tomoe-user)

(defparameter +inactive-alpha+ 22/25 "Hyprland's inactive_opacity.")
(defparameter +inactive-dim+ "#00000059" "And dim_inactive.")
(defparameter +inactive-ease+ '(:ease (:duration-ms 170 :curve :ease-out-quad))
  "Both are window state, so the compositor crosses between them on focus.")

(define-extension "inactive" (:reads (:windows :focus) :state nil) (snapshot state event)
  (declare (ignore state event))
  (let ((focused (context snapshot :focus)))
    (values nil
            (cons (settings :animations (list :window-alpha +inactive-ease+
                                              :window-tint +inactive-ease+))
                  (loop for window in (context snapshot :windows)
                        unless (eql (getf window :id) focused)
                          collect (window-properties (getf window :id)
                                                     :alpha +inactive-alpha+
                                                     :tint +inactive-dim+)))
            nil)))
