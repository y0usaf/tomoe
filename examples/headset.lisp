(in-package #:tomoe-user)

(defparameter +headset-panels+
  '(("VIRTUAL-1" 2560 1440 144)
    ("VIRTUAL-2" 3840 2160 60)))

(define-extension "headset" (:reads (:connectors) :state nil) (snapshot state event)
  (declare (ignore event))
  (values state
          (append (loop for (name . mode) in +headset-panels+
                        collect (virtual-output name :mode mode))
                  (loop for connector in (context snapshot :connectors)
                        for name = (getf connector :name)
                        unless (assoc name +headset-panels+ :test #'equal)
                          collect (configure-output name :disabled t)))
          nil))
