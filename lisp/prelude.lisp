(defvar *definitions* :not-loading)
(defvar *source*)
(defvar *source-directory*)
(defvar *runtime* nil)
(defvar *bootstrap-limits*
  (list :datum-nodes 32768 :string-chars 65536 :source 1048576 :patch 1048576
        :effects 512 :commands 32 :extensions 64 :watchdog-ms 1000)
  "The compiled-in floors, used until a commit publishes the :budgets group.")

(defun %round-away-positive (value)
  "Round a non-negative REAL away from zero at an exact half."
  (floor (+ (coerce value 'double-float) 0.5d0)))

(defparameter %nul (code-char 0))

(defun %boolean-p (value) (or (eq value t) (null value)))

(defun %lower (value)
  (string-downcase (if (stringp value) value (symbol-name value))))

(defun %alpha-p (character)
  (let ((code (char-code character)))
    (or (<= 65 code 90) (<= 97 code 122) (> code 127))))

(defun %dedupe (list &optional (test #'equal))
  (let ((result nil))
    (dolist (item list (nreverse result))
      (unless (member item result :test test) (push item result)))))

(defun %in-range-p (value low high)
  (and (integerp value) (<= low value high)))

(defun %alist-set (key value alist &optional (test #'equal))
  (let ((entry (assoc key alist :test test)))
    (if entry
        (progn (setf (cdr entry) value) alist)
        (append alist (list (cons key value))))))

(defun %alist-remove (key alist &optional (test #'equal))
  (remove key alist :test test :key #'car))

(defun %plist-remove (key plist)
  (loop for (item value) on plist by #'cddr
        unless (equal item key) append (list item value)))

(defun %error-text (thunk)
  "THUNK's error message, or NIL when it returned."
  (let ((message nil))
    (%catch thunk (lambda (text) (setf message text)))
    message))
