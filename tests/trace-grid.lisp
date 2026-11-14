(in-package #:tomoe-user)

;;; Every window placed in a grid, plus a rule declaration that places the window
;;; it matches. Several windows are mounted at once, so one event that moves one
;;; window records what a layout step does while the other boxes stay put.

(defun trace--record (control &rest arguments)
  (with-open-file (stream "/run/trace.log" :direction :output :if-exists :append
                          :if-does-not-exist :create)
    (apply #'format stream control arguments)
    (terpri stream)))

(defun trace--event (event)
  (format nil "~A~@[ keys=~S~]~@[ id=~A~]~@[ fail=~A~]"
          (getf event :type) (getf event :keys) (getf event :id) (getf event :fail)))

(defun trace--slot (index)
  (list (* 160 (mod index 4)) (* 120 (truncate index 4))))

(defun trace--place (window x y)
  "Move WINDOW without resizing it, so its client never recommits because of us."
  (place (getf window :id) x y (getf window :width) (getf window :height)))

(defun trace--grid (windows event)
  (loop for window in windows
        for index from 0
        for slot = (trace--slot index)
        collect (if (and (eq (getf event :type) :button)
                         (eql (getf window :id) (getf event :id)))
                    (trace--place window (getf event :x) (getf event :y))
                    (trace--place window (first slot) (second slot)))))

(define-extension "trace-grid" (:reads (:windows :button) :state nil)
    (snapshot state event)
  (declare (ignore state))
  (let* ((windows (context snapshot :windows))
         (ids (mapcar (lambda (window) (getf window :id)) windows)))
    (trace--record "trace-grid ~A windows=~A" (trace--event event) ids)
    (values (list :ids ids)
            (append
             (trace--grid windows event)
             (list (window-rule :trace-grid
                                :app-id "trace-window"
                                :properties (list (cons :trace-grid (length ids)))
                                :reads '(:windows)
                                :state '(:seen 0)
                                :apply (lambda (window snapshot state event)
                                         (declare (ignore snapshot))
                                         (trace--record "trace-grid-apply window=~A ~A"
                                                        (getf window :id) (trace--event event))
                                         (values (list :seen (1+ (getf state :seen)))
                                                 (list (trace--place
                                                        window
                                                        (+ 400 (* 20 (getf window :id)))
                                                        300))
                                                 nil)))))
            nil)))
