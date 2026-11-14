(in-package #:tomoe-user)

(define-extension "scenario-contract"
    (:reads (:pointer) :state (list :wakes 0 :hits 0 :others 0))
    (snapshot state event)
  (declare (ignore snapshot))
  (let* ((type (getf event :type))
         (motion (and (eq type :pointer) (eq (getf event :state) :motion)))
         (other (and (eq type :pointer) (not motion)))
         (hit (and motion (or (getf event :entered) (getf event :edges-in)))))
    (when other
      (scenario-note (format nil "other ~A ~A" (getf event :state) (getf event :id))))
    (when motion
      (scenario-note (format nil "contract entered=~S left=~S in=~S out=~S distance=~A"
                             (getf event :entered) (getf event :left)
                             (getf event :edges-in) (getf event :edges-out)
                             (getf event :distance))))
    (values (list :wakes (if motion (1+ (getf state :wakes 0)) (getf state :wakes 0))
                  :hits (if hit (1+ (getf state :hits 0)) (getf state :hits 0))
                  :others (if other (1+ (getf state :others 0)) (getf state :others 0)))
            (list (motion-contract :edges '((:top 12))
                                   :regions '(("corner" 0 0 80 80))
                                   :distance 500))
            nil)))
