;; A livepatch that redefines a helper a proven reducer calls. It changes nothing
;; a reducer does, but the proof was made against the definition that was loaded,
;; so every extension goes back to private copies until its source is reloaded.
(defun tomoe::purity-ids (windows)
  (mapcar (lambda (window) (getf window :id)) windows))
