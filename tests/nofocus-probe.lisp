(in-package #:tomoe-user)

(defparameter +nofocus-window+
  (list "tomoe-test-client" "--mode" "xdg" "--app-id" "check-input" "--title" "input"
        "--color" "0000ff" "--activate" "--seconds" "3600"))

(defparameter +nofocus-quiet-window+
  (list "tomoe-test-client" "--mode" "xdg" "--app-id" "check-quiet" "--title" "quiet"
        "--color" "00ffff" "--activate" "--seconds" "3600"))

(define-extension "nofocus-probe" (:reads (:command) :state nil) (snapshot state event)
  (declare (ignore snapshot))
  (let ((name (and (eq (getf event :type) :command) (getf event :name))))
    (values nil
            (list (define-command "focused-spawn"
                    :description "Spawn the focusing probe window."
                    :arguments (list (list :argv :argv :default +nofocus-window+)))
                  (define-command "quiet-spawn"
                    :description "Spawn the background probe window."
                    :arguments (list (list :argv :argv :default +nofocus-quiet-window+)))
                  (bind-key '(:mod) "F9" "focused-spawn")
                  (bind-key '(:mod) "F10" "quiet-spawn"))
            (cond ((equal name "focused-spawn")
                   (list (spawn (getf (getf event :arguments) :argv))))
                  ((equal name "quiet-spawn")
                   (list (spawn (getf (getf event :arguments) :argv) :focus nil)))))))
