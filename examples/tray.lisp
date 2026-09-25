(in-package #:tomoe-user)

(defun tray--field (item key)
  (let ((value (and (listp item) (getf item key))))
    (and (stringp value) (plusp (length value)) value)))

(defun tray--control-character-p (character)
  (let ((code (char-code character)))
    (or (< code #x20)
        (<= #x7f code #x9f)
        (<= #x200b code #x200f)
        (<= #x202a code #x202e)
        (<= #x2060 code #x2064)
        (member code '(#xfeff #x2028 #x2029)))))

(defun tray--display (value &optional (limit 24))
  "Make a bounded, one-line display copy of VALUE."
  (when (stringp value)
    (let* ((limit (max 1 limit))
           (length (length value))
           (truncated (> length limit))
           (keep (if truncated (1- limit) limit)))
      (with-output-to-string (stream)
        (loop for index below (min length keep)
              do (write-char (if (tray--control-character-p (char value index))
                                 #\Space
                                 (char value index))
                               stream))
        (when truncated (write-char #\… stream))))))

(defun tray--title (item)
  (or (tray--display (tray--field item :title))
      (tray--display (tray--field item :id))
      (tray--display (tray--field item :service))
      (tray--display (tray--field item :path))))

(defun tray--icon-name (item)
  (let ((name (tray--field item :icon-name)))
    (and name (<= (length name) 4096)
         (notany #'tray--control-character-p name)
         name)))

(defun tray--icon (item)
  (or (tray--icon-name item)
      (tray--title item)
      "applications-system"))

(defun tray--bounded-items (service)
  "Return up to nine items and a bounded overflow label."
  (let ((items (and (listp service) (getf service :items)))
        (shown nil)
        (total 0))
    (loop while (consp items) do
      (incf total)
      (when (<= total 9) (push (car items) shown))
      (setf items (cdr items)))
    (setf shown (nreverse shown))
    (values shown
            (when (> total 9)
              (let ((remaining (- total 9)))
                (if (> remaining 999) "+999+"
                    (format nil "+~D" remaining)))))))

(defun tray--tree (items overflow)
  (ui :row :padding '(5 10 5 10) :gap 4 :radius 6
      :background "#1e1e2e"
      :children
      (append
       (mapcar (lambda (item)
                 (ui :icon :name (tray--icon item)
                     :size 18 :color "#cdd6f4"))
               items)
       (when overflow
         (list (ui :text :text overflow :size 12 :color "#cdd6f4"))))))

(define-extension "tray-indicator" (:reads (:services :outputs) :state nil)
    (snapshot state event)
  (declare (ignore state event))
  (let* ((service (service-state snapshot :tray))
         (outputs (context snapshot :outputs)))
    (multiple-value-bind (items overflow) (tray--bounded-items service)
      (values nil
              (when (and items (listp outputs) outputs)
                (list
                 (shell-surface :tray
                   (tray--tree items overflow)
                   :width 240 :height 28 :anchors '(:top :right) :margin 8
                   :layer :top :exclusive-zone 0 :background "#00000000")))
              nil))))
