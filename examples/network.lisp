(in-package #:tomoe-user)

(defun network--connected-p (service)
  (and (listp service) (eq (getf service :connected) t)))

(defun network--ssid (service)
  (let ((ssid (and (listp service) (getf service :ssid))))
    (and (stringp ssid) (plusp (length ssid)) ssid)))

(defun network--strength (service)
  (let ((strength (and (listp service) (getf service :strength))))
    (if (integerp strength)
        (max 0 (min 255 strength))
        0)))

(defun network--control-character-p (character)
  (let ((code (char-code character)))
    (or (< code #x20)
        (<= #x7f code #x9f)
        (<= #x200b code #x200f)
        (<= #x202a code #x202e)
        (<= #x2060 code #x2064)
        (member code '(#xfeff #x2028 #x2029)))))

(defun network--display (value &optional (limit 28))
  "Return a bounded, one-line display copy without control characters."
  (when (stringp value)
    (let* ((limit (max 1 limit))
           (length (length value))
           (truncated (> length limit))
           (keep (if truncated (1- limit) limit)))
      (with-output-to-string (stream)
        (loop for index below (min length keep)
              do (write-char (if (network--control-character-p (char value index))
                                 #\Space
                                 (char value index))
                               stream))
        (when truncated (write-char #\… stream))))))

(defun network--icon (service)
  (if (not (network--connected-p service))
      "network-offline"
      (let ((ssid (network--ssid service))
            (strength (network--strength service)))
        (if (null ssid)
            "network-wired"
            (cond ((>= strength 75) "network-wireless-signal-excellent")
                  ((>= strength 50) "network-wireless-signal-good")
                  ((>= strength 25) "network-wireless-signal-ok")
                  (t "network-wireless-signal-weak"))))))

(defun network--tree (service)
  (let ((icon (ui :icon :name (network--icon service)
                  :size 18 :color "#cdd6f4")))
    (ui :row :padding '(5 10 5 10) :gap 4 :radius 6
        :background "#1e1e2e"
        :children
        (if (not (network--connected-p service))
            (list icon)
            (list icon
                  (ui :text :text (or (network--display (network--ssid service))
                                      "Wired")
                      :size 12 :color "#cdd6f4"))))))

(define-extension "network-indicator" (:reads (:services :outputs) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((service (service-state snapshot :network))
         (outputs (context snapshot :outputs)))
    (values nil
            (when (and (listp outputs) outputs)
              (list
               (shell-surface :network
                 (network--tree service)
                 :width 240 :height 28 :anchors '(:top :right) :margin 8
                 :layer :top :exclusive-zone 0 :background "#00000000")))
            nil)))
