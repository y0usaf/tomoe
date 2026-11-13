(in-package #:tomoe-user)

(defparameter *scenario-log* (sb-ext:posix-getenv "TOMOE_SCENARIO_LOG"))

(defun scenario-note (line)
  (when *scenario-log*
    (with-open-file (stream *scenario-log* :direction :output :if-exists :append
                                 :if-does-not-exist :create)
      (write-line line stream))))

(defparameter +scenario-commands+
  (list (define-command "mark"
          :description "Note a command run in the scenario log."
          :arguments '((:name :string :required t)))
        (define-command "snapshot"
          :description "Note a checkpoint in the scenario log."
          :arguments '((:name :string :required t)))))

(define-extension "scenario" (:reads (:command :windows :layout :focus :stacking) :state nil)
    (snapshot state event)
  (declare (ignore state))
  (let ((command (and (eq (getf event :type) :command)
                      (equal (getf event :owner) "scenario")
                      (getf event :name)))
        (arguments (getf event :arguments))
        (layout (context snapshot :layout)))
    (when (member (getf event :type) '(:command :map :unmap :mount :request))
      (scenario-note (format nil "event ~A ~A" (getf event :type)
                             (loop for window in layout
                                   collect (list (getf window :id) (getf window :x)
                                                 (getf window :y) (getf window :width)
                                                 (getf window :height)
                                                 (and (getf window :visible) t))))))
    (when command
      (scenario-note (format nil "command ~A ~A" command (getf arguments :name))))
    (values nil
            (append +scenario-commands+
                    (list (bind-key '(:mod) "F2" (invoke "mark" :name "key"))
                          (settings :animations nil)))
            nil)))
